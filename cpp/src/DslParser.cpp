#include "graphdb/DslParser.hpp"
#include "graphdb/ValueCodec.hpp"
#include "graphdb/QueryJson.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace graphdb {
class EngineAccess {
  GraphEngine &engine_;

public:
  explicit EngineAccess(GraphEngine &engine) : engine_(engine) {}
  Status checkpoint() { return engine_.checkpoint_unlocked(); }
  Status upsert_node(std::string label, std::string id, PropertyMap properties) {
    return engine_.upsert_node_unlocked(label, id, properties);
  }
  Status upsert_edge(std::string type, std::string from, std::string to, PropertyMap properties) {
    return engine_.upsert_edge_unlocked(type, from, to, properties);
  }
  Status delete_node(std::string id) { return engine_.delete_node_unlocked(id); }
  Status delete_edge(std::string from, std::string type, std::string to) {
    return engine_.delete_edge_unlocked(from, type, to);
  }
  Result<GraphObject> get_node(const std::string &id) { return engine_.get_node_unlocked(id); }
  Result<GraphObject> get_edge(const std::string &from, const std::string &type,
                               const std::string &to) {
    return engine_.get_edge_unlocked(from, type, to);
  }
  Result<std::vector<GraphObject>> find_nodes(std::string label, std::string where_key,
                                              Property where_value, size_t limit,
                                              std::string where_op = "=") {
    return engine_.find_nodes_unlocked(label, where_key, where_value, limit, where_op);
  }
  Result<std::vector<GraphObject>> walk_out(std::string from, std::string edge_type, size_t depth,
                                            size_t limit) {
    return engine_.walk_out_unlocked(from, edge_type, depth, limit);
  }
  Result<std::vector<GraphObject>> walk_in(std::string to, std::string edge_type, size_t depth,
                                           size_t limit) {
    return engine_.walk_in_unlocked(to, edge_type, depth, limit);
  }
  Result<TraversalResult> traverse(std::string from, std::string type, size_t depth,
                                    size_t limit, bool inbound) {
    return engine_.traverse_unlocked(from, type, depth, limit, inbound, true);
  }
  Status create_node_property_index(std::string label, std::string property_name) {
    return engine_.create_node_property_index_unlocked(label, property_name);
  }
  bool has_node_property_index(const std::string &label, const std::string &property_name) const {
    return engine_.has_node_property_index_unlocked(label, property_name);
  }
  Status rebuild_indexes() { return engine_.rebuild_indexes_unlocked(); }
  QueryContext &context() const { return *engine_.active_query_; }
  size_t result_limit() const { return context().options().result_bytes; }
  QueryResult run_program(const TransactionBatch &batch,
                          const std::function<QueryResult(GraphEngine &)> &run) {
    return engine_.run_program(batch, run);
  }
};

