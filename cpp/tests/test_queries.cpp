#include "TestSupport.hpp"
using namespace nova_test;
NOVA_TEST(parameters_comparisons_projections_explain,"queries","") {
  TempDirectory dir; Handle db(dir.path());
  auto setup=db.query("create index on Developer(email); upsert node Developer alice set name=\"Alice\", email=\"alice@nova.test\", age=28; upsert node Developer bob set name=\"Bob\", email=\"bob@nova.test\", age=34;");
  CHECK(setup.find("\"ok\":false")==std::string::npos);
  auto parameter=db.query("find nodes Developer where email = $email",R"({"email":"bob@nova.test"})"); CHECK(parameter.find("\"name\":\"Bob\"")!=std::string::npos);
  auto filter=db.query("find nodes Developer where age >= 30"); CHECK(filter.find("Bob")!=std::string::npos); CHECK(filter.find("Alice")==std::string::npos);
  auto projection=db.query("find nodes Developer where age < 30 return { id, name }"); CHECK(projection.find("Alice")!=std::string::npos); CHECK(projection.find("email")==std::string::npos);
  auto explain=db.query("explain find nodes Developer where email = \"alice@nova.test\""); CHECK(explain.find("PropertyIndexScan")!=std::string::npos);
  auto rebuild=db.query("rebuild indexes"); CHECK(rebuild.find("\"ok\":true")!=std::string::npos);
}
