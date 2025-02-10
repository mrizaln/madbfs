# You can add libraries here that are not managed by conan or system.

# * For example you want to add a new library called 'mylib' to your project.
#   You have copied the file into 'lib/mylib' directory. It doesn't have a
#   CMakeLists.txt file, so you need to manually add it to the project.

set(mylib_DIR "${CMAKE_CURRENT_SOURCE_DIR}/mylib")
set(mylib_INCLUDE_DIRS "${mylib_DIR}/include")
set(mylib_SOURCES "${mylib_DIR}/src/mylib.cpp" ...) # declare the sources here

add_library(mylib STATIC "${mylib_SOURCES}")
target_include_directories(mylib PUBLIC "${mylib_INCLUDE_DIRS}")

# * If the project has a CMakeLists.txt, it is more straightforward. For
#   example, you have an library copied to 'lib/otherlib':
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/otherlib")

# you can add more libraries here ...

# * You can create a variable that emitted from this cmake file that contains
#   the library targets, so that the parent CMakeLists.txt can use it. Or you
#   don't, you can just use the library targets directly.
set(LIB
  mylib
  otherlib
  PARENT_SCOPE)
