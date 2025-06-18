# TCMalloc Conan package
# Dmitriy Vetutnev, ODANT 2018

from conan import ConanFile
from conan.errors import ConanInvalidConfiguration, ConanException
from conan.tools.cmake import CMake, CMakeToolchain, cmake_layout
from conan.tools.files import (
    apply_conandata_patches,
    copy,
    export_conandata_patches,
    get,
    replace_in_file,
    rmdir
)
from conan.tools.microsoft import (
    MSBuild, MSBuildDeps, MSBuildToolchain, VCVars, vs_layout
)
from conan.tools.files import patch, save, chdir, collect_libs, copy

from datetime import datetime
import os, glob, shutil


def generateVersionH(version, current_time=datetime.now()):
    content = ""
    content += "#ifndef VERSIONNO__H\n"
    content += "#define VERSIONNO__H\n"
    content += "\n"
    content += "#define VERSION_FULL           " + version + "\n"
    content += "\n"
    content += "#define VERSION_DATE           " + "\"" + current_time.strftime("%Y-%m-%d") + "\"\n"
    content += "#define VERSION_TIME           " + "\"" + current_time.strftime("%H:%M:%S") + "\"\n"
    content += "\n"
    content += "#define VERSION_FILE           " + version.replace(".", ",") + "\n"
    content += "#define VERSION_PRODUCT        " + version.replace(".", ",") + "\n"
    content += "#define VERSION_FILESTR        " + "\"" + version + "\"\n"
    content += "#define VERSION_PRODUCTSTR     " + "\"" + version.rsplit(".", 2)[0] + "\"\n"
    content += "\n"
    content += "#endif\n"
    return content


class TCMallocConan(ConanFile):
    name = "tcmalloc"
    version = "2.17.0-rc1+0"
    license = "BSD 3-Clause"
    description = "Thread-Cached Malloc"
    url = "https://github.com/odant/conan-tcmalloc"
    settings = "os", "compiler", "build_type", "arch"
    options = {
        "dll_sign": [True, False]
    }
    default_options = { "dll_sign": True }
    exports_sources = "src/*", "libtcmalloc_minimal.vcxproj.patch", "tcmalloc.rc", "add_envvar_to_disable_patching.patch", "fix_win7_compatible.patch"
    no_copy_source = False
    build_policy = "missing"
    package_type = "shared-library"
    python_requires = "windows_signtool/[>=1.2]@odant/stable"
    
    def layout(self):
        cmake_layout(self, src_folder="src")
            
    def configure(self):
        # DLL sign
        if self.settings.os != "Windows":
            self.options.rm_safe("dll_sign")
        # Pure C library
        self.settings.compiler.rm_safe("libcxx")
        self.settings.compiler.rm_safe("cppstd")

    def build_requirements(self):
        self.tool_requires("ninja/[>=1.12.1]")
    
    def generate(self):
        tc = CMakeToolchain(self, generator="Ninja")
        tc.generate()

    def _patch_sources(self):
        self.output.info(f"self.source_folder = {self.source_folder}")
        self.output.info(f"self.export_sources_folder = {self.export_sources_folder}")
        patch(self, patch_file=os.path.join(self.export_sources_folder, "libtcmalloc_minimal.vcxproj.patch"))
        patch(self, patch_file=os.path.join(self.export_sources_folder, "add_envvar_to_disable_patching.patch"))
        if self.settings.os == "Windows":
            patch(self, patch_file=os.path.join(self.export_sources_folder, "fix_win7_compatible.patch"))

    def build(self):
        self._patch_sources()
        cmake = CMake(self)
        cmake.verbose = False
        cmake.configure()
        cmake.build(target="tcmalloc_minimal")

    def package(self):
        lib = os.path.join(self.package_folder, "lib")
        bin = os.path.join(self.package_folder, "bin")
        if self.settings.os == "Windows":
            copy(self, "tcmalloc*.lib", dst = lib, src = self.build_folder, keep_path = False)
            copy(self, "tcmalloc*.dll", dst = bin, src = self.build_folder, keep_path = False)
            copy(self, "tcmalloc*.pdb", dst = bin, src = self.build_folder, keep_path = False)
        else:    
            copy(self, "libtcmalloc_minimal.so*", dst = lib, src = self.build_folder, keep_path = False)
        copy(self, "*.h", dst=os.path.join(self.package_folder, "include"), src=os.path.join(self.source_folder, "src", "gperftools"), keep_path=False)
        # Sign DLL
        if self.options.get_safe("dll_sign"):
            self.python_requires["windows_signtool"].module.sign(self, [os.path.join(self.package_folder, "bin", "*.dll")])

    def package_info(self):
        self.output.info(f"before self.cpp_info.libdirs: {self.cpp_info.libdirs}")
        self.output.info(f"before self.cpp_info.bindirs: {self.cpp_info.bindirs}")
        #self.cpp_info.libdirs = []
        #self.cpp_info.bindirs = []
        self.cpp_info.libs = collect_libs(self)
        self.output.info(f"after self.cpp_info.libdirs: {self.cpp_info.libdirs}")
        self.output.info(f"after self.cpp_info.bindirs: {self.cpp_info.bindirs}")
