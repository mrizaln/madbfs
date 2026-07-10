set(FETCHCONTENT_QUIET FALSE)
include(FetchContent)

find_package(Doxygen REQUIRED)

if(DOXYGEN_FOUND)
  FetchContent_Declare(
    doxygen-awesome-css
    URL https://github.com/jothepro/doxygen-awesome-css/archive/refs/heads/main.zip
    DOWNLOAD_EXTRACT_TIMESTAMP ON
  )
  FetchContent_MakeAvailable(doxygen-awesome-css)
  FetchContent_GetProperties(doxygen-awesome-css SOURCE_DIR AWESOME_CSS_DIR)

  set(DOXYFILE_IN ${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile.in)
  set(DOXYFILE_OUT ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile)

  configure_file(${DOXYFILE_IN} ${DOXYFILE_OUT} @ONLY)

  add_custom_target(
    docs
    COMMAND Doxygen::doxygen ${DOXYFILE_OUT}
    COMMENT "Generating docs with Doxygen"
    VERBATIM
  )
else()
  message(WARNING "Doxygen can't be found, skipping docs target")
endif()
