cmake_minimum_required(VERSION 3.22)
include(./madbfs-common/cmake/version.cmake)

project(
  madbfs
  VERSION ${MADBFS_PROJECT_VERSION}
  LANGUAGES CXX
  DESCRIPTION "Userspace filesystem for Android via adb using FUSE"
)

# use unity build. turn off if you don't want to
set(CMAKE_UNITY_BUILD OFF)

option(MADBFS_ENABLE_TESTS "Enable tests" ON)
option(MADBFS_AUTORUN_TESTS "Automatically ran tests" OFF)
option(MADBFS_BUILD_DOCS "Build docs" ON)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(MADBFS_USE_NON_BOOST_ASIO OFF CACHE BOOL "use non-boost asio")
set(MADBFS_ENABLE_RAPIDHASH_BLANKET_IMPL ON CACHE BOOL "enable rapidhash")
set(MADBFS_BUILD_IPC ON CACHE BOOL "build ipc")

add_subdirectory(madbfs-common)
add_subdirectory(madbfs)
add_subdirectory(madbfs-msg)

if(MADBFS_ENABLE_TESTS)
  message(STATUS "madbfs: Building tests")
  enable_testing()
  add_subdirectory(test) # add custom target: build_tests
endif()

if(MADBFS_BUILD_DOCS)
  add_subdirectory(docs)
endif()

