cmake_minimum_required(VERSION 3.22)
project(NovaGraph LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_library(novagraphdb
  src/MaintenanceOperations.cpp
  src/LegacyImport.cpp
  src/Maintenance.cpp
  src/Recovery.cpp
  src/Transaction.cpp
  src/FileIO.cpp
  src/DatabaseLock.cpp
  src/QueryContext.cpp
  src/ValueCodec.cpp
  src/GraphTypes.cpp
  src/Storage.cpp
  src/GraphEngine.cpp
  src/DslParser.cpp
  src/Wal.cpp
  src/Catalog.cpp
  src/CatalogScan.cpp
  src/CGraphDB.cpp
)

target_include_directories(novagraphdb PUBLIC include PRIVATE third_party)

target_compile_options(novagraphdb PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic>
)

add_executable(nova-maintenance ../tools/NovaMaintenance/main.cpp)
target_link_libraries(nova-maintenance PRIVATE novagraphdb)

include(CTest)
option(NOVA_STRICT_REGRESSIONS "Fail on every unresolved production regression" OFF)
if(BUILD_TESTING)
  set(NOVA_TEST_TIMEOUT 30 CACHE STRING "Per-suite timeout in seconds; increase for sanitizer runs")
  if(NOT NOVA_TEST_TIMEOUT MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "NOVA_TEST_TIMEOUT must be a positive integer number of seconds")
  endif()
  find_package(Threads REQUIRED)
  add_executable(nova_graph_test
    tests/test_engine.cpp tests/test_lifecycle.cpp tests/test_c_api.cpp tests/test_values.cpp tests/test_storage.cpp
    tests/test_queries.cpp tests/test_traversal.cpp tests/test_parser.cpp tests/test_limits.cpp tests/test_regressions.cpp tests/test_io.cpp tests/test_wal.cpp tests/test_catalog.cpp tests/test_indexes.cpp tests/test_transactions.cpp tests/test_consistency.cpp tests/test_checkpoint.cpp tests/test_tiering.cpp tests/test_gc.cpp tests/test_recovery.cpp tests/test_maintenance.cpp tests/test_model.cpp)
  target_link_libraries(nova_graph_test PRIVATE novagraphdb Threads::Threads)
  add_executable(nova_crash_worker tests/faults/CrashWorker.cpp)
  target_link_libraries(nova_crash_worker PRIVATE novagraphdb Threads::Threads)
  set(NOVA_TEST_OPTIONS)
  if(NOVA_STRICT_REGRESSIONS)
    list(APPEND NOVA_TEST_OPTIONS --strict)
  endif()
  foreach(suite lifecycle c_api values storage queries traversal parser limits regressions io wal catalog indexes transactions consistency checkpoint tiering gc recovery maintenance model)
    add_test(NAME nova_${suite} COMMAND nova_graph_test --suite ${suite} ${NOVA_TEST_OPTIONS})
    set_tests_properties(nova_${suite} PROPERTIES TIMEOUT ${NOVA_TEST_TIMEOUT})
  endforeach()
  if(NOVA_TEST_TIMEOUT LESS 120)
    set_tests_properties(nova_maintenance PROPERTIES TIMEOUT 120)
  endif()
  set_tests_properties(nova_regressions PROPERTIES LABELS "known-release-blockers")
endif()

add_executable(nova-doc-example ../tools/doc-example-runner.cpp)
target_link_libraries(nova-doc-example PRIVATE novagraphdb)
