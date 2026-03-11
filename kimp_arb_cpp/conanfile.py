from conan import ConanFile
from conan.tools.cmake import cmake_layout


class KimpArbCppConan(ConanFile):
    name = "kimp_arb_cpp"
    version = "1.0.0"
    package_type = "application"

    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeToolchain", "CMakeDeps"

    requires = (
        "boost/1.86.0",
        "openssl/3.3.2",
        "simdjson/3.12.2",
        "spdlog/1.15.3",
        "fmt/11.2.0",
        "yaml-cpp/0.8.0",
    )

    default_options = {
        "boost/*:header_only": False,
    }

    def layout(self):
        cmake_layout(self)