namespace {

struct UnboundParameter : std::runtime_error {
  using std::runtime_error::runtime_error;
};

enum class TokenKind { End, Atom, String, Symbol, Arrow, Operator };

struct Token {
  TokenKind kind{TokenKind::End};
  std::string text;
  size_t start{0}, end{0};
};

std::string lower_copy(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

struct ParseFailure : std::runtime_error {
  Status status;
  ParseFailure(Status value) : std::runtime_error(value.message), status(std::move(value)) {}
};
class Lexer {
public:
  explicit Lexer(std::string input, QueryContext &context) : input_(std::move(input)), context_(context) {}

  std::vector<Token> tokenize() {
    std::vector<Token> tokens;
    while (true) {
      skip_space();
      auto start = pos_;
      context_.reserve_work(sizeof(Token)*2+64);
      Token token;
      try {
        token=next();
        context_.reserve_work(token.text.size()*8);
        (void)json_escape(token.text); // validate complete UTF-8 before tokens reach output
      }
      catch(const QueryFailure &) {throw;}
      catch(const std::exception &e) {
        auto status=Status::Error(e.what(),ErrorCode::parseError);
        status.context.source_start=start;status.context.source_end=pos_;
        throw ParseFailure(std::move(status));
      }
      token.start = start; token.end = pos_;
      tokens.push_back(std::move(token));
      if (tokens.back().kind == TokenKind::End)
        break;
    }
    return tokens;
  }

private:
  Token next() {
    skip_space();
    if (pos_ >= input_.size())
      return {TokenKind::End, {}};
    char c = input_[pos_];

    if (c == '-' && pos_ + 1 < input_.size() && input_[pos_ + 1] == '>') {
      pos_ += 2;
      return {TokenKind::Arrow, "->"};
    }

    if (c == '!' && pos_ + 1 < input_.size() && input_[pos_ + 1] == '=') {
      pos_ += 2;
      return {TokenKind::Operator, "!="};
    }
    if (c == '=' && pos_ + 1 < input_.size() && input_[pos_ + 1] == '=') {
      pos_ += 2;
      return {TokenKind::Operator, "=="};
    }
    if (c == '<' && pos_ + 1 < input_.size() && input_[pos_ + 1] == '=') {
      pos_ += 2;
      return {TokenKind::Operator, "<="};
    }
    if (c == '>' && pos_ + 1 < input_.size() && input_[pos_ + 1] == '=') {
      pos_ += 2;
      return {TokenKind::Operator, ">="};
    }
    if (c == '<') {
      ++pos_;
      return {TokenKind::Operator, "<"};
    }
    if (c == '>') {
      ++pos_;
      return {TokenKind::Operator, ">"};
    }

    if (c == '"' || c == '\'')
      return quoted();

    if (c == '$') {
      ++pos_;
      auto start = pos_;
      while (pos_ < input_.size()) {
        char ch = input_[pos_];
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_')
          break;
        ++pos_;
      }
      if (start == pos_)
        throw std::runtime_error("expected identifier after $");
      return {TokenKind::Atom, "$" + input_.substr(start, pos_ - start)};
    }

    if (is_symbol(c)) {
      ++pos_;
      return {TokenKind::Symbol, std::string(1, c)};
    }

    auto start = pos_;
    while (pos_ < input_.size()) {
      char ch = input_[pos_];
      if (std::isspace(static_cast<unsigned char>(ch)) || input_.compare(pos_, 2, "//") == 0)
        break;
      if (ch == '-' && pos_ + 1 < input_.size() && input_[pos_ + 1] == '>')
        break;
      if (is_symbol(ch) || ch == '"' || ch == '\'' || ch == '<' || ch == '>' || ch == '!')
        break;
      ++pos_;
    }
    if (start == pos_)
      throw std::runtime_error(std::string("unexpected character: ") + c);
    return {TokenKind::Atom, input_.substr(start, pos_ - start)};
  }

  Token quoted() {
    char quote = input_[pos_++];
    std::string value;
    while (pos_ < input_.size()) {
      char c = input_[pos_++];
      if (c == quote)
        return {TokenKind::String, value};
      if (c == '\\' && pos_ < input_.size()) {
        char escaped = input_[pos_++];
        switch (escaped) {
        case 'n':
          value.push_back('\n');
          break;
        case 'r':
          value.push_back('\r');
          break;
        case 't':
          value.push_back('\t');
          break;
        case '\\':
          value.push_back('\\');
          break;
        case '"':
          value.push_back('"');
          break;
        case '\'':
          value.push_back('\'');
          break;
        default:
          throw std::runtime_error("unsupported string escape");
        }
      } else {
        value.push_back(c);
      }
    }
    throw std::runtime_error("unterminated string literal");
  }

  static bool is_symbol(char c) {
    switch (c) {
    case ',':
    case '=':
    case '(':
    case ')':
    case ';':
    case '{':
    case '}':
      return true;
    default:
      return false;
    }
  }

  void skip_space() {
    for (;;) {
      while (pos_ < input_.size() && std::isspace(static_cast<unsigned char>(input_[pos_]))) ++pos_;
      if (input_.compare(pos_, 2, "//") != 0) break;
      while (pos_ < input_.size() && input_[pos_] != '\n') ++pos_;
    }
  }

  std::string input_;
  QueryContext &context_;
  size_t pos_{0};
};

enum class StatementKind {
  CreateNode,
  CreateEdge,
  GetNode,
  GetEdge,
  FindNodes,
  Walk,
  CreateIndex,
  DeleteNode,
  DeleteEdge,
  Checkpoint,
  RebuildIndexes
};

struct Statement {
  StatementKind kind{StatementKind::GetNode};
  bool is_explain{false};
  bool include_paths{false};
  std::string label;
  std::string id;
  std::string type;
  std::string from;
  std::string to;
  std::string direction{"out"};
  PropertyMap properties;
  std::string where_key;
  std::string where_op{"="};
  Property where_value;
  bool has_where{false};
  size_t depth{1};
  size_t limit{100};
  std::string index_property;
  std::vector<std::string> projection;
};

class Parser {
public:
  explicit Parser(std::string statement, const PropertyMap &parameters, size_t index, size_t offset, QueryContext &context)
      : parameters_(parameters), index_(index), offset_(offset), context_(context) {
    try { tokens_ = Lexer(statement,context).tokenize(); }
    catch(const QueryFailure &) { throw; }
    catch(const ParseFailure &e) { fail(e.what(),e.status.code,*e.status.context.source_start,*e.status.context.source_end); }
    catch (const std::exception &e) { fail(e.what(), ErrorCode::parseError, 0, statement.size()); }
  }

  Statement parse() {
    try { return parse_checked(); }
    catch (const QueryFailure &) { throw; }
    catch (const ParseFailure &) { throw; }
    catch (const UnboundParameter &e) { fail(e.what(), ErrorCode::unboundParameter); }
    catch (const std::exception &e) { fail(e.what(), ErrorCode::parseError); }
  }
private:
  [[noreturn]] void fail(std::string message, ErrorCode code, size_t start, size_t end) {
    auto status = Status::Error("statement " + std::to_string(index_) + ": " + message, code);
    status.context.statement_index=index_; status.context.source_start=offset_+start; status.context.source_end=offset_+end;
    throw ParseFailure(std::move(status));
  }
  [[noreturn]] void fail(std::string message, ErrorCode code) {
    const auto &token=tokens_[error_pos_]; fail(std::move(message),code,token.start,token.end);
  }
  Statement parse_checked() {
    if (match_word("explain")) {
      auto stmt = parse_inner();
      stmt.is_explain = true;
      return stmt;
    }
    return parse_inner();
  }

private:
  Statement parse_inner() {
    if (match_word("create")) {
      if (match_word("index"))
        return parse_create_index();
      if (match_word("node"))
        return parse_create_node();
      if (match_word("edge"))
        return parse_create_edge();
      throw std::runtime_error("expected node, edge, or index after create");
    }
    if (match_word("upsert")) {
      if (match_word("node"))
        return parse_create_node();
      if (match_word("edge"))
        return parse_create_edge();
      throw std::runtime_error("expected node or edge after upsert");
    }
    if (match_word("delete")) {
      if (match_word("node"))
        return parse_delete_node();
      if (match_word("edge"))
        return parse_delete_edge();
      throw std::runtime_error("expected node or edge after delete");
    }
    if (match_word("rebuild")) {
      if (match_word("index") || match_word("indexes")) {
        expect_end();
        Statement stmt;
        stmt.kind = StatementKind::RebuildIndexes;
        return stmt;
      }
      throw std::runtime_error("expected index or indexes after rebuild");
    }
    if (match_word("get"))
      return parse_get();
    if (match_word("find"))
      return parse_find();
    if (match_word("walk"))
      return parse_walk();
    if (match_word("checkpoint")) {
      expect_end();
      Statement stmt;
      stmt.kind = StatementKind::Checkpoint;
      return stmt;
    }
    throw std::runtime_error(
        "expected create, upsert, delete, get, find, walk, rebuild, or checkpoint");
  }

  Statement parse_create_node() {
    Statement stmt;
    stmt.kind = StatementKind::CreateNode;
    stmt.label = expect_atom("node label");
    stmt.id = expect_atom("node id");
    if (match_word("set"))
      stmt.properties = parse_properties();
    expect_end();
    return stmt;
  }

  Statement parse_create_edge() {
    Statement stmt;
    stmt.kind = StatementKind::CreateEdge;
    stmt.type = expect_atom("edge type");
    stmt.from = expect_atom("edge source id");
    expect_arrow();
    stmt.to = expect_atom("edge destination id");
    if (match_word("set"))
      stmt.properties = parse_properties();
    expect_end();
    return stmt;
  }

  Statement parse_delete_node() {
    Statement stmt;
    stmt.kind = StatementKind::DeleteNode;
    stmt.id = expect_atom("node id");
    expect_end();
    return stmt;
  }

  Statement parse_delete_edge() {
    Statement stmt;
    stmt.kind = StatementKind::DeleteEdge;
    stmt.type = expect_atom("edge type");
    stmt.from = expect_atom("edge source id");
    expect_arrow();
    stmt.to = expect_atom("edge destination id");
    expect_end();
    return stmt;
  }

  Statement parse_get() {
    if (match_word("node")) {
      Statement stmt;
      stmt.kind = StatementKind::GetNode;
      stmt.id = expect_atom("node id");
      if (match_word("return"))
        stmt.projection = parse_projection();
      expect_end();
      return stmt;
    }
    if (match_word("edge")) {
      Statement stmt;
      stmt.kind = StatementKind::GetEdge;
      stmt.type = expect_atom("edge type");
      stmt.from = expect_atom("edge source id");
      expect_arrow();
      stmt.to = expect_atom("edge destination id");
      if (match_word("return"))
        stmt.projection = parse_projection();
      expect_end();
      return stmt;
    }
    throw std::runtime_error("expected node or edge after get");
  }

  Statement parse_find() {
    Statement stmt;
    stmt.kind = StatementKind::FindNodes;
    stmt.limit=std::min(size_t(100),context_.options().max_results);
    if (!match_word("nodes"))
      throw std::runtime_error("expected nodes after find");
    stmt.label = expect_atom("node label");
    std::unordered_set<std::string> clauses;
    while (!is_end()) {
      error_pos_=pos_;
      if (!clauses.insert(lower_copy(peek().text)).second) throw std::runtime_error("duplicate find clause");
      if (match_word("where")) {
        stmt.where_key = expect_atom("property name");
        stmt.where_op = expect_operator();
        stmt.where_value = parse_value();
        stmt.has_where = true;
      } else if (match_word("return")) {
        stmt.projection = parse_projection();
      } else if (match_word("limit")) {
        stmt.limit = parse_size("limit");
      } else {
        throw std::runtime_error("unexpected token in find statement: " + peek().text);
      }
    }
    return stmt;
  }

  Statement parse_walk() {
    Statement stmt;
    stmt.kind = StatementKind::Walk;
    stmt.limit=std::min(size_t(100),context_.options().max_results);
    stmt.depth=std::min(size_t(1),context_.options().max_depth);
    if (!match_word("from"))
      throw std::runtime_error("expected from after walk");
    stmt.from = expect_atom("start node id");
    std::unordered_set<std::string> clauses;
    while (!is_end()) {
      error_pos_=pos_;
      if (!clauses.insert(lower_copy(peek().text)).second) throw std::runtime_error("duplicate walk clause");
      if (match_word("over")) {
        stmt.type = expect_atom("edge type");
      } else if (match_word("direction")) {
        stmt.direction = lower_copy(expect_atom("direction (out or in)"));
        if (stmt.direction != "in" && stmt.direction != "out") throw std::runtime_error("direction must be out or in");
      } else if (match_word("depth")) {
        stmt.depth = parse_size("depth");
      } else if (match_word("limit")) {
        stmt.limit = parse_size("limit");
      } else if (match_word("paths")) {
        stmt.include_paths = true;
      } else if (match_word("return")) {
        stmt.projection = parse_projection();
      } else {
        throw std::runtime_error("unexpected token in walk statement: " + peek().text);
      }
    }
    return stmt;
  }

  Statement parse_create_index() {
    Statement stmt;
    stmt.kind = StatementKind::CreateIndex;
    if (!match_word("on"))
      throw std::runtime_error("expected on after create index");
    stmt.label = expect_atom("node label");
    expect_symbol("(");
    stmt.index_property = expect_atom("property name");
    expect_symbol(")");
    expect_end();
    return stmt;
  }

  std::vector<std::string> parse_projection() {
    std::vector<std::string> proj;
    bool braced=match_symbol("{");
    do { proj.push_back(expect_atom("projection field")); } while (match_symbol(","));
    if (braced) expect_symbol("}");
    return proj;
  }

  std::string expect_operator() {
    error_pos_=pos_;
    if (peek().kind == TokenKind::Operator) {
      return advance().text;
    }
    if (match_symbol("="))
      return "=";
    throw std::runtime_error("expected comparison operator (=, ==, !=, <, <=, >, >=)");
  }

  PropertyMap parse_properties() {
    PropertyMap props;
    do {
      auto key = expect_atom("property name");
      bool has_separator = false;
      if (match_symbol("=") || match_symbol(":")) {
        has_separator = true;
      } else if (!key.empty() && key.back() == ':') {
        key.pop_back();
        has_separator = true;
      }
      if (!has_separator)
        throw std::runtime_error("expected = or : after property name");
      if (key.empty())
        throw std::runtime_error("property name must not be empty");
      if (props.contains(key)) throw std::runtime_error("duplicate property name");
      props.emplace(std::move(key), parse_value());
    } while (match_symbol(","));
    return props;
  }

  Property parse_value() {
    auto token = advance();
    if (token.kind == TokenKind::String)
      return token.text;
    if (token.kind == TokenKind::Atom) {
      if (token.kind == TokenKind::Atom && token.text.size() > 1 && token.text[0] == '$') {
        std::string param_name = token.text.substr(1);
        auto it = parameters_.find(param_name);
        if (it == parameters_.end()) {
          throw UnboundParameter("unbound query parameter: $" + param_name);
        }
        size_t bytes=128;
        if(auto value=std::get_if<std::string>(&it->second)) bytes+=value->size()*2;
        context_.reserve_work(bytes);
        return it->second;
      }
      return parse_scalar(token.text);
    }
    throw std::runtime_error("expected scalar or parameter value");
  }

  size_t parse_size(const std::string &name) {
    auto token=advance();
    if (token.kind!=TokenKind::Atom) throw std::runtime_error("expected unsigned integer " + name);
    if (token.text.starts_with("$")) {
      auto it=parameters_.find(token.text.substr(1));
      if(it==parameters_.end()) throw UnboundParameter("unbound query parameter: " + token.text);
      auto n=std::get_if<int64_t>(&it->second);
      if(!n || *n<0) throw std::runtime_error("size parameter must be a nonnegative Int64");
      if(static_cast<uint64_t>(*n)>std::numeric_limits<size_t>::max())
        throw std::runtime_error("size parameter exceeds platform range");
      return static_cast<size_t>(*n);
    }
    size_t n=0;
    auto [end,error]=std::from_chars(token.text.data(),token.text.data()+token.text.size(),n);
    if(error!=std::errc{} || end!=token.text.data()+token.text.size() || token.text.empty())
      throw std::runtime_error("invalid unsigned integer " + name);
    return n;
  }

  bool match_word(const std::string &word) {
    error_pos_=pos_;
    if (peek().kind != TokenKind::Atom)
      return false;
    if (lower_copy(peek().text) != word)
      return false;
    ++pos_;
    return true;
  }

  bool match_symbol(const std::string &symbol) {
    if (peek().kind != TokenKind::Symbol || peek().text != symbol)
      return false;
    ++pos_;
    return true;
  }

  void expect_symbol(const std::string &symbol) {
    error_pos_=pos_;
    if (!match_symbol(symbol))
      throw std::runtime_error("expected `" + symbol + "`");
  }

  void expect_arrow() {
    error_pos_=pos_;
    if (peek().kind != TokenKind::Arrow)
      throw std::runtime_error("expected ->");
    ++pos_;
  }

  std::string expect_atom(const std::string &name) {
    auto token = advance();
    if (token.kind == TokenKind::Atom && token.text.size() > 1 && token.text[0] == '$') {
      std::string param_name = token.text.substr(1);
      auto it = parameters_.find(param_name);
      if (it != parameters_.end()) {
        if (auto value=std::get_if<std::string>(&it->second)) context_.reserve_work(value->size()*2+64);
        return property_to_string(it->second);
      }
      throw UnboundParameter("unbound query parameter: $" + param_name);
    }
    if (token.kind == TokenKind::Atom || token.kind == TokenKind::String)
      return token.text;
    throw std::runtime_error("expected " + name);
  }

  void expect_end() {
    error_pos_=pos_;
    if (!is_end())
      throw std::runtime_error("unexpected token: " + peek().text);
  }

  const Token &peek() const { return tokens_[pos_]; }

  Token advance() {
    error_pos_=pos_;
    auto token = peek();
    if (token.kind != TokenKind::End)
      ++pos_;
    return token;
  }

  bool is_end() const { return peek().kind == TokenKind::End; }

  std::vector<Token> tokens_;
  const PropertyMap &parameters_;
  size_t pos_{0}, error_pos_{0}, index_{1}, offset_{0};
  QueryContext &context_;
};

struct StatementText { std::string text; size_t offset; };
std::vector<StatementText> split_statements(const std::string &input) {
  std::vector<StatementText> out;
  size_t start=0; char quote=0; bool escape=false, comment=false, content=false;
  auto finish=[&](size_t end) {
    if (content) {
      if(out.size()>=kMaxBatchStatements) throw ParseFailure(Status::Error("too many statements",ErrorCode::limitExceeded));
      out.push_back({input.substr(start,end-start),start});
    }
    start=end+1; content=false;
  };
  for(size_t i=0;i<input.size();++i) {
    char c=input[i];
    if(comment) { if(c=='\n')comment=false; continue; }
    if(escape) {escape=false;continue;}
    if(quote) {if(c=='\\')escape=true;else if(c==quote)quote=0;continue;}
    if(c=='/' && i+1<input.size() && input[i+1]=='/') {comment=true;++i;continue;}
    if(c=='\"' || c=='\'') {quote=c;content=true;continue;}
    if(c==';') {finish(i);continue;}
    if(!std::isspace(static_cast<unsigned char>(c)))content=true;
  }
  finish(input.size());return out;
}

static std::string vector_to_json(const std::vector<GraphObject> &objects,
    const std::vector<std::string> &projection, QueryContext &context) {
  QueryJson json(context);json.append("[");bool first=true;
  for(const auto &obj:objects) {if(!first)json.append(",");first=false;json.object(obj,projection);}
  json.append("]");return json.take();
}
static std::string bounded_object_json(const GraphObject &object,
    const std::vector<std::string> &projection, QueryContext &context) {
  QueryJson json(context);json.object(object,projection);return json.take();
}

QueryResult execute_statement(EngineAccess &engine, const Statement &stmt) {
  if (stmt.is_explain) {
    auto bytes=stmt.label.size()+stmt.id.size()+stmt.type.size()+stmt.from.size()+stmt.to.size()+stmt.where_key.size()+stmt.index_property.size();
    if(auto value=std::get_if<std::string>(&stmt.where_value))bytes+=value->size();
    engine.context().reserve_work(bytes*16+4096);
    std::string json = "{\"explain\":true";
    switch (stmt.kind) {
    case StatementKind::FindNodes: {
      bool has_idx = !stmt.where_key.empty() && (stmt.where_op == "=" || stmt.where_op == "==") &&
                     engine.has_node_property_index(stmt.label, stmt.where_key);
      json += ",\"operation\":\"FindNodes\",\"label\":\"" + json_escape(stmt.label) + "\"";
      json += ",\"access_path\":\"" +
              std::string(has_idx ? "PropertyIndexScan"
                                  : (stmt.has_where ? "LabelScanWithFilter" : "LabelScan")) +
              "\"";
      json += ",\"index_used\":\"" +
              std::string(has_idx ? stmt.label + "(" + stmt.where_key + ")" : "none") + "\"";
      if (stmt.has_where) {
        json += ",\"filter_key\":\"" + json_escape(stmt.where_key) + "\"";
        json += ",\"filter_operator\":\"" + json_escape(stmt.where_op) + "\"";
        json += ",\"filter_value\":" + property_to_json(stmt.where_value);
      }
      json += ",\"limit\":" + std::to_string(stmt.limit);
      break;
    }
    case StatementKind::Walk: {
      json += ",\"operation\":\"Walk\",\"from\":\"" + json_escape(stmt.from) + "\"";
      json += ",\"edge_type\":\"" + json_escape(stmt.type) + "\"";
      json += ",\"direction\":\"" + json_escape(stmt.direction) + "\"";
      json +=
          ",\"access_path\":\"" +
          std::string(stmt.direction == "in" ? "InboundAdjacencyIndex" : "OutboundAdjacencyIndex") +
          "\"";
      json += ",\"depth\":" + std::to_string(stmt.depth);
      json += ",\"limit\":" + std::to_string(stmt.limit);
      json += ",\"algorithm\":\"BreadthFirst\",\"tie_break\":\"canonical_edge_key\"";
      json += ",\"result_mode\":\"" + std::string(stmt.include_paths ? "paths" : "nodes") + "\"";
      if (stmt.include_paths) json += ",\"path_semantics\":\"one_shortest_hop\"";
      break;
    }
    case StatementKind::GetNode: {
      json += ",\"operation\":\"GetNode\",\"id\":\"" + json_escape(stmt.id) + "\"";
      json += ",\"access_path\":\"PrimaryIndexLookup\"";
      break;
    }
    case StatementKind::GetEdge: {
      json += ",\"operation\":\"GetEdge\",\"from\":\"" + json_escape(stmt.from) + "\"";
      json +=
          ",\"type\":\"" + json_escape(stmt.type) + "\",\"to\":\"" + json_escape(stmt.to) + "\"";
      json += ",\"access_path\":\"PrimaryIndexLookup\"";
      break;
    }
    case StatementKind::CreateIndex: {
      json += ",\"operation\":\"CreateIndex\",\"label\":\"" + json_escape(stmt.label) + "\"";
      json += ",\"property\":\"" + json_escape(stmt.index_property) + "\"";
      break;
    }
    case StatementKind::RebuildIndexes: {
      json += ",\"operation\":\"RebuildIndexes\"";
      break;
    }
    case StatementKind::Checkpoint: {
      json += ",\"operation\":\"Checkpoint\"";
      break;
    }
    default:
      json += ",\"operation\":\"Mutation\"";
      break;
    }
    json += "}";
    engine.context().reserve_result(json.size());
    return {true, std::move(json), {}};
  }

  switch (stmt.kind) {
  case StatementKind::CreateNode: {
    auto st = engine.upsert_node(stmt.label, stmt.id, stmt.properties);
    if (!st.ok)
      return {false, {}, st.message, st};
    return {true, "{\"ok\":true,\"mutation\":\"upsert_node\"}", {}};
  }
  case StatementKind::CreateEdge: {
    auto st = engine.upsert_edge(stmt.type, stmt.from, stmt.to, stmt.properties);
    if (!st.ok)
      return {false, {}, st.message, st};
    return {true, "{\"ok\":true,\"mutation\":\"upsert_edge\"}", {}};
  }
  case StatementKind::DeleteNode: {
    auto st = engine.delete_node(stmt.id);
    if (!st.ok)
      return {false, {}, st.message, st};
    return {true, "{\"ok\":true,\"mutation\":\"delete_node\"}", {}};
  }
  case StatementKind::DeleteEdge: {
    auto st = engine.delete_edge(stmt.from, stmt.type, stmt.to);
    if (!st.ok)
      return {false, {}, st.message, st};
    return {true, "{\"ok\":true,\"mutation\":\"delete_edge\"}", {}};
  }
  case StatementKind::GetNode: {
    auto res = engine.get_node(stmt.id);
    if (!res)
      return {false, {}, res.status.message, res.status};
    return {true, bounded_object_json(res.value, stmt.projection,engine.context()), {}};
  }
  case StatementKind::GetEdge: {
    auto res = engine.get_edge(stmt.from, stmt.type, stmt.to);
    if (!res)
      return {false, {}, res.status.message, res.status};
    return {true, bounded_object_json(res.value, stmt.projection,engine.context()), {}};
  }
  case StatementKind::FindNodes: {
    auto res = engine.find_nodes(stmt.label, stmt.has_where ? stmt.where_key : std::string{},
                                 stmt.where_value, stmt.limit, stmt.where_op);
    if (!res)
      return {false, {}, res.status.message, res.status};
    return {true, vector_to_json(res.value, stmt.projection,engine.context()), {}};
  }
  case StatementKind::Walk: {
    if (stmt.include_paths) {
      auto result = engine.traverse(stmt.from, stmt.type, stmt.depth, stmt.limit, stmt.direction == "in");
      if (!result) return {false, {}, result.status.message, result.status};
      QueryJson json(engine.context());
      json.traversal(result.value, stmt.projection);
      return {true, json.take(), {}};
    }
    auto res = (stmt.direction == "in")
                   ? engine.walk_in(stmt.from, stmt.type, stmt.depth, stmt.limit)
                   : engine.walk_out(stmt.from, stmt.type, stmt.depth, stmt.limit);
    if (!res)
      return {false, {}, res.status.message, res.status};
    return {true, vector_to_json(res.value, stmt.projection,engine.context()), {}};
  }
  case StatementKind::CreateIndex: {
    auto st = engine.create_node_property_index(stmt.label, stmt.index_property);
    if (!st.ok)
      return {false, {}, st.message, st};
    return {true,
            "{\"ok\":true,\"mutation\":\"create_index\",\"label\":\"" + json_escape(stmt.label) +
                "\",\"property\":\"" + json_escape(stmt.index_property) + "\"}",
            {}};
  }
  case StatementKind::RebuildIndexes: {
    auto st = engine.rebuild_indexes();
    if (!st.ok)
      return {false, {}, st.message, st};
    return {true, "{\"ok\":true,\"mutation\":\"rebuild_indexes\"}", {}};
  }
  case StatementKind::Checkpoint: {
    auto st = engine.checkpoint();
    if (!st.ok)
      return {false, {}, st.message, st};
    return {true, "{\"ok\":true,\"mutation\":\"checkpoint\"}", {}};
  }
  }
  return {false, {}, "unknown statement kind"};
}

} // namespace

PropertyMap parse_property_list(std::string input) { return parse_json_properties(input); }

QueryResult execute_ngql_unlocked(GraphEngine &engine, std::string query,
                                  const PropertyMap &parameters) {
  if (query.size()>kMaxStringBytes)
    return {false, {}, "query exceeds 1 MiB", Status::Error("query exceeds 1 MiB", ErrorCode::limitExceeded)};
  if (query.empty())
    return {false, {}, "NGQL parse error: empty query"};

  size_t statement_index=0, statement_start=0, statement_end=0;
  try {
    auto &context=EngineAccess(engine).context();
    context.enforce();
    context.reserve_work(query.size()*2);
    size_t parameter_bytes=0;
    for(const auto &[key,value]:parameters) {
      context.enforce();
      size_t bytes=key.size()+32;
      if(auto text=std::get_if<std::string>(&value))bytes+=text->size();
      if(bytes>kMaxStringBytes-parameter_bytes)throw QueryFailure(Status::Error("parameters exceed 1 MiB",ErrorCode::limitExceeded));
      parameter_bytes+=bytes; context.reserve_work(bytes*16+256);
    }
    validate_properties(parameters);
    auto statements = split_statements(query);
    if (statements.empty())
      return {false, {}, "NGQL parse error: empty query"};

    if (statements.size() > context.options().max_statements)
      return {false,
              {},
              "program exceeds 1000 statements",
              Status::Error("program exceeds 1000 statements", ErrorCode::limitExceeded)};
    std::vector<Statement> parsed;
    TransactionBatch batch{new_transaction_id(), {}};
    bool maintenance = false;
    size_t batch_bytes=0;
    for (const auto &text : statements) {
      statement_index=parsed.size()+1;statement_start=text.offset;statement_end=text.offset+text.text.size();
      context.enforce();
      context.reserve_work(sizeof(Statement)*2+text.text.size()*4);
      auto stmt = Parser(text.text, parameters, parsed.size()+1, text.offset,context).parse();
      context.bounds(stmt.kind==StatementKind::Walk ? stmt.depth : 0,
                     stmt.kind==StatementKind::Walk || stmt.kind==StatementKind::FindNodes ? stmt.limit : 0);
      if(context.read_only && !stmt.is_explain && stmt.kind!=StatementKind::GetNode &&
         stmt.kind!=StatementKind::GetEdge && stmt.kind!=StatementKind::FindNodes && stmt.kind!=StatementKind::Walk)
        throw QueryFailure(Status::Error("only reads are allowed in a snapshot",ErrorCode::conflict));
      if(!stmt.is_explain && (stmt.kind==StatementKind::CreateNode || stmt.kind==StatementKind::CreateEdge || stmt.kind==StatementKind::DeleteNode || stmt.kind==StatementKind::DeleteEdge || stmt.kind==StatementKind::CreateIndex)) {
        size_t bytes=64+stmt.label.size()+stmt.id.size()+stmt.type.size()+stmt.from.size()+stmt.to.size()+stmt.index_property.size();
        for(const auto &[key,value]:stmt.properties) {bytes+=key.size()+16; if(auto text=std::get_if<std::string>(&value))bytes+=text->size();}
        if(bytes>context.options().batch_bytes-batch_bytes)throw QueryFailure(Status::Error("batch byte budget exceeded",ErrorCode::limitExceeded));
        batch_bytes+=bytes; context.reserve_work(bytes*4+stmt.properties.size()*256);
      }
      if (!stmt.is_explain)
        switch (stmt.kind) {
        case StatementKind::CreateNode:
          batch.mutations.emplace_back(UpsertNode{stmt.label, stmt.id, stmt.properties});
          break;
        case StatementKind::CreateEdge:
          batch.mutations.emplace_back(UpsertEdge{stmt.type, stmt.from, stmt.to, stmt.properties});
          break;
        case StatementKind::DeleteNode:
          batch.mutations.emplace_back(DeleteNode{stmt.id});
          break;
        case StatementKind::DeleteEdge:
          batch.mutations.emplace_back(DeleteEdge{stmt.from, stmt.type, stmt.to});
          break;
        case StatementKind::CreateIndex:
          batch.mutations.emplace_back(CreateIndex{stmt.label, stmt.index_property});
          break;
        case StatementKind::Checkpoint:
        case StatementKind::RebuildIndexes:
          maintenance = true;
          break;
        default:
          break;
        }
      parsed.push_back(std::move(stmt));
    }
    if (maintenance && !batch.mutations.empty())
      return {false,
              {},
              "maintenance cannot run inside a mutating program",
              Status::Error("maintenance cannot run inside a mutating program",
                            ErrorCode::invalidArgument)};
    EngineAccess access(engine);
    return access.run_program(batch, [&](GraphEngine &target) -> QueryResult {
      EngineAccess view(target);
      std::vector<std::string> results;
      results.reserve(parsed.size());
      auto &context=view.context();
      if(parsed.size()>1)context.reserve_result(14+parsed.size()-1);
      context.reserve_work(parsed.size()*sizeof(std::string)*2);
      size_t result_bytes=0;
      for (size_t i = 0; i < parsed.size(); ++i) {
        statement_index=i+1;statement_start=statements[i].offset;statement_end=statement_start+statements[i].text.size();
        context.enforce();
        auto result = execute_statement(view, parsed[i]);
        if (!result.ok) {
          result.status.context.statement_index=i+1;result.status.context.source_start=statement_start;result.status.context.source_end=statement_end;
          return {false, {}, "statement " + std::to_string(i + 1) + ": " + result.error, result.status};
        }
        if(!parsed[i].is_explain && parsed[i].kind!=StatementKind::GetNode && parsed[i].kind!=StatementKind::GetEdge && parsed[i].kind!=StatementKind::FindNodes && parsed[i].kind!=StatementKind::Walk) {
          context.reserve_result(result.json.size()); context.reserve_work(result.json.size()*2);
        }
        result_bytes += result.json.size();
        results.push_back(std::move(result.json));
      }
      if (results.size() == 1)
        return {true, std::move(results.front()), {}};
      context.reserve_work(result_bytes*2+32);
      std::string json = "{\"results\":[";
      for (size_t i = 0; i < results.size(); ++i) {
        if (i)
          json += ",";
        json += results[i];
      }
      json += "]}";
      return {true, std::move(json), {}};
    });
  } catch (const QueryFailure &e) {
    auto status=e.status;
    if(statement_index) {status.context.statement_index=statement_index;status.context.source_start=statement_start;status.context.source_end=statement_end;}
    return {false, {}, e.what(), status};
  } catch (const ParseFailure &e) {
    return {false, {}, e.what(), e.status};
  } catch (const std::bad_alloc &) {
    return {false,
            {},
            "query allocation failed",
            Status::Error("query allocation failed", ErrorCode::limitExceeded)};
  } catch (const UnboundParameter &e) {
    return {false, {}, e.what(), Status::Error(e.what(), ErrorCode::unboundParameter)};
  } catch (const std::exception &e) {
    return {false, {}, std::string("NGQL parse/execution error: ") + e.what()};
  }
}

QueryResult execute_ngql(GraphEngine &engine, std::string query, const PropertyMap &parameters) {
  return engine.execute_dsl(std::move(query), parameters);
}
} // namespace graphdb
