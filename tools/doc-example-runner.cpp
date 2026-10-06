// CLI fixture adapter over the public C API. Python owns fresh-directory cleanup.
#include "graphdb/CGraphDB.h"
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
int main(int argc, char** argv) {
  if (argc < 3) return 2;
  const std::string path(argv[1]);
  auto opened = graphdb_open_v2(path.data(), path.size(), 60000, 60000, nullptr);
  if (!opened.handle) { graphdb_string_free(opened.error); return 3; }
  int code = 0;
  for (int i = 2; i < argc; ++i) {
    std::ifstream input(argv[i]);
    if (!input) { code = 4; break; }
    std::string query((std::istreambuf_iterator<char>(input)), {});
    auto result = graphdb_execute_query_v2(opened.handle, query.data(), query.size(), nullptr, 0, nullptr);
    if (!result.data) { code = 5; break; }
    std::cout.write(result.data, result.len); std::cout << '\n';
    graphdb_string_free(result);
  }
  auto closed = graphdb_close_v2(opened.handle, nullptr);
  // Closing is itself part of the executable contract, checked by the caller.
  if (closed.data) { std::cout.write(closed.data, closed.len); std::cout << '\n'; }
  else code = 6;
  graphdb_string_free(closed); graphdb_release(opened.handle);
  return code;
}
