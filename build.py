# Build Conan package
# Dmitriy Vetutnev, ODANT, 2020


from conan.packager import ConanMultiPackager
import os


env_pure_c = os.getenv("CONAN_PURE_C", True)
pure_c = True if str(env_pure_c).lower() != "false" else False


if __name__ == "__main__":
    builder = ConanMultiPackager(
        exclude_vcvars_precommand=True
    )
    builder.add_common_builds(
        pure_c=pure_c
    )
    builder.remove_build_if(
        lambda build: build.settings.get("compiler.libcxx") == "libstdc++"
    )
    builder.run()
