cmake_minimum_required(VERSION 3.16)
project(madbfs VERSION 0.1.0)

# use unity build. turn off if you don't want to
set(CMAKE_UNITY_BUILD OFF)

set(MADBFS_ENABLE_TESTS ON CACHE BOOL "Enable tests")

include(cmake/prelude.cmake)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Boost REQUIRED)
find_package(fmt REQUIRED)
find_package(libfuse REQUIRED)
find_package(rapidhash REQUIRED)
find_package(spdlog REQUIRED)

add_subdirectory(vendor/saf)

add_library(madbfs-lib STATIC)
target_sources(
    madbfs-lib
    PRIVATE
        source/madbfs.cpp
        source/data/cache.cpp
        source/data/connection.cpp
        source/data/ipc.cpp
        source/path/path.cpp
        source/tree/file_tree.cpp
        source/tree/node.cpp
        source/tree/node.cpp
)
target_link_libraries(
    madbfs-lib
    PUBLIC
        boost::boost
        fmt::fmt
        libfuse::libfuse
        rapidhash::rapidhash
        saf::saf
        spdlog::spdlog
)
target_include_directories(madbfs-lib PUBLIC include)

add_executable(madbfs source/main.cpp)
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
