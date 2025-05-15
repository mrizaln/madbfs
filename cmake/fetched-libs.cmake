set(FETCHCONTENT_QUIET FALSE)

include(FetchContent)

# target: shared_futex
# --------------------
FetchContent_Declare(
    shared_futex
    SYSTEM
    GIT_REPOSITORY https://github.com/mrizaln/shared_futex
    GIT_TAG 0aaac10f126022312c26a1546bac06d7e81f3afc
    GIT_PROGRESS TRUE
)
FetchContent_MakeAvailable(shared_futex)
# --------------------
