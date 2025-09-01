cmake_minimum_required(VERSION 3.22)
project(
    madbfs-msg
    VERSION 0.8.1
    LANGUAGES CXX
    DESCRIPTION "Userspace filesystem for Android via adb using FUSE (ipc component)"
)

include(cmake/prelude.cmake)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(MADBFS_BUILD_IPC ON CACHE BOOL "build ipc")

# https://stackoverflow.com/a/8349410/16506263
add_subdirectory(
    ${CMAKE_CURRENT_SOURCE_DIR}/../madbfs-common
    ${CMAKE_CURRENT_BINARY_DIR}/madbfs-common
)

# include(cmake/fetched-libs.cmake)

add_executable(madbfs-msg src/main.cpp)
target_link_libraries(madbfs-msg PRIVATE madbfs-common)
target_compile_options(madbfs-msg PRIVATE -Wall -Wextra -Wconversion)
