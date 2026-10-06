#include "TestSupport.hpp"
#include "graphdb/CGraphDB.h"
#include "../third_party/nlohmann/json.hpp"
using namespace nova_test;
using Json = nlohmann::json;
#include <cstdlib>
#include <new>
// Test-only allocation seam: C response buffers use new[]. Limit fault to this
// thread and one allocation so background workers and error reporting still run.
namespace { thread_local bool fail_response_array = false; }
void *operator new[](std::size_t bytes) {
  if (fail_response_array) { fail_response_array=false; throw std::bad_alloc(); }
  if (auto pointer=std::malloc(bytes ? bytes : 1)) return pointer;
  throw std::bad_alloc();
}
void operator delete[](void *pointer) noexcept { std::free(pointer); }
void operator delete[](void *pointer, std::size_t) noexcept { std::free(pointer); }

namespace {
struct Database {
  TempDirectory dir;
  GraphDBHandle *handle = graphdb_open(dir.path().c_str());
  ~Database() { graphdb_close(handle); }
};
Json decode(GraphDBString value) {
  struct Free { GraphDBString value; ~Free() { graphdb_string_free(value); } } free{value};
  CHECK(value.data != nullptr);
  return Json::parse(value.data, value.data + value.len);
}
// Exercise the v2 contract and legacy compatibility through the public C header.
Json request(GraphDBHandle *db, const std::string &query, const std::string &params = "{}") {
  return decode(graphdb_execute_query_v2(db, query.data(), query.size(), params.data(), params.size(), nullptr));
}
}
NOVA_TEST(c_v2_scalar_tags_and_commit_receipt, "c_api", "") {
  Database db; CHECK(db.handle);
  auto written=request(db.handle,"upsert node N v set one=$one, real=$real, max=$max", "{\"one\":1,\"real\":1.0,\"max\":9223372036854775807}");
  CHECK(written.value("schemaVersion",0)==2);
  CHECK(written.at("receipt").at("committedLSN").get<uint64_t>()>0);
  CHECK(!written.at("receipt").at("transactionId").get<std::string>().empty());
  auto read=request(db.handle,"get node v");
  CHECK(read.at("data").at("properties").at("one")==Json({{"type","int"},{"value",1}}));
  CHECK(read.at("data").at("properties").at("real").at("type")=="double");
  CHECK(read.at("data").at("properties").at("max").at("value")==INT64_MAX);
  auto legacy=decode(graphdb_execute_query(db.handle,"get node v"));
  CHECK(!legacy.contains("schemaVersion"));
  CHECK(legacy.at("data").at("properties").at("one")==1);
}
NOVA_TEST(c_v2_length_bearing_nul_and_structured_errors, "c_api", "") {
  Database db;
  std::string query="upsert node N \"a";query.push_back('\0');query+="b\" set text=\"tail\"";
  auto written=request(db.handle,query);CHECK(written.at("ok")==true);
  auto read=request(db.handle,std::string("get node \"a")+std::string(1,'\0')+"b\"");
  CHECK(read.at("data").at("id")==std::string("a\0b",3));
  auto missing=request(db.handle,"get node absent");
  CHECK(missing.at("ok")==false);
  CHECK(missing.at("error").is_object());
  CHECK(missing.at("error").at("code")=="notFound");
  CHECK(missing.at("error").at("context").at("statementIndex")==1);
}
NOVA_TEST(c_v2_atomic_failure_and_legacy_error_shape, "c_api", "") {
  Database db;
  auto failed=request(db.handle,"upsert node N kept; upsert edge E kept -> absent");
  CHECK(failed.at("ok")==false);
  CHECK(failed.at("error").is_object());
  CHECK(!failed.contains("receipt"));
  CHECK(request(db.handle,"get node kept").at("ok")==false);
  auto legacy=decode(graphdb_execute_query(db.handle,"get node kept"));
  CHECK(legacy.at("error").is_string());CHECK(legacy.at("code")=="notFound");
  graphdb_string_free({nullptr,0});
}

