find_package(ut)
include(cmake/fetched-libs.cmake)

function(create_test_exe name)
    add_executable(${name} ${CMAKE_CURRENT_SOURCE_DIR}/${name}.cpp)
    target_link_libraries(
        ${name}
        PRIVATE adbfsm-lib Boost::ut fetch::dtl-modern
    )
    target_compile_options(${name} PRIVATE -Wall -Wextra -Wconversion)

    # sanitizer
    target_compile_options(${name} PRIVATE -fsanitize=address,leak,undefined)
    target_link_options(${name} PRIVATE -fsanitize=address,leak,undefined)

    # register test and post-build command
    add_test(name ${name} COMMAND $<TARGET_FILE:${name}>)
    add_custom_command(TARGET ${name} POST_BUILD COMMAND $<TARGET_FILE:${name}>)
endfunction()

enable_testing()

create_test_exe(tree)
create_test_exe(path)
