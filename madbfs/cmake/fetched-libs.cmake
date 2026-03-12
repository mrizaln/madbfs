set(FETCHCONTENT_QUIET FALSE)

include(FetchContent)

# linr library
# ------------
FetchContent_Declare(
  linr
  URL https://github.com/mrizaln/linr/archive/refs/tags/v0.1.0.zip
  URL_HASH SHA256=0a48d742faf8e7afaf3c4a8ecda7dd6848b6eb80a1fb073ffc9e0d4d21be8fc5
  DOWNLOAD_EXTRACT_TIMESTAMP ON
)
FetchContent_MakeAvailable(linr)

add_library(fetch::linr ALIAS linr)
# ------------
