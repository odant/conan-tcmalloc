cmake_minimum_required(VERSION 3.15)
project(PackageTest CXX)

find_package(tcmalloc REQUIRED)

enable_testing()

add_executable(test_package_CONAN_PKG test_package.cpp)
target_link_libraries(test_package_CONAN_PKG tcmalloc::tcmalloc)

if (WIN32)
    set_target_properties(test_package_CONAN_PKG PROPERTIES LINK_FLAGS "/INCLUDE:__tcmalloc")
endif()

add_test(
    NAME test_package_CONAN_PKG
    COMMAND test_package_CONAN_PKG
)


if("${CMAKE_BUILD_TYPE}" STREQUAL "Release")

    add_executable(binary_trees binary_trees.cc)
    add_test(
        NAME binary_trees
        COMMAND binary_trees
    )

    add_executable(binary_trees_tcmalloc binary_trees.cc)
    target_link_libraries(binary_trees_tcmalloc tcmalloc::tcmalloc)
    if (WIN32)
        set_target_properties(binary_trees_tcmalloc PROPERTIES LINK_FLAGS "/INCLUDE:__tcmalloc")
    endif()
    add_test(
        NAME binary_trees_tcmalloc
        COMMAND binary_trees_tcmalloc
    )

endif() # Build type Release
