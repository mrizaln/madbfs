include(cmake/prelude.cmake)

find_package(Boost REQUIRED)
find_package(libfuse REQUIRED)

include(cmake/fetched-libs.cmake) # linr, saf

add_library(madbfs-lib STATIC)
target_sources(
    madbfs-lib
    PRIVATE
        src/madbfs.cpp
        src/cmd.cpp
        src/operations.cpp
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
    PUBLIC madbfs-common fetch::linr boost::boost libfuse::libfuse
)
target_include_directories(madbfs-lib PUBLIC include)
target_compile_definitions(
    madbfs-lib
    PUBLIC MADBFS_VERSION_STRING="${CMAKE_PROJECT_VERSION}"
)

add_executable(madbfs src/main.cpp)
target_link_libraries(madbfs PRIVATE madbfs-lib)
target_compile_options(madbfs PRIVATE -Wall -Wextra -Wconversion)

# # sanitizer
# target_compile_options(madbfs PRIVATE -fsanitize=address,leak,undefined)
# target_link_options(madbfs PRIVATE -fsanitize=address,leak,undefined)

if(MADBFS_ENABLE_TESTS)
    message(STATUS "Building tests")
    enable_testing()
    add_subdirectory(test)
endif()
