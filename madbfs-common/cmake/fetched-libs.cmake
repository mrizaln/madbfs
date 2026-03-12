set(FETCHCONTENT_QUIET FALSE)

include(FetchContent)

# saf library
# ------------
FetchContent_Declare(
  saf
  URL https://github.com/ashtum/saf/archive/142c7e987d779babf0a895eda05faca231ed918f.zip
  URL_HASH SHA256=41c6294229f82eadfc9fdaa5848451f79b5db47b560e5312eb169f66ad7b591a
  DOWNLOAD_EXTRACT_TIMESTAMP ON
)
FetchContent_MakeAvailable(saf)

add_library(fetch::saf ALIAS saf)
# ------------
