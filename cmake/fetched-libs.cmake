set(FETCHCONTENT_QUIET FALSE)

include(FetchContent)

# linr library
# ------------
FetchContent_Declare(
  linr
  GIT_REPOSITORY https://github.com/mrizaln/linr
  GIT_TAG v0.1.0)
FetchContent_MakeAvailable(linr)

add_library(fetch::linr ALIAS linr)
# ------------

# saf library
# ------------
FetchContent_Declare(
  saf
  GIT_REPOSITORY https://github.com/ashtum/saf
  GIT_TAG 142c7e987d779babf0a895eda05faca231ed918f)
FetchContent_MakeAvailable(saf)

add_library(fetch::saf ALIAS saf)
# ------------
