from conan import ConanFile
from conan.tools.build import cross_building, can_run
from conan.tools.cmake import CMake, CMakeToolchain, cmake_layout
from conan.tools.scm import Version
from conan.tools.files import chdir, copy
import os


class PackageTestConan(ConanFile):
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "VirtualRunEnv", "VirtualBuildEnv"

    def layout(self):
        cmake_layout(self)

    def build_requirements(self):
        self.tool_requires("ninja/[>=1.12.1]")

    def requirements(self):
        self.requires(self.tested_reference_str)

    def generate(self):
        tc = CMakeToolchain(self, generator="Ninja")
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def test(self):
        if can_run(self):
            with chdir(self, self.folders.build_folder):
                self.run(f"ctest --output-on-failure -C {self.settings.build_type}", env="conanrun")
