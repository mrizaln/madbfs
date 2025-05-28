cmake_minimum_required(VERSION 3.22)
project(madbfs-server VERSION 0.1.0)

include(cmake/prelude.cmake)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -static")

find_package(asio REQUIRED)
find_package(fmt REQUIRED)
find_package(spdlog REQUIRED)

# https://stackoverflow.com/a/8349410/16506263
set(MADBFS_USE_NON_BOOST_ASIO ON CACHE BOOL "use non-boost asio")
set(MADBFS_ENABLE_RAPIDHASH_BLANKET_IMPL OFF CACHE BOOL "disable rapidhash")
add_subdirectory(
    ${CMAKE_CURRENT_SOURCE_DIR}/../madbfs-common
    ${CMAKE_CURRENT_BINARY_DIR}/madbfs-common
)

add_library(madbfs-server-lib STATIC src/server.cpp)
target_include_directories(madbfs-server-lib PUBLIC include)
target_link_libraries(
    madbfs-server-lib
    PUBLIC fmt::fmt spdlog::spdlog madbfs-common
)

add_executable(madbfs-server src/main.cpp)
target_include_directories(madbfs-server PRIVATE src)
target_link_libraries(madbfs-server PRIVATE madbfs-server-lib)
target_compile_options(madbfs-server PRIVATE -Wall -Wextra -Wconversion)
