set(MADBFS_VERSION_FULL "${CMAKE_PROJECT_VERSION}")

find_package(Git QUIET)
if(GIT_FOUND)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" describe --tags --exact-match
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    RESULT_VARIABLE GIT_TAG_MATCH
    OUTPUT_VARIABLE GIT_TAG_NAME
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )

  if(GIT_TAG_MATCH EQUAL 0)
    set(MADBFS_VERSION_FULL "${PROJECT_VERSION}")
  else()
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
      OUTPUT_VARIABLE GIT_HASH
      OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE GIT_HASH_OK
      ERROR_QUIET
    )
    if(GIT_HASH_OK EQUAL 0)
      set(MADBFS_GIT_HASH ${GIT_HASH})
      set(MADBFS_VERSION_FULL "${PROJECT_VERSION}-dev+g${GIT_HASH}")
    endif()
  endif()
endif()

configure_file(
  ${CMAKE_CURRENT_LIST_DIR}/version.hpp.in
  ${CMAKE_BINARY_DIR}/generated/madbfs-gen/version.hpp
  @ONLY
)