NOVA_TEST(c_v2_validates_lengths_options_and_utf8_before_mutation, "c_api", "") {
  Database db;
  auto invalid=decode(graphdb_execute_query_v2(db.handle,nullptr,1,nullptr,0,nullptr));
  CHECK(invalid.at("error").at("code")=="invalidArgument");
  const char byte='x';
  CHECK(decode(graphdb_execute_query_v2(db.handle,&byte,1024*1024+1,nullptr,0,nullptr)).at("error").at("code")=="limitExceeded");
  CHECK(request(db.handle,std::string("\xff",1)).at("ok")==false);
  CHECK(request(db.handle,"get node absent",std::string("{}\0{}",5)).at("error").at("code")=="invalidArgument");
  auto options=graphdb_default_query_options();options.result_bytes=1;
  std::string query="upsert node N rejected";
  CHECK(decode(graphdb_execute_query_v2(db.handle,query.data(),query.size(),nullptr,0,&options)).at("error").at("code")=="limitExceeded");
  CHECK(request(db.handle,"get node rejected").at("error").at("code")=="notFound");
  options.timeout_ms=0;
  CHECK(decode(graphdb_execute_query_v2(db.handle,query.data(),query.size(),nullptr,0,&options)).at("error").at("code")=="invalidArgument");
}
NOVA_TEST(c_v2_open_error_and_nonterminated_input_ownership, "c_api", "") {
  TempDirectory dir;
  const auto name=dir.path().string();
  std::vector<char> bytes(name.begin(),name.end());
  auto result=graphdb_open_v2(bytes.data(),bytes.size(),600000,30000,nullptr);
  CHECK(result.handle);CHECK(result.error.data==nullptr);graphdb_string_free(result.error);
  auto second=graphdb_open_v2(bytes.data(),bytes.size(),600000,30000,nullptr);
  CHECK(!second.handle);
  auto error=decode(second.error);CHECK(error.at("schemaVersion")==2);CHECK(error.at("error").at("code")=="busy");
  auto invalid=graphdb_open_v2("a\0b",3,600000,30000,nullptr);
  CHECK(!invalid.handle);CHECK(decode(invalid.error).at("error").at("code")=="invalidArgument");
  for(int i=0;i<50;++i) {
    const char query[]={'g','e','t',' ','n','o','d','e',' ','x'};
    CHECK(decode(graphdb_execute_query_v2(result.handle,query,sizeof(query),nullptr,0,nullptr)).at("error").at("code")=="notFound");
  }
  graphdb_close(result.handle);
}
NOVA_TEST(c_v2_per_request_options_only_tighten_database_bounds, "c_api", "") {
  TempDirectory dir;auto options=graphdb_default_query_options();options.max_results=1;
  auto opened=graphdb_open_v2(dir.path().c_str(),dir.path().string().size(),600000,30000,&options);
  CHECK(opened.handle);graphdb_string_free(opened.error);
  CHECK(request(opened.handle,"upsert node N a; upsert node N b").at("ok")==true);
  auto defaults=graphdb_default_query_options();std::string query="find nodes N limit 2";
  auto response=decode(graphdb_execute_query_v2(opened.handle,query.data(),query.size(),nullptr,0,&defaults));
  CHECK(response.at("error").at("code")=="limitExceeded");
  query="find nodes N limit 1";
  CHECK(request(opened.handle,query).at("data").size()==1);
  graphdb_close(opened.handle);
}

NOVA_TEST(c_response_allocation_failure_precedes_commit_in_both_abis, "c_api", "") {
  Database db;
  for(bool versioned : {false,true}) {
    const std::string query="upsert node N must_not_commit";
    fail_response_array=true;
    auto output=versioned ? graphdb_execute_query_v2(db.handle,query.data(),query.size(),nullptr,0,nullptr)
                          : graphdb_execute_query(db.handle,query.c_str());
    fail_response_array=false;
    auto error=decode(output);CHECK(error.at("ok")==false);
    auto read=request(db.handle,"get node must_not_commit");
    CHECK(read.at("ok")==false);
    CHECK(read.at("error").at("code")=="notFound");
  }
}

