from conan import ConanFile
from conan.tools.cmake import cmake_layout


class Recipe(ConanFile):
    settings = ["os", "compiler", "build_type", "arch"]
    generators = ["CMakeToolchain", "CMakeDeps"]
    requires = [
        "asio/1.29.0",
        "fmt/11.1.3",
        "spdlog/1.15.1",
    ]

    def layout(self):
        cmake_layout(self)
