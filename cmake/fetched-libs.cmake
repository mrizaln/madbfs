set(FETCHCONTENT_QUIET FALSE)

include(FetchContent)

# target: saf
# --------------------
FetchContent_Declare(
    saf
    SYSTEM
    GIT_REPOSITORY https://github.com/ashtum/saf
    GIT_TAG 3643d80c9ce0eaec374416bd456637b07a90b4d4
    GIT_PROGRESS TRUE
)
FetchContent_MakeAvailable(saf)
# --------------------
