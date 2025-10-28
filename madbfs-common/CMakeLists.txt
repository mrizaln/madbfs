option(MADBFS_USE_NON_BOOST_ASIO "use non-boost version of asio" OFF)
option(MADBFS_BUILD_IPC "build ipc (requires boost json)" OFF)
option(
    MADBFS_ENABLE_RAPIDHASH_BLANKET_IMPL
    "enable blanket implementation of std::hash using rapidhash"
    OFF
)

if(MADBFS_USE_NON_BOOST_ASIO)
    set(MADBFS_ASIO_TARGET asio::asio)
    find_package(asio REQUIRED)
else()
    set(MADBFS_ASIO_TARGET boost::boost)
    find_package(Boost REQUIRED)
endif()

if(MADBFS_ENABLE_RAPIDHASH_BLANKET_IMPL)
    set(MADBFS_RAPIDHASH_TARGET rapidhash::rapidhash)
    find_package(rapidhash REQUIRED)
endif()

find_package(spdlog REQUIRED)
find_package(fmt REQUIRED)

include(cmake/fetched-libs.cmake)

add_library(madbfs-common STATIC src/rpc.cpp)
if(MADBFS_BUILD_IPC)
    target_sources(madbfs-common PRIVATE src/ipc.cpp)
endif()

target_link_libraries(
    madbfs-common
    PUBLIC
        fetch::saf
        fmt::fmt
        spdlog::spdlog
        ${MADBFS_ASIO_TARGET}
        ${MADBFS_RAPIDHASH_TARGET}
)

target_include_directories(madbfs-common PUBLIC include)
target_compile_features(madbfs-common PUBLIC cxx_std_23)
set_target_properties(madbfs-common PROPERTIES CXX_EXTENSIONS OFF)

target_compile_definitions(
    madbfs-common
    PUBLIC MADBFS_VERSION_STRING="${CMAKE_PROJECT_VERSION}"
)

if(MADBFS_ENABLE_RAPIDHASH_BLANKET_IMPL)
    target_compile_definitions(madbfs-common PUBLIC MADBFS_RAPIDHASH_ENABLED=1)
endif()

if(MADBFS_USE_NON_BOOST_ASIO)
    target_compile_definitions(madbfs-common PUBLIC MADBFS_NON_BOOST_ASIO=1)
    target_compile_definitions(madbfs-common PUBLIC SAF_ASIO_STANDALONE)
endif()

if(MADBFS_BUILD_IPC)
    target_compile_definitions(madbfs-common PUBLIC MADBFS_BUILD_IPC=1)
endif()
