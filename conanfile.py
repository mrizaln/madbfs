from conan import ConanFile
from conan.tools.cmake import cmake_layout

# from boost recipe from conan-center-index
CONFIGURE_OPTIONS = (
    # "atomic",
    "charconv",
    "chrono",
    "cobalt",
    # "container",
    # "context",
    "contract",
    "coroutine",
    # "date_time",
    # "exception",
    "fiber",
    # "filesystem",
    "graph",
    "graph_parallel",
    "iostreams",
    # "json",
    "locale",
    "log",
    "math",
    "mpi",
    "nowide",
    # "process",
    # "program_options",
    "python",
    "random",
    "regex",
    "serialization",
    "stacktrace",
    # "system",
    "test",
    "thread",
    "timer",
    "type_erasure",
    "url",
    "wave",
)

BOOST_DEFAULT_OPTIONS = {
    "boost/*:system_no_deprecated": True,
    "boost/*:asio_no_deprecated": True,
    "boost/*:filesystem_no_deprecated": True,
    "boost/*:filesystem_use_std_fs": True,
} | dict((f"boost/*:without_{name}", True) for name in CONFIGURE_OPTIONS)


class Recipe(ConanFile):
    settings = ["os", "compiler", "build_type", "arch"]
    generators = ["CMakeToolchain", "CMakeDeps"]
    default_options = BOOST_DEFAULT_OPTIONS

    requires = [
        "boost/1.89.0",
        "libfuse/3.18.2",
        "rapidhash/3.0",
        "spdlog/1.17.0",
    ]

    test_requires = [
        "boost-ext-ut/2.3.1",
    ]

    def layout(self):
        cmake_layout(self)
