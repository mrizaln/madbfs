cmake_minimum_required(VERSION 3.12)

project(
    saf
    VERSION 0.1.0
    LANGUAGES CXX
    DESCRIPTION
        "single-header scheduler aware C++ future/promise that does not block"
)

add_library(saf_lib INTERFACE)
target_include_directories(saf_lib SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(saf_lib INTERFACE cxx_std_20)
set_target_properties(saf_lib PROPERTIES CXX_EXTENSIONS OFF)

add_library(saf::saf ALIAS saf_lib)
