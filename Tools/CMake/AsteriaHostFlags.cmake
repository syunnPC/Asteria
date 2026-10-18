# SPDX-License-Identifier: Apache-2.0
# Compiler flag sets for the hosted verification build.
#
# The nucleus sources are compiled freestanding exactly as on the targets;
# only test translation units are hosted. ASTERIA_PROFILE selects one of the
# hosted verification profiles.
#
# GCC builds every profile except Fuzz. Clang builds the Fuzz profile and the
# source-based Coverage profile; Analyze needs GCC's -fanalyzer.

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(ASTERIA_HOST_COMPILER GCC)
    set(asteria_cast_align_warning -Wcast-align=strict)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    set(ASTERIA_HOST_COMPILER Clang)
    # Clang has no =strict level; -Wcast-align already diagnoses every cast
    # that increases the required alignment, independent of the target.
    set(asteria_cast_align_warning -Wcast-align)
else()
    message(FATAL_ERROR
        "The hosted build supports GCC and Clang, not '${CMAKE_CXX_COMPILER_ID}'")
endif()

set(ASTERIA_WARNING_FLAGS
    -Wall
    -Wextra
    -Wpedantic
    -Werror
    -Wconversion
    -Wsign-conversion
    -Wshadow
    -Wundef
    ${asteria_cast_align_warning}
    -Wcast-qual
    -Wold-style-cast
    -Woverloaded-virtual
    -Wnon-virtual-dtor
)

set(ASTERIA_COMMON_LANGUAGE_FLAGS
    -fno-exceptions
    -fno-rtti
    -fno-threadsafe-statics
    -fno-use-cxa-atexit
    -fno-common
)

set(ASTERIA_FREESTANDING_FLAGS ${ASTERIA_COMMON_LANGUAGE_FLAGS} -ffreestanding)
set(ASTERIA_HOSTED_FLAGS ${ASTERIA_COMMON_LANGUAGE_FLAGS})

# The compile database lets tools (Tools/Mutation) rebuild one translation
# unit with its exact command.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(ASTERIA_PROFILES Debug Release Thread Analyze Coverage Fuzz)
if(NOT ASTERIA_PROFILE IN_LIST ASTERIA_PROFILES)
    message(FATAL_ERROR
        "ASTERIA_PROFILE must be one of ${ASTERIA_PROFILES}, not '${ASTERIA_PROFILE}'")
endif()
if((ASTERIA_PROFILE STREQUAL "Analyze") AND NOT (ASTERIA_HOST_COMPILER STREQUAL "GCC"))
    message(FATAL_ERROR "The Analyze profile runs GCC's -fanalyzer and needs GCC")
endif()
if((ASTERIA_PROFILE STREQUAL "Fuzz") AND NOT (ASTERIA_HOST_COMPILER STREQUAL "Clang"))
    message(FATAL_ERROR "The Fuzz profile links libFuzzer and needs Clang")
endif()

# Extra link flags of the libFuzzer harness executables (Fuzz profile only).
set(ASTERIA_FUZZER_LINK_FLAGS)

if(ASTERIA_PROFILE STREQUAL "Debug")
    set(ASTERIA_PROFILE_COMPILE_FLAGS
        -O1 -g
        -fsanitize=address,undefined
        -fno-sanitize-recover=undefined
        -fno-omit-frame-pointer)
    set(ASTERIA_PROFILE_LINK_FLAGS
        -fsanitize=address,undefined
        -fno-sanitize-recover=undefined)
elseif(ASTERIA_PROFILE STREQUAL "Release")
    set(ASTERIA_PROFILE_COMPILE_FLAGS -O2 -DNDEBUG -flto=auto)
    set(ASTERIA_PROFILE_LINK_FLAGS -flto=auto)
elseif(ASTERIA_PROFILE STREQUAL "Thread")
    set(ASTERIA_PROFILE_COMPILE_FLAGS
        -O1 -g
        -fsanitize=thread
        -fno-omit-frame-pointer)
    set(ASTERIA_PROFILE_LINK_FLAGS -fsanitize=thread)
