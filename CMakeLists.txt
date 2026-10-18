# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.28)

project(Asteria LANGUAGES CXX)

# One build tree verifies one target. The hosted tree builds the nucleus
# freestanding and runs the unit, model, and native-context tests. Target
# trees cross-compile the kernel, user images, and UEFI applications.
set(ASTERIA_TARGET "Host" CACHE STRING "Build target: Host, Ia64, or Amd64")
set_property(CACHE ASTERIA_TARGET PROPERTY STRINGS Host Ia64 Amd64)

set(ASTERIA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(ASTERIA_TOOLS "${ASTERIA_ROOT}/Tools")
list(APPEND CMAKE_MODULE_PATH "${ASTERIA_TOOLS}/CMake")
include(AsteriaAsmLayout)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# C++ modules are not supported by these build profiles.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)

if(ASTERIA_TARGET STREQUAL "Host")
    set(ASTERIA_PROFILE "Debug" CACHE STRING
        "Hosted verification profile: Debug, Release, Thread, Analyze, Coverage, or Fuzz")
    set_property(CACHE ASTERIA_PROFILE PROPERTY STRINGS Debug Release Thread Analyze Coverage Fuzz)
    # The native AMD64 libraries assemble the kernel-context switch (.S).
    enable_language(ASM)
    include(AsteriaHostFlags)
    enable_testing()
    add_subdirectory(Source)
    add_subdirectory(Tests)
    add_subdirectory(Tools)
else()
    if(DEFINED ENV{HOST_CXX})
        set(ASTERIA_HOST_CXX "$ENV{HOST_CXX}" CACHE FILEPATH "Host compiler for build tools")
    else()
        set(ASTERIA_HOST_CXX "g++" CACHE FILEPATH "Host compiler for build tools")
    endif()
    enable_language(ASM)
    include(AsteriaTarget)
    enable_testing()
    add_subdirectory(Source)
endif()

# C23 public runtime headers use the cross C drivers directly; C remains disabled here.
add_test(NAME User/Runtime/HeaderCompile
    COMMAND "${Python3_EXECUTABLE}" "${ASTERIA_ROOT}/Tests/User/Runtime/HeaderCompile.py"
        "${ASTERIA_ROOT}/Source/User/Runtime/Include")
set_tests_properties(User/Runtime/HeaderCompile PROPERTIES LABELS static TIMEOUT 60)

add_test(NAME SourceStyle
    COMMAND "${Python3_EXECUTABLE}" "${ASTERIA_TOOLS}/Build/CheckStyle.py")
set_tests_properties(SourceStyle PROPERTIES LABELS static TIMEOUT 60)
