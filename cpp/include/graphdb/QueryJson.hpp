#pragma once
#include "graphdb/GraphTypes.hpp"
#include "graphdb/QueryContext.hpp"
namespace graphdb {
// Charges every append before growing output; strings are escaped without a full-size temporary.
class QueryJson {
  QueryContext &context_;
  std::string output_;
  bool count_only_;
public:
  explicit QueryJson(QueryContext &context, bool count_only = false)
      : context_(context), count_only_(count_only) {}
  void append(std::string_view bytes) {
    context_.reserve_result(bytes.size());
    if (!count_only_) {
      context_.reserve_work(bytes.size()*2);
      output_.append(bytes);
    }
  }
  void quoted(std::string_view value) {
    append("\"");
    size_t start=0;
    for(size_t i=0;i<value.size();++i) {
      if((i&255)==0)context_.enforce();
      unsigned char ch=value[i];
      if(ch>=32 && ch!='"' && ch!='\\')continue;
      append(value.substr(start,i-start)); start=i+1;
      switch(ch) {
        case '"':append("\\\"");break; case '\\':append("\\\\");break;
        case '\n':append("\\n");break; case '\r':append("\\r");break;
        case '\t':append("\\t");break; case '\b':append("\\b");break; case '\f':append("\\f");break;
        default: {char escaped[]={'\\','u','0','0',"0123456789abcdef"[ch>>4],"0123456789abcdef"[ch&15]}; append({escaped,6});}
      }
    }
    append(value.substr(start)); append("\"");
  }
  void value(const Property &property) {
    if (context_.tagged_values) {
      static constexpr const char *types[] = {"null", "bool", "int", "double", "string"};
      append("{\"type\":"); quoted(types[property.index()]); append(",\"value\":");
    }
    if(auto text=std::get_if<std::string>(&property))quoted(*text);
    else append(property_to_json(property));
    if (context_.tagged_values) append("}");
  }
  void object(const GraphObject &obj, const std::vector<std::string> &projection) {
    append("{"); bool first=true;
    auto key=[&](std::string_view name) {if(!first)append(",");first=false;quoted(name);append(":");};
    if(projection.empty()) {
      key("kind");quoted(kind_to_string(obj.kind)); key("id");quoted(obj.id);
      key(obj.kind==ObjectKind::Node ? "label" : "type");quoted(obj.label_or_type);
      if(obj.kind==ObjectKind::Edge) {key("from");quoted(obj.from);key("to");quoted(obj.to);}
      key("properties");append("{");bool property_first=true;
      for(const auto &[name,v]:obj.properties) {if(!property_first)append(",");property_first=false;quoted(name);append(":");value(v);}
      append("}"); key("last_read_ms");append(std::to_string(obj.last_read_ms));
      key("last_modified_ms");append(std::to_string(obj.last_modified_ms));key("version");append(std::to_string(obj.version));
    } else for(const auto &field:projection) {
      key(field);
      if(field=="id")quoted(obj.id);
      else if(field=="label" || field=="type")quoted(obj.label_or_type);
      else if(field=="from")quoted(obj.from);
      else if(field=="to")quoted(obj.to);
      else if(field=="kind")quoted(kind_to_string(obj.kind));
      else if(field=="version")append(std::to_string(obj.version));
      else {auto found=obj.properties.find(field);if(found==obj.properties.end())append("null");else value(found->second);}
    }
    append("}");
  }
  void objects(const std::vector<GraphObject> &values, const std::vector<std::string> &projection) {
    append("[");
    bool first = true;
    for (const auto &value : values) {
      if (!first) append(",");
      first = false;
      object(value, projection);
    }
    append("]");
  }
  void traversal(const TraversalResult &result, const std::vector<std::string> &projection = {}) {
    append("{\"nodes\":"); objects(result.nodes, projection);
    append(",\"paths\":[");
    bool first = true;
    for (const auto &path : result.paths) {
      if (!first) append(",");
      first = false;
      append("{\"nodes\":"); objects(path.nodes, projection);
      append(",\"edges\":"); objects(path.edges, projection); append("}");
    }
    append("]}");
  }
  std::string take() { return std::move(output_); }
};
} // namespace graphdb
