set(FETCHCONTENT_QUIET FALSE)

include(FetchContent)

# subprocess
# ----------
FetchContent_Declare(
    subprocess
    GIT_REPOSITORY https://github.com/benman64/subprocess
    GIT_TAG 1b35a489e92ebe261ce781a65d7bf47d71be3655
)
FetchContent_MakeAvailable(subprocess)

add_library(fetch::subprocess ALIAS subprocess)
# ----------
