set(FETCHCONTENT_QUIET FALSE)

include(FetchContent)

# dtl-modern library
# ------------------
FetchContent_Declare(
  dtl-modern
  GIT_REPOSITORY https://github.com/mrizaln/dtl-modern
  GIT_TAG v1.0.1)
FetchContent_MakeAvailable(dtl-modern)

add_library(fetch::dtl-modern ALIAS dtl-modern)
# ------------------
