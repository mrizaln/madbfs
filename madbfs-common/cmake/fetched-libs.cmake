set(FETCHCONTENT_QUIET FALSE)

include(FetchContent)

# saf library
# ------------
FetchContent_Declare(
  saf
  GIT_REPOSITORY https://github.com/ashtum/saf
  GIT_TAG 142c7e987d779babf0a895eda05faca231ed918f)
FetchContent_MakeAvailable(saf)

add_library(fetch::saf ALIAS saf)
# ------------
