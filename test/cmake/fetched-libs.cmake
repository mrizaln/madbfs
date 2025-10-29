set(FETCHCONTENT_QUIET FALSE)

include(FetchContent)

# dtlx library
# ------------------
FetchContent_Declare(
  dtlx
  GIT_REPOSITORY https://github.com/mrizaln/dtlx
  GIT_TAG v2.0.0)
FetchContent_MakeAvailable(dtlx)

add_library(fetch::dtlx ALIAS dtlx)
# ------------------
