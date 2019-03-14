project(PackageTest CXX)
cmake_minimum_required(VERSION 3.0)

include(${CMAKE_BINARY_DIR}/conanbuildinfo.cmake)
conan_basic_setup(TARGETS)

enable_testing()


add_executable(test_package_CONAN_PKG test_package.cpp)
target_link_libraries(test_package_CONAN_PKG CONAN_PKG::tcmalloc)
set_target_properties(test_package_CONAN_PKG PROPERTIES LINK_FLAGS "/INCLUDE:__tcmalloc")
add_test(
    NAME test_package_CONAN_PKG
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    COMMAND test_package_CONAN_PKG
)


if("${CMAKE_BUILD_TYPE}" STREQUAL "Release")

    add_executable(binary_trees binary_trees.cc)
    add_test(
        NAME binary_trees
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin
        COMMAND binary_trees
    )

    add_executable(binary_trees_tcmalloc binary_trees.cc)
    target_link_libraries(binary_trees_tcmalloc CONAN_PKG::tcmalloc)
    set_target_properties(binary_trees_tcmalloc PROPERTIES LINK_FLAGS "/INCLUDE:__tcmalloc")
    add_test(
        NAME binary_trees_tcmalloc
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin
        COMMAND binary_trees_tcmalloc
    )

endif() # Build type Release
