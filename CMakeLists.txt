cmake_minimum_required(VERSION 3.22)
project(
    madbfs
    VERSION 0.6.0
    LANGUAGES CXX
    DESCRIPTION "Userspace filesystem for Android via adb using FUSE"
)

# use unity build. turn off if you don't want to
set(CMAKE_UNITY_BUILD OFF)

set(MADBFS_ENABLE_TESTS ON CACHE BOOL "Enable tests")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(MADBFS_USE_NON_BOOST_ASIO OFF CACHE BOOL "use non-boost asio")
set(MADBFS_ENABLE_RAPIDHASH_BLANKET_IMPL ON CACHE BOOL "enable rapidhash")
add_subdirectory(madbfs-common)
add_subdirectory(madbfs)
