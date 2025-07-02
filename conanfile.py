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
    "program_options",
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
    requires = [
        "boost/1.87.0",
        "fmt/11.1.3",
        "libfuse/3.16.2",
        "rapidhash/1.0",
        "spdlog/1.15.1",
    ]
    test_requires = ["boost-ext-ut/1.1.9"]
    default_options = BOOST_DEFAULT_OPTIONS

    def layout(self):
        cmake_layout(self)
