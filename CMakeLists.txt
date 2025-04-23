cmake_minimum_required(VERSION 3.16)
project(adbfsm VERSION 0.0.0)

# use unity build. turn off if you don't want to
set(CMAKE_UNITY_BUILD OFF)

set(ADBFSM_ENABLE_TESTS ON CACHE BOOL "Enable tests")

include(cmake/prelude.cmake)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(fmt REQUIRED)
find_package(spdlog REQUIRED)
find_package(libfuse REQUIRED)

include(cmake/fetched-libs.cmake) # emits fetch::*

add_library(adbfsm-lib STATIC)
target_sources(
    adbfsm-lib
    PRIVATE
        source/path/path.cpp
        source/tree/file_tree.cpp
        source/tree/node.cpp
        source/adbfsm.cpp
)
target_link_libraries(
    adbfsm-lib
    PUBLIC
        libfuse::libfuse
        fetch::subprocess
        fmt::fmt
        spdlog::spdlog
)
target_include_directories(adbfsm-lib PUBLIC include)
target_include_directories(adbfsm-lib PRIVATE source)

add_executable(adbfsm source/main.cpp)
target_link_libraries(adbfsm PRIVATE adbfsm-lib)
target_compile_options(adbfsm PRIVATE -Wall -Wextra -Wconversion)

# # sanitizer
# target_compile_options(adbfsm PRIVATE -fsanitize=address,leak,undefined)
# target_link_options(adbfsm PRIVATE -fsanitize=address,leak,undefined)

if(ADBFSM_ENABLE_TESTS)
    message(STATUS "Building tests")
    enable_testing()
    add_subdirectory(test)
endif()
