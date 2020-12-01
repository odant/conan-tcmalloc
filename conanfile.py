# TCMalloc Conan package
# Dmitriy Vetutnev, ODANT 2018


from conans import ConanFile, MSBuild, tools
from conans.errors import ConanException
from datetime import datetime
import os, glob, shutil


def get_safe(options, name):
    try:
        return getattr(options, name, None)
    except ConanException:
        return None

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
    version = "2.8.20236.14001"
    license = "BSD 3-Clause"
    description = "Thread-Cached Malloc"
    url = "https://github.com/odant/conan-tcmalloc"
    settings = {
        "os": ["Windows"],
        "compiler": ["Visual Studio"],
        "build_type": ["Debug", "Release"],
        "arch": ["x86_64", "x86"]
    }
    options = {
        "dll_sign": [True, False]
    }
    default_options = "dll_sign=True"
    exports_sources = "src/*", "libtcmalloc_minimal.vcxproj.patch", "tcmalloc.rc"
    no_copy_source = False
    build_policy = "missing"

    def configure(self):
        # DLL sign
        if self.settings.os != "Windows":
            del self.options.dll_sign
        # Pure C library
        del self.settings.compiler.libcxx

    def build_requirements(self):
        if get_safe(self.options, "dll_sign"):
            self.build_requires("windows_signtool/[~=1.1]@%s/stable" % self.user)

    def source(self):
        tools.patch(patch_file="libtcmalloc_minimal.vcxproj.patch")

    def build(self):
        if self.settings.compiler == "Visual Studio":
            self.msvc_build()

    def msvc_build(self):
        version_h = generateVersionH(self.version)
        tools.save(os.path.join(self.build_folder, "src", "vsprojects", "libtcmalloc_minimal", "version.h"), version_h)
        shutil.copy(os.path.join(self.source_folder, "tcmalloc.rc"), os.path.join(self.build_folder, "src", "vsprojects", "libtcmalloc_minimal"))
        with tools.chdir("src"):
            builder = MSBuild(self)
            build_type = {
                "Release": "Release-Patch",
                "Debug": "Debug"
            }.get(str(self.settings.build_type))
            builder.build("gperftools.sln", upgrade_project=False, verbosity="normal", use_env=False, build_type=build_type, targets=["libtcmalloc_minimal"])

    def package(self):
        for releasePath in [ "src/x64/Release-Patch", "src/Win32/Release-Patch" ]:
            self.copy("tcmalloc.lib", dst="lib", src=releasePath, keep_path=False)
            self.copy("tcmalloc.dll", dst="bin", src=releasePath, keep_path=False)
            self.copy("tcmalloc.pdb", dst="bin", src=releasePath, keep_path=False)
        for debugPath in [ "src/x64/Debug", "src/Win32/Debug" ]:
            self.copy("tcmallocd.lib", dst="lib", src=debugPath, keep_path=False)
            self.copy("tcmallocd.dll", dst="bin", src=debugPath, keep_path=False)
            self.copy("tcmallocd.pdb", dst="bin", src=debugPath, keep_path=False)
        # Sign DLL
        if get_safe(self.options, "dll_sign"):
            import windows_signtool
            pattern = os.path.join(self.package_folder, "bin", "*.dll")
            for fpath in glob.glob(pattern):
                fpath = fpath.replace("\\", "/")
                for alg in ["sha1", "sha256"]:
                    is_timestamp = True if self.settings.build_type == "Release" else False
                    cmd = windows_signtool.get_sign_command(fpath, digest_algorithm=alg, timestamp=is_timestamp)
                    self.output.info("Sign %s" % fpath)
                    self.run(cmd)

    def package_info(self):
        self.cpp_info.libs = tools.collect_libs(self)