elseif(ASTERIA_PROFILE STREQUAL "Analyze")
    # GCC's static analyzer runs during compilation; every diagnostic is an
    # error through -Werror. Tests are built but not required to run.
    set(ASTERIA_PROFILE_COMPILE_FLAGS -O0 -fanalyzer "$<$<COMPILE_LANGUAGE:CXX>:-fdump-tree-gimple-lineno;-fdump-lang-class>")
    set(ASTERIA_PROFILE_LINK_FLAGS)
elseif(ASTERIA_PROFILE STREQUAL "Coverage")
    if(ASTERIA_HOST_COMPILER STREQUAL "Clang")
        # Source-based coverage: each test writes its raw profile to the
        # LLVM_PROFILE_FILE that Tests/CMakeLists.txt assigns, and
        # Tools/Coverage/Report.py merges the profiles and reports Source/.
        set(ASTERIA_PROFILE_COMPILE_FLAGS
            -O0 -g
            -fprofile-instr-generate
            -fcoverage-mapping)
        set(ASTERIA_PROFILE_LINK_FLAGS -fprofile-instr-generate)
    else()
        # gcov fallback; the report tool supports only Clang profiles.
        set(ASTERIA_PROFILE_COMPILE_FLAGS -O0 -g --coverage)
        set(ASTERIA_PROFILE_LINK_FLAGS --coverage)
    endif()
elseif(ASTERIA_PROFILE STREQUAL "Fuzz")
    # Every translation unit carries libFuzzer's coverage instrumentation and
    # the sanitizers; only the harness executables link libFuzzer itself.
    # Other executables resolve the instrumentation hooks to the sanitizer
    # runtime's no-op definitions.
    set(ASTERIA_PROFILE_COMPILE_FLAGS
        -O1 -g
        -fsanitize=fuzzer-no-link,address,undefined
        -fno-sanitize-recover=undefined
        -fno-omit-frame-pointer)
    set(ASTERIA_PROFILE_LINK_FLAGS
        -fsanitize=address,undefined
        -fno-sanitize-recover=undefined)
    set(ASTERIA_FUZZER_LINK_FLAGS -fsanitize=fuzzer)
endif()

# Applies the flags for one compilation mode to a target.
#   MODE FREESTANDING: nucleus sources, exactly as compiled for the targets.
#   MODE HOSTED: test translation units using the host runtime.
function(asteria_apply_host_flags target mode)
    if(mode STREQUAL "FREESTANDING")
        set(language_flags ${ASTERIA_FREESTANDING_FLAGS})
    elseif(mode STREQUAL "HOSTED")
        set(language_flags ${ASTERIA_HOSTED_FLAGS})
    else()
        message(FATAL_ERROR "Unknown compilation mode '${mode}' for ${target}")
    endif()
    target_compile_options(${target} PRIVATE
        ${language_flags}
        ${ASTERIA_WARNING_FLAGS}
        ${ASTERIA_PROFILE_COMPILE_FLAGS})
    get_target_property(target_type ${target} TYPE)
    if(target_type STREQUAL "EXECUTABLE")
        target_link_options(${target} PRIVATE ${ASTERIA_PROFILE_LINK_FLAGS})
    endif()
endfunction()

# Applies fixed native code-generation flags without the profile's sanitizer
# or analyzer options; OPTIMIZATION is O0 or O2.
function(asteria_apply_native_flags target mode optimization)
    if(mode STREQUAL "FREESTANDING")
        set(language_flags ${ASTERIA_FREESTANDING_FLAGS})
    else()
        set(language_flags ${ASTERIA_HOSTED_FLAGS})
    endif()
    target_compile_options(${target} PRIVATE
        ${language_flags}
        ${ASTERIA_WARNING_FLAGS}
        -${optimization})
endfunction()
