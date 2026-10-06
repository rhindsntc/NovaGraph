#include "graphdb/CGraphDB.h"
#include <iostream>
#include <string_view>
int main(int argc, char **argv) {
  if (argc < 3 || argc > 4) {
    std::cerr << "{\"ok\":false,\"code\":\"invalidArgument\",\"error\":\"usage: nova-maintenance "
                 "inspect|verify|rebuild-indexes SOURCE; backup|restore|migrate-v1 SOURCE "
                 "NEW_DESTINATION\"}\n";
    return 2;
  }
  std::string_view command = argv[1];
  bool destination = command == "backup" || command == "restore" || command == "migrate-v1";
  if ((destination && argc != 4) || (!destination && argc != 3)) {
    std::cerr
        << "{\"ok\":false,\"code\":\"invalidArgument\",\"error\":\"wrong number of paths\"}\n";
    return 2;
  }
  auto result = graphdb_maintenance(argv[1], argv[2], argc == 4 ? argv[3] : nullptr);
  if (!result.data) {
    std::cerr << "{\"ok\":false,\"code\":\"ioFailure\",\"error\":\"response allocation failed\"}\n";
    return 1;
  }
  std::string_view json(result.data, result.len);
  bool ok = json.starts_with("{\"ok\":true");
  std::cout << json << '\n';
  graphdb_string_free(result);
  return ok ? 0 : 1;
}
