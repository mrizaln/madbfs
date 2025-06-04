include(cmake/prelude.cmake)

find_package(Boost REQUIRED)
find_package(libfuse REQUIRED)
find_package(unordered_dense REQUIRED)

include(cmake/fetched-libs.cmake) # linr, saf

add_library(madbfs-lib STATIC)
target_sources(
    madbfs-lib
    PRIVATE
        src/madbfs.cpp
        src/connection/connection.cpp
        src/connection/adb_connection.cpp
        src/connection/server_connection.cpp
        src/data/cache.cpp
        src/data/ipc.cpp
        src/path/path.cpp
        src/tree/file_tree.cpp
        src/tree/node.cpp
        src/tree/node.cpp
)
target_link_libraries(
    madbfs-lib
    PUBLIC
        madbfs-common
        fetch::linr
        boost::boost
        libfuse::libfuse
        unordered_dense::unordered_dense
)
target_include_directories(madbfs-lib PUBLIC include)

add_executable(madbfs src/main.cpp)
target_link_libraries(madbfs PRIVATE madbfs-lib)
target_compile_options(madbfs PRIVATE -Wall -Wextra -Wconversion)

# # sanitizer
# # target_compile_options(madbfs PRIVATE -fsanitize=address,leak,undefined)
# # target_link_options(madbfs PRIVATE -fsanitize=address,leak,undefined)
# target_compile_options(madbfs PRIVATE -fsanitize=address,undefined)
# target_link_options(madbfs PRIVATE -fsanitize=address,undefined)

if(MADBFS_ENABLE_TESTS)
    message(STATUS "Building tests")
    enable_testing()
    add_subdirectory(test)
endif()
