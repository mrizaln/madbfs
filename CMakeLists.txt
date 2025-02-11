cmake_minimum_required(VERSION 3.16)
project(adbfsm VERSION 0.0.0)

include(cmake/prelude.cmake)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(fmt REQUIRED)
find_package(spdlog REQUIRED)
find_package(PkgConfig REQUIRED)

pkg_check_modules(FUSE REQUIRED IMPORTED_TARGET fuse)

include(cmake/fetched-libs.cmake)       # emits fetch::*

add_executable(adbfsm source/main.cpp source/adbfsm.cpp)
target_include_directories(adbfsm PRIVATE source)
target_link_libraries(adbfsm PRIVATE PkgConfig::FUSE fetch::subprocess fmt::fmt spdlog::spdlog)
target_compile_options(adbfsm PRIVATE -Wall -Wextra -Wconversion)

# sanitizer
target_compile_options(adbfsm PRIVATE -fsanitize=address,leak,undefined)
target_link_options(adbfsm PRIVATE -fsanitize=address,leak,undefined)
