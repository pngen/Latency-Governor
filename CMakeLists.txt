cmake_minimum_required(VERSION 3.24)

project(latency_governor
    VERSION 1.0.0
    DESCRIPTION "A vendor-neutral runtime for enforcing explicit latency SLOs across heterogeneous AI serving infrastructure."
    LANGUAGES CXX)

option(LATENCY_GOVERNOR_ENABLE_CUDA "Build the CUDA backend for accelerator-side validation." OFF)
option(LATENCY_GOVERNOR_BUILD_TESTS "Build the test suite." ON)
option(LATENCY_GOVERNOR_BUILD_EXAMPLES "Build the runnable examples." ON)
option(LATENCY_GOVERNOR_BUILD_BENCHMARKS "Build the benchmarks." ON)
option(LATENCY_GOVERNOR_BUILD_APPS "Build the coordinator/worker/ctl applications." ON)

if(LATENCY_GOVERNOR_ENABLE_CUDA)
    enable_language(CUDA)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
enable_testing()

function(lg_apply_warnings tgt)
    if(MSVC)
        target_compile_options(${tgt} PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /EHsc)
    else()
        target_compile_options(${tgt} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endfunction()

# ---------------------------------------------------------------------------
# Core static library.
# ---------------------------------------------------------------------------
set(LG_CORE_SOURCES
    src/core/clock.cpp
    src/core/request.cpp
    src/core/snapshot.cpp
    src/core/explanation.cpp
    src/core/serialization.cpp
    src/persistence/persistence.cpp
    src/policy/policy.cpp
    src/policy/governor.cpp
    src/prediction/predictor.cpp
    src/risk/risk.cpp
    src/fairness/fairness.cpp)

add_library(latency_governor STATIC ${LG_CORE_SOURCES})
add_library(latency_governor::latency_governor ALIAS latency_governor)
target_include_directories(latency_governor PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_definitions(latency_governor PUBLIC LATENCY_GOVERNOR_VERSION_STRING="1.0.0")
set_target_properties(latency_governor PROPERTIES
    OUTPUT_NAME latency_governor
    POSITION_INDEPENDENT_CODE ON)
lg_apply_warnings(latency_governor)

if(MSVC)
    target_link_libraries(latency_governor PUBLIC ws2_32)
endif()

foreach(sub src/protocol src/backends src/coordinator src/worker src/cli tests)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${sub}/CMakeLists.txt")
        add_subdirectory(${sub})
    endif()
endforeach()
if(LATENCY_GOVERNOR_BUILD_APPS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/apps/CMakeLists.txt")
    add_subdirectory(apps)
endif()
if(LATENCY_GOVERNOR_BUILD_EXAMPLES AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/CMakeLists.txt")
    add_subdirectory(examples)
endif()
if(LATENCY_GOVERNOR_BUILD_BENCHMARKS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/CMakeLists.txt")
    add_subdirectory(benchmarks)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS latency_governor
    EXPORT LatencyGovernorTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(DIRECTORY include/latency_governor
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/LatencyGovernorConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/LatencyGovernorConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/LatencyGovernorConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/LatencyGovernor)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/LatencyGovernorConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/LatencyGovernorConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/LatencyGovernor)

install(EXPORT LatencyGovernorTargets
    FILE LatencyGovernorTargets.cmake
    NAMESPACE LatencyGovernor::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/LatencyGovernor)
