# SPDX-License-Identifier: Apache-2.0

# Cross toolchain for AMD64 UEFI images. Each tool honors its AMD64_* environment
# variable and otherwise resolves the cross-tool name through PATH.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(asteria_amd64_prefix "x86_64-elf-")
foreach(tool IN ITEMS CXX LD OBJCOPY NM READELF OBJDUMP)
    string(TOLOWER "${tool}" suffix)
    if(tool STREQUAL "CXX")
        set(suffix "g++")
    endif()
    if(DEFINED ENV{AMD64_${tool}})
        set(ASTERIA_TARGET_${tool} "$ENV{AMD64_${tool}}" CACHE FILEPATH "AMD64 ${tool}")
    else()
        set(ASTERIA_TARGET_${tool} "${asteria_amd64_prefix}${suffix}" CACHE FILEPATH "AMD64 ${tool}")
    endif()
    # Freeze the selected program so later PATH changes cannot mix toolchains.
    unset(asteria_amd64_resolved_tool)
    find_program(asteria_amd64_resolved_tool NAMES "${ASTERIA_TARGET_${tool}}" REQUIRED NO_CACHE)
    set(ASTERIA_TARGET_${tool} "${asteria_amd64_resolved_tool}" CACHE FILEPATH "AMD64 ${tool}" FORCE)
endforeach()
set(CMAKE_CXX_COMPILER "${ASTERIA_TARGET_CXX}")
# Preprocessed assembly (.S) goes through the same compiler driver.
set(CMAKE_ASM_COMPILER "${ASTERIA_TARGET_CXX}")
