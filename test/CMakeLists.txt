find_package(ut)
include(cmake/fetched-libs.cmake)

function(create_test_exe name)
  add_executable(${name} ${CMAKE_CURRENT_SOURCE_DIR}/${name}.cpp)
  target_link_libraries(${name} PRIVATE madbfs-lib Boost::ut fetch::dtlx)
  target_compile_options(${name} PRIVATE -Wall -Wextra -Wconversion)

  # # sanitizer
  # target_compile_options(${name} PRIVATE -fsanitize=address,leak,undefined)
  # target_link_options(${name} PRIVATE -fsanitize=address,leak,undefined)

  # register test
  add_test(NAME ${name} COMMAND $<TARGET_FILE:${name}>)

  # post-build command
  if(MADBFS_AUTORUN_TESTS)
    add_custom_command(TARGET ${name} POST_BUILD COMMAND $<TARGET_FILE:${name}>)
  endif()
endfunction()

enable_testing()

create_test_exe(test_tree)
create_test_exe(test_path)
create_test_exe(test_rpc)
create_test_exe(test_ipc)
