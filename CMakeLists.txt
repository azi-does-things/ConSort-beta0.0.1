cmake_minimum_required(VERSION 3.14.0)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

project(ConSort VERSION 0.0.1 LANGUAGES CXX)

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

# Project options
option(BUILD_TESTING "Build the tests" ON)
option(BUILD_BENCHMARKS "Build the benchmarks" OFF)

# Create consort::ConSort as an interface library
add_library(ConSort INTERFACE)

target_include_directories(ConSort INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_features(ConSort INTERFACE cxx_std_20)

add_library(consort::ConSort ALIAS ConSort)

# Install targets and files
install(
    TARGETS ConSort
    EXPORT consort-targets
    DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(
    EXPORT consort-targets
    NAMESPACE consort::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/consort
)

install(
    FILES ${CMAKE_CURRENT_SOURCE_DIR}/include/consort/consort.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/consort
)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/consort-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/cmake/consort-config.cmake
    INSTALL_DESTINATION
        ${CMAKE_INSTALL_LIBDIR}/cmake/consort
)

write_basic_package_version_file(
    ${CMAKE_BINARY_DIR}/cmake/consort-config-version.cmake
    COMPATIBILITY SameMajorVersion
    ARCH_INDEPENDENT
)

install(
    FILES
        ${CMAKE_CURRENT_BINARY_DIR}/cmake/consort-config.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/cmake/consort-config-version.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/consort
)

# Export target so that it can be used in subdirectories
export(
    EXPORT consort-targets
    FILE ${CMAKE_CURRENT_BINARY_DIR}/cmake/consort-targets.cmake
    NAMESPACE consort::
)

# Build tests and/or benchmarks if this is the main project
if (PROJECT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    if (BUILD_TESTING)
        enable_testing()
        add_subdirectory(tests)
    endif()

    if (BUILD_BENCHMARKS)
        add_subdirectory(benchmarks)
    endif()
endif()
