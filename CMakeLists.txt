cmake_minimum_required(VERSION 3.22)
include(./madbfs-common/cmake/version.cmake)

project(
  madbfs
  VERSION ${MADBFS_PROJECT_VERSION}
  LANGUAGES CXX
  DESCRIPTION "Userspace filesystem for Android via adb using FUSE"
)

# # use unity build. turn off if you don't want to
# set(CMAKE_UNITY_BUILD OFF)

option(MADBFS_ENABLE_TESTS "Enable tests" ON)
option(MADBFS_AUTORUN_TESTS "Automatically ran tests" OFF)
option(MADBFS_BUILD_DOCS "Build docs" OFF)
option(MADBFS_BUILD_TARGET_BASELINE "Build target x86_64-baseline" ON)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_SKIP_RPATH TRUE)  # stop injecting RPATH for conan deps

set(MADBFS_USE_NON_BOOST_ASIO OFF CACHE BOOL "use non-boost asio")
set(MADBFS_ENABLE_RAPIDHASH_BLANKET_IMPL ON CACHE BOOL "enable rapidhash")
set(MADBFS_BUILD_IPC ON CACHE BOOL "build ipc")

if(MADBFS_BUILD_TARGET_BASELINE AND CMAKE_SYSTEM_PROCESSOR MATCHES "(x86_64|amd64|AMD64)")
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-march=x86-64)
    message(STATUS "madbfs: Build targeting x86_64-baseline")
  else()
    message(FATAL_ERROR "Currently only supporting GNU/Clang for this target")
  endif()
endif()

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

