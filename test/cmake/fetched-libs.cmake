set(FETCHCONTENT_QUIET FALSE)

include(FetchContent)

# dtlx library
# ------------------
FetchContent_Declare(
  dtlx
  URL https://github.com/mrizaln/dtlx/archive/refs/tags/v2.0.0.zip
  URL_HASH SHA256=030d28dee3207bba0eb4744c647e9f316d468d074364e3de9548c795977765a0
  DOWNLOAD_EXTRACT_TIMESTAMP ON
)
FetchContent_MakeAvailable(dtlx)

add_library(fetch::dtlx ALIAS dtlx)
# ------------------
