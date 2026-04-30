include(cmake/prelude.cmake)

find_package(Boost REQUIRED)
find_package(libfuse REQUIRED)

include(cmake/fetched-libs.cmake) # linr, saf

add_library(madbfs-lib STATIC)
target_sources(
  madbfs-lib
  PRIVATE
    src/adb.cpp
    src/args.cpp
    src/cmd.cpp
    src/connection.cpp
    src/data/cache.cpp
    src/filesystem.cpp
    src/madbfs.cpp
    src/node.cpp
    src/operations.cpp
    src/path.cpp
    src/transport/adb_transport.cpp
    src/transport/proxy_transport.cpp
)
target_link_libraries(
  madbfs-lib
  PUBLIC madbfs-common fetch::linr boost::boost libfuse::libfuse
)
target_include_directories(madbfs-lib PUBLIC include)

add_executable(madbfs src/main.cpp)
target_link_libraries(madbfs PRIVATE madbfs-lib)
target_compile_options(madbfs PRIVATE -Wall -Wextra -Wconversion)

# # sanitizer
# target_compile_options(madbfs PRIVATE -fsanitize=address,leak,undefined)
# target_link_options(madbfs PRIVATE -fsanitize=address,leak,undefined)
