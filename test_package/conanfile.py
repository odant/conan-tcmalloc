from conans import ConanFile, CMake


class PackageTestConan(ConanFile):
    settings = "os", "compiler", "build_type", "arch"
    generators = "cmake"

    def build(self):
        cmake = CMake(self, build_type=self.settings.build_type, msbuild_verbosity="normal")
        cmake.verbose = True
        if (cmake.is_multi_configuration):
            cmake.definitions["CMAKE_BUILD_TYPE"] = cmake.build_type
        cmake.configure()
        cmake.build()

    def imports(self):
        self.copy("*.pdb", dst="bin", src="bin")
        self.copy("*.dll", dst="bin", src="bin")
        self.copy("*.so*", dst="bin", src="lib")

    def test(self):
        if self.settings.os == "Windows" and self.settings.compiler == "Visual Studio":
            self.run("ctest --verbose --build-config %s" % self.settings.build_type)
        else:
            self.run("ctest --verbose")
