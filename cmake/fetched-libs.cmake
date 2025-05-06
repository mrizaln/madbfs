set(FETCHCONTENT_QUIET FALSE)

include(FetchContent)

# shared_futex library
# ------------------
FetchContent_Declare(
    shared_futex
    SYSTEM
    GIT_REPOSITORY https://github.com/mrizaln/shared_futex
    GIT_TAG 0aaac10f126022312c26a1546bac06d7e81f3afc
)
FetchContent_MakeAvailable(shared_futex)

add_library(fetch::shared_futex ALIAS shared_futex)
# ------------------
