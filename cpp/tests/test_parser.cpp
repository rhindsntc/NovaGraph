#include "TestSupport.hpp"
using namespace graphdb;
using namespace nova_test;
// Each fixture exercises grammar acceptance/rejection through the real engine.
NOVA_TEST(parser_comments_preserve_quoted_text_and_statement_boundaries, "parser", "") {
  EngineFixture f;
  auto result = f.engine->execute_dsl("// ignored ; ' quote\nupsert node P a set value=\"https://nova/; // text\"; // ignored\nget node a return {value}; // final");
  CHECK(result.ok);
  CHECK(result.json.find("https://nova/; // text") != std::string::npos);
}
NOVA_TEST(parser_rejects_malformed_options_without_mutation, "parser", "") {
  const char *invalid[] = {
    "walk from a direction sideways", "walk from a depth -1", "find nodes P limit -1",
    "walk from a depth 18446744073709551616", "find nodes P limit +1",
    "walk from a depth 1.5", "find nodes P limit 1 limit 2", "walk from a over E over F",
    "get node a return", "get node a return {}", "get node a return {id name}",
    "get node a return {id,}", "find nodes P return id,", "upsert node P b set",
    "upsert node P b set n=1,", "upsert node P b set n=1,n=2",
    R"(upsert node P b set s="bad\q")", "checkpoint garbage", "rebuild indexes garbage",
    "walk from a direction", "find nodes P where n ~~ 1", "get node a garbage"
  };
  for (auto query : invalid) {
    EngineFixture f;
    auto result=f.engine->execute_dsl(std::string("upsert node P sentinel; ")+query);
    if(result.ok) throw Failure(std::string("accepted malformed query: ")+query);
    CHECK(!f.engine->get_node("sentinel"));
  }
}
NOVA_TEST(parser_quoted_parameter_lookalikes_are_literal, "parser", "") {
  EngineFixture f;
  CHECK(f.engine->execute_dsl("upsert node P '$id' set value='$value'", {{"id",std::string("wrong")}}).ok);
  CHECK(f.engine->get_node("$id"));
  CHECK(!f.engine->get_node("wrong"));
}
NOVA_TEST(parser_typed_parameter_values_cannot_inject_statements, "parser", "") {
  EngineFixture f;
  PropertyMap params{{"id",std::string("a; delete node victim")},{"n",int64_t(9223372036854775807LL)},
    {"s",std::string("x=1; // quoted\nnext")},{"b",true},{"null",std::monostate{}},{"d",1.25}};
  CHECK(f.engine->execute_dsl("upsert node P $id set n=$n,s=$s,b=$b,nil=$null,d=$d",params).ok);
  auto got=f.engine->get_node("a; delete node victim"); CHECK(got);
  CHECK(got.value.properties.at("n")==Property{int64_t(9223372036854775807LL)});
  CHECK(got.value.properties.at("s")==params.at("s"));
}

NOVA_TEST(parser_error_envelope_has_statement_and_exact_token_span, "parser", "") {
  TempDirectory dir; Handle db(dir.path());
  auto result=db.query("get node missing; walk from a depth -1");
  CHECK(result.find("\"statementIndex\":2")!=std::string::npos);
  CHECK(result.find("\"sourceStart\":36")!=std::string::npos);
  CHECK(result.find("\"sourceEnd\":38")!=std::string::npos);
}

NOVA_TEST(parser_lexical_error_span_includes_original_leading_comments, "parser", "") {
  EngineFixture f;
  std::string query="  // lead\nupsert node P a; upsert node P b set value='bad\\q'";
  auto result=f.engine->execute_dsl(query);
  CHECK(!result.ok); CHECK(result.status.context.statement_index==2);
  CHECK(result.status.context.source_start==query.find("'bad"));
  CHECK(result.status.context.source_end && *result.status.context.source_end>*result.status.context.source_start);
  CHECK(!f.engine->get_node("a"));
}
NOVA_TEST(parser_supports_statement_operator_escape_and_parameter_fixtures, "parser", "") {
  EngineFixture f;
  const char *valid[]={
    "create node P a set n: 2,s='line\\n\\r\\t\\\\\\\"\\\'',b=true,nil=null,d=2.5",
    "create node P b set n=3", "create edge E a -> b set n=1", "create index on P(n)",
    "get node a return id,n", "get edge E a -> b return {from,to,n}",
    "walk from a direction out over E depth 1 return {id} limit 2",
    "walk from b direction in over E depth 1 limit 2", "explain get node a",
    "explain get edge E a -> b", "explain walk from a", "explain create node P z",
    "explain create index on P(n)", "explain rebuild indexes", "explain checkpoint",
    "find nodes P where n = 2", "find nodes P where n == 2", "find nodes P where n != 2",
    "find nodes P where n < 3", "find nodes P where n <= 3", "find nodes P where n > 2", "find nodes P where n >= 2",
    "rebuild index", "rebuild indexes", "checkpoint", "delete edge E a -> b", "delete node b"
  };
  for(auto query:valid) {auto result=f.engine->execute_dsl(query);if(!result.ok)throw Failure(std::string(query)+": "+result.error);}
  CHECK(f.engine->execute_dsl("find nodes P limit $limit",{{"limit",int64_t(1)}}).ok);
  CHECK(!f.engine->execute_dsl("find nodes P limit $limit",{{"limit",std::string("1")}}).ok);
  CHECK(!f.engine->execute_dsl("get node $missing").ok);
}

NOVA_TEST(parser_rejects_invalid_utf8_projection_before_serialization, "parser", "") {
  EngineFixture f;CHECK(f.engine->upsert_node("P","a",{}).ok);
  auto result=f.engine->execute_dsl(std::string("get node a return {'bad")+char(0xff)+"'}");
  CHECK(!result.ok);CHECK(result.status.code==ErrorCode::parseError);
}

NOVA_TEST(parser_failed_keyword_points_to_offending_token, "parser", "") {
  EngineFixture f;
  auto result=f.engine->execute_dsl("create nonsense");CHECK(!result.ok);
  CHECK(result.status.context.source_start==7);CHECK(result.status.context.source_end==15);
}
