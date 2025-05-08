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

# target: Sockpp::sockpp
# -----------------------
set(SOCKPP_BUILD_STATIC ON)

FetchContent_Declare(
    sockpp
    GIT_REPOSITORY https://github.com/fpagliughi/sockpp
    GIT_TAG afdeacba9448c7a77194eed6ab8e1c0b1653c79a
)
FetchContent_MakeAvailable(sockpp)
# -----------------------
