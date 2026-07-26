include(cmake/prelude.cmake)

find_package(Boost REQUIRED)
find_package(libfuse REQUIRED)
find_package(zstr REQUIRED)

add_library(madbfs-lib STATIC)
target_sources(
  madbfs-lib
  PRIVATE
    src/adb.cpp
    src/args.cpp
    src/cmd.cpp
    src/connection.cpp
    src/file_handle_store.cpp
    src/filesystem.cpp
    src/madbfs.cpp
    src/node.cpp
    src/operations.cpp
    src/path.cpp
    src/transport/adb_transport.cpp
    src/transport/proxy_transport.cpp
    src/cache/dirty_map.cpp
    src/cache/page.cpp
    src/cache/lru_cache.cpp
)
target_link_libraries(
  madbfs-lib
  PUBLIC madbfs-common Boost::boost libfuse::libfuse zstr::zstr
)
target_include_directories(madbfs-lib PUBLIC include)
target_compile_options(madbfs-lib PRIVATE -Wall -Wextra -Wconversion)

set_source_files_properties(
  src/adb.cpp
  PROPERTIES
    COMPILE_DEFINITIONS MADBFS_SERVER_BINARY_DIR=${MADBFS_SERVER_BINARY_DIR}
)

message(STATUS "madbfs-server binary dir: ${MADBFS_SERVER_BINARY_DIR}")

add_executable(madbfs src/main.cpp)
target_link_libraries(madbfs PRIVATE madbfs-lib)
target_compile_options(madbfs PRIVATE -Wall -Wextra -Wconversion)

# # sanitizer
# target_compile_options(madbfs PRIVATE -fsanitize=address,leak,undefined)
# target_link_options(madbfs PRIVATE -fsanitize=address,leak,undefined)
