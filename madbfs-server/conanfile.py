from conan import ConanFile
from conan.errors import ConanException
from conan.tools.cmake import cmake_layout

# https://developer.android.com/ndk/guides/abis
SUPPORTED_ARCH = ["armv7", "armv8", "x86", "x86_64"]


class Recipe(ConanFile):
    settings = ["os", "compiler", "build_type", "arch"]
    generators = ["CMakeToolchain", "CMakeDeps"]

    requires = [
        "asio/1.36.0",  # asio/1.38.0 has a bug in its std::aligned_alloc detection
        "spdlog/1.17.0",
    ]

    def layout(self):
        arch = self.settings.get_safe("arch")  # pyright: ignore
        if arch is None or arch not in SUPPORTED_ARCH:
            raise ConanException(f"arch should be one of these: {SUPPORTED_ARCH}")

        self.folders.build_folder_vars = [  # pyright: ignore
            "settings.os",
            "settings.arch",
            "settings.build_type",
        ]
        cmake_layout(self)
