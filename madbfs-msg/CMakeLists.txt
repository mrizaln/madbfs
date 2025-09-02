find_package(Boost REQUIRED)

add_executable(madbfs-msg src/main.cpp)
target_link_libraries(madbfs-msg PRIVATE madbfs-common boost::boost)
target_compile_options(madbfs-msg PRIVATE -Wall -Wextra -Wconversion)
