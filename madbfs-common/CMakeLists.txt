cmake_minimum_required(VERSION 3.12)

find_package(Boost REQUIRED)
find_package(rapidhash REQUIRED)

add_library(madbfs-common STATIC src/rpc.cpp)
target_link_libraries(madbfs-common PUBLIC boost::boost rapidhash::rapidhash)
target_include_directories(madbfs-common PUBLIC include)
target_compile_features(madbfs-common PUBLIC cxx_std_23)
set_target_properties(madbfs-common PROPERTIES CXX_EXTENSIONS OFF)
