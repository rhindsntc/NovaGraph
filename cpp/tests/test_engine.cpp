#include "TestSupport.hpp"
#include <algorithm>
using namespace nova_test;
int main(int argc,char** argv) {
  std::cout << std::unitbuf; // Preserve the last completed case on a timeout.
  bool strict=false,list=false; std::string suite;
  for (int i=1;i<argc;++i) {
    std::string arg=argv[i];
    if (arg=="--strict") strict=true;
    else if (arg=="--list-json") list=true;
    else if (arg=="--suite" && i+1<argc) suite=argv[++i];
    else { std::cerr<<"unknown/incomplete option: "<<arg<<'\n'; return 2; }
  }
  auto all=cases(); std::sort(all.begin(),all.end(),[](auto& a,auto& b){return a.id<b.id;});
  if (list) {
    std::cout<<"["; bool first=true;
    for (auto& test:all) {
      if (!first) std::cout<<',';
      first=false;
      std::cout<<"{\"id\":\""<<test.id<<"\",\"suite\":\""<<test.suite<<"\",\"owner\":\""<<test.owner<<"\"}";
    }
    std::cout<<"]\n"; return 0;
  }
  int passed=0,known=0,failed=0;
  for (auto& test:all) {
    if (!suite.empty() && test.suite!=suite) continue;
    try {
      test.run();
      if (!test.owner.empty()) { ++failed; std::cout<<"XPASS "<<test.id<<": remove expectation after "<<test.owner<<" verification\n"; }
      else { ++passed; std::cout<<"PASS "<<test.id<<'\n'; }
    } catch (const KnownFailure& error) {
      if (error.id!=test.id || test.owner.empty() || strict) { ++failed; std::cout<<"FAIL "<<test.id<<": "<<error.what()<<'\n'; }
      else { ++known; std::cout<<"XFAIL "<<test.id<<" ["<<test.owner<<"; remove when fixed]: "<<error.what()<<'\n'; }
    } catch (const std::exception& error) {
      ++failed; std::cout<<"FAIL "<<test.id<<" (unexpected/setup): "<<error.what()<<'\n';
    }
  }
  std::cout<<passed<<" passed, "<<known<<" known release blockers, "<<failed<<" failed\n";
  return failed || passed+known==0 ? 1 : 0;
}
