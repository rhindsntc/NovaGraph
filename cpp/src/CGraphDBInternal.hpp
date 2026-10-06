#pragma once
#include "graphdb/CGraphDB.h"
#include "graphdb/FileIO.hpp"
namespace graphdb::detail {
// Internal construction seam for real adapter fault/barrier acceptance tests.
// Public C opens always supply default_file_io(); this is not an exported C API.
GraphDBOpenResult open_with_io(const char *,long long,long long,
                              const GraphDBQueryOptions *,std::shared_ptr<FileIO>);
}