namespace {
Json inspect(GraphDBHandle *db, unsigned kind, const std::string &cursor = "", unsigned limit = 2,
             const GraphDBQueryOptions *options = nullptr, GraphDBRequest *request = nullptr) {
  return decode(graphdb_inspect_v2(db, kind, cursor.data(), cursor.size(), limit, options, request));
}
}
NOVA_TEST(c_inspection_pages_are_typed_bounded_and_revision_checked, "c_api", "") {
  Database db;
  CHECK(request(db.handle,"upsert node N a set exact=9223372036854775807; upsert node N b; upsert node N c; upsert edge E a -> b; create index on N(exact)").at("ok")==true);
  auto first=inspect(db.handle,0).at("data");
  CHECK(first.at("items").size()==2);
  CHECK(first.at("items")[0].at("id")=="a");
  CHECK(first.at("items")[0].at("properties").at("exact").at("value")==INT64_MAX);
  auto cursor=first.at("nextCursor").get<std::string>();
  auto second=inspect(db.handle,0,cursor).at("data");
  CHECK(second.at("items").size()==1);CHECK(second.at("items")[0].at("id")=="c");
  CHECK(second.at("nextCursor").is_null());
  CHECK(inspect(db.handle,1).at("data").at("items")[0].at("from")=="a");
  CHECK(inspect(db.handle,2).at("data").at("items")[0]==Json({{"label","N"},{"property","exact"}}));
  CHECK(inspect(db.handle,1,cursor).at("error").at("code")=="invalidArgument");
  CHECK(decode(graphdb_trim_memory_v2(db.handle,0,nullptr)).at("ok")==true);
  CHECK(inspect(db.handle,0).at("data").at("items")[0].at("properties").at("exact").at("value")==INT64_MAX);
  CHECK(inspect(db.handle,2).at("data").at("items")[0].at("property")=="exact");
  CHECK(request(db.handle,"upsert node N d").at("ok")==true);
  CHECK(inspect(db.handle,0,cursor).at("error").at("code")=="conflict");
}
NOVA_TEST(c_inspection_rejects_invalid_limits_cancellation_and_closed_handles, "c_api", "") {
  Database db;
  CHECK(inspect(db.handle,3).at("error").at("code")=="invalidArgument");
  CHECK(inspect(db.handle,0,"",0).at("error").at("code")=="invalidArgument");
  CHECK(inspect(db.handle,0,"",101).at("error").at("code")=="invalidArgument");
  CHECK(inspect(db.handle,0,"bad cursor").at("error").at("code")=="invalidArgument");
  CHECK(decode(graphdb_inspect_v2(db.handle,0,nullptr,1,2,nullptr,nullptr)).at("error").at("code")=="invalidArgument");
  auto r=graphdb_request_create(5000);graphdb_request_cancel(r);
  CHECK(inspect(db.handle,0,"",2,nullptr,r).at("error").at("code")=="cancelled");graphdb_request_release(r);
  CHECK(request(db.handle,"upsert node N a").at("ok")==true);
  auto options=graphdb_default_query_options();options.result_bytes=16;
  CHECK(inspect(db.handle,0,"",2,&options).at("error").at("code")=="limitExceeded");
  CHECK(request(db.handle,"upsert node N b").at("ok")==true);
  options=graphdb_default_query_options();options.max_expanded_edges=1;
  CHECK(inspect(db.handle,0,"",1,&options).at("error").at("code")=="limitExceeded");
  CHECK(decode(graphdb_close_v2(db.handle,nullptr)).at("ok")==true);
  CHECK(inspect(db.handle,0).at("error").at("code")=="closed");
}
NOVA_TEST(c_inspection_cursors_are_bound_to_engine_instance, "c_api", "") {
  Database first, second;
  CHECK(request(first.handle,"upsert node N z; upsert node N zz").at("ok")==true);
  CHECK(request(second.handle,"upsert node N a; upsert node N b").at("ok")==true);
  auto cursor=inspect(first.handle,0,"",1).at("data").at("nextCursor").get<std::string>();
  auto result=inspect(second.handle,0,cursor,1);
  CHECK(result.at("ok")==false);CHECK(result.at("error").at("code")=="conflict");
  graphdb_close(first.handle);first.handle=graphdb_open(first.dir.path().c_str());CHECK(first.handle);
  CHECK(inspect(first.handle,0,cursor,1).at("error").at("code")=="conflict");
}
