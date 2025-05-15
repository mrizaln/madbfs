from conan import ConanFile
from conan.tools.cmake import cmake_layout


class Recipe(ConanFile):
    settings = ["os", "compiler", "build_type", "arch"]
    generators = ["CMakeToolchain", "CMakeDeps"]
    requires = [
        "boost/1.87.0",
        "fmt/11.0.2",
        "libfuse/3.16.2",
        "rapidhash/1.0",
        "spdlog/1.15.0",
    ]
    test_requires = ["boost-ext-ut/1.1.9"]

    def layout(self):
        cmake_layout(self)
