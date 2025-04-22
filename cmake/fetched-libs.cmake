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

# opt-iter
# --------
FetchContent_Declare(
    opt-iter
    GIT_REPOSITORY https://github.com/mrizaln/opt-iter
    GIT_TAG v0.1.0
)
FetchContent_MakeAvailable(opt-iter)

add_library(fetch::opt-iter ALIAS opt-iter)
# -------
