# SPDX-License-Identifier: Apache-2.0
# Hosted tests. Every *Tests.cpp below this directory is discovered and registered with CTest; there
# is no manifest to keep in sync. The tables below name only the tests that need a different
# configuration. Guest/ holds the guest test program, which the target trees build into their test
# firmware.

find_package(Threads REQUIRED)

add_library(AsteriaTestSupport INTERFACE)
target_include_directories(AsteriaTestSupport INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")

file(GLOB_RECURSE asteria_test_sources CONFIGURE_DEPENDS
    RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/*Tests.cpp")
list(FILTER asteria_test_sources EXCLUDE REGEX "^Guest/")

# Compile-time contracts with no runnable path; the target builds compile
# them again with each ISA's ABI.
set(ASTERIA_COMPILE_ONLY_TESTS
    Target/AbiContractTests.cpp
)

# Cooperative kernel-context switches executed natively on an x86-64 host at
# both target optimization levels, without sanitizer runtimes.
set(ASTERIA_AMD64_NATIVE_TESTS
    Platform/Amd64/Scheduler/Amd64KernelContextNativeTests.cpp
    Platform/Amd64/Scheduler/Amd64ProcessorRunLoopNativeTests.cpp
)

# Hosted models of AMD64 platform code that use hosted stand-ins.
set(ASTERIA_AMD64_HOSTED_MODEL_TESTS
    Platform/Amd64/Device/Amd64MmioWindowTests.cpp
    Platform/Amd64/Executable/Amd64InitialUserImageTests.cpp
    Platform/Amd64/Interrupt/Amd64ApplicationProcessorStartupTests.cpp
    Platform/Amd64/Interrupt/Amd64UserEntryTests.cpp
)

# Tests of the AMD64 UEFI application and the shared AHCI firmware capture,
# which build with the AMD64 target configuration.
set(ASTERIA_AMD64_UEFI_TESTS
    Boot/Uefi/BootConsoleTests.cpp
    Boot/Uefi/GraphicsModeChoiceTests.cpp
    Boot/Uefi/BootApplicationIntegrationTests.cpp
    Boot/Uefi/AcpiTableDumpTests.cpp
    Boot/Uefi/RootBridgeReportTests.cpp
    Platform/Firmware/AhciPciResourceCaptureTests.cpp
)

# Tests that exercise the freestanding memory routines themselves.
set(ASTERIA_NO_BUILTIN_TESTS
    Runtime/MemoryTests.cpp
    Platform/Ia64/Firmware/SalSystemTableCaptureTests.cpp
)

# Tests that link owners whose unreferenced IA-64 leaf callers must be
# discarded by section garbage collection.
set(ASTERIA_GC_SECTIONS_TESTS
    Platform/Ia64/Scheduler/Ia64UserExecutionProbeTests.cpp
    Platform/Ia64/Scheduler/Ia64BspKernelRuntimeTests.cpp
)

# Tests that run once per argv mode, each in its own process.
set(ASTERIA_TEST_MODES_Kernel/Memory/PagerChannelTests all frame-rights)
set(ASTERIA_TEST_MODES_Platform/Ia64/Memory/Ia64BspRuntimeMemoryTests
    success invalid-charge-owner activation-rejected activation-unknown destroyed-root)
set(ASTERIA_TEST_MODES_Platform/Ia64/Memory/Ia64ApplicationProcessorRuntimeMappingOwnerTests
    smp1 smp2 smp3 smp4 shootdown-failure)
set(ASTERIA_TEST_MODES_Platform/Device/PhysicalMemoryMappedIoTests ia64 amd64)
set(ASTERIA_TEST_MODES_Platform/Device/PciEcamRuntimeTests ia64 amd64)

set(ASTERIA_HOST_TEST_STACK_BUDGET_BYTES 262144)
set(asteria_stack_probe_objects)

# Scenario sources are verification code, even when hosted models exercise them.
file(GLOB asteria_pager_scenarios CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Guest/Pager/*.cpp")
list(FILTER asteria_pager_scenarios EXCLUDE REGEX "PagerTestImage[.]cpp$")
add_library(AsteriaPagerScenarioSupport STATIC ${asteria_pager_scenarios})
target_link_libraries(AsteriaPagerScenarioSupport PRIVATE AsteriaHostNucleus AsteriaTestSupport)
asteria_apply_host_flags(AsteriaPagerScenarioSupport FREESTANDING)

function(asteria_configure_test_unit target relative)
    target_link_libraries(${target} PRIVATE AsteriaTestSupport)
    if(relative MATCHES "^User/Imaging/")
        target_compile_definitions(${target} PRIVATE
            ASTERIA_CURSOR_DATA_DIR="${PROJECT_SOURCE_DIR}/Data/Cursors/Default")
    endif()
    if(relative MATCHES "^User/Toolkit/")
        target_compile_definitions(${target} PRIVATE
            ASTERIA_TOOLKIT_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/User/Toolkit/Golden"
            ASTERIA_FONT_DATA_DIR="${PROJECT_SOURCE_DIR}/Data/Fonts/Vegur")
    endif()
    if(relative MATCHES "^User/(Font/|Session/(SceneCorpusTests|UiTextTests))")
        target_compile_definitions(${target} PRIVATE
            ASTERIA_FONT_DATA_DIR="${PROJECT_SOURCE_DIR}/Data/Fonts/Vegur"
            ASTERIA_FONT_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/User/Font/Golden")
    endif()
    if(relative MATCHES "^Acpi/Aml/")
        target_compile_definitions(${target} PRIVATE
            ASTERIA_AML_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/Acpi/Aml/Fixtures")
    endif()
    if(relative MATCHES "^User/(Session|Terminal)/")
        target_compile_definitions(${target} PRIVATE
            ASTERIA_RENDER_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/User")
    endif()
    if(relative IN_LIST ASTERIA_AMD64_HOSTED_MODEL_TESTS)
        target_link_libraries(${target} PRIVATE AsteriaHostAmd64Models)
    endif()
    if(relative IN_LIST ASTERIA_AMD64_UEFI_TESTS)
        target_link_libraries(${target} PRIVATE AsteriaHostAmd64Uefi)
    endif()
    if(relative IN_LIST ASTERIA_NO_BUILTIN_TESTS OR relative MATCHES "^User/Libc/")
        target_compile_options(${target} PRIVATE -fno-builtin)
    endif()
    target_link_libraries(${target} PRIVATE AsteriaPagerScenarioSupport AsteriaHostNucleus Threads::Threads)
endfunction()

foreach(relative IN LISTS asteria_test_sources)
    string(REGEX REPLACE "\\.cpp$" "" test_path "${relative}")
    string(MAKE_C_IDENTIFIER "${test_path}" target)
    get_filename_component(test_name "${test_path}" NAME)
    get_filename_component(test_directory "${test_path}" DIRECTORY)

    if(relative IN_LIST ASTERIA_COMPILE_ONLY_TESTS)
        add_library(${target} OBJECT "${relative}")
        target_link_libraries(${target} PRIVATE AsteriaHostNucleus)
        target_compile_definitions(${target} PRIVATE ASTERIA_TARGET_AMD64=1)
        asteria_apply_host_flags(${target} FREESTANDING)
        continue()
    endif()

    if(relative IN_LIST ASTERIA_AMD64_NATIVE_TESTS)
        foreach(optimization IN ITEMS O0 O2)
            set(native_target ${target}${optimization})
            add_executable(${native_target} "${relative}")
            set_target_properties(${native_target} PROPERTIES
                OUTPUT_NAME ${test_name}${optimization}
                RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${test_directory}")
            target_link_libraries(${native_target} PRIVATE
                AsteriaTestSupport AsteriaHostAmd64Native${optimization})
            asteria_apply_native_flags(${native_target} HOSTED ${optimization})
            add_test(NAME ${test_path}${optimization} COMMAND ${native_target})
            set_tests_properties(${test_path}${optimization} PROPERTIES
                LABELS "unit;native" TIMEOUT 5)
        endforeach()
    else()
        add_executable(${target} "${relative}")
        set_target_properties(${target} PROPERTIES
            OUTPUT_NAME ${test_name}
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${test_directory}")
        asteria_configure_test_unit(${target} "${relative}")
        asteria_apply_host_flags(${target} HOSTED)
        if(relative IN_LIST ASTERIA_GC_SECTIONS_TESTS)
            target_link_options(${target} PRIVATE -Wl,--gc-sections)
        endif()

        if(DEFINED ASTERIA_TEST_MODES_${test_path})
            foreach(mode IN LISTS ASTERIA_TEST_MODES_${test_path})
                add_test(NAME ${test_path}:${mode} COMMAND ${target} ${mode})
                set_tests_properties(${test_path}:${mode} PROPERTIES LABELS unit TIMEOUT 600)
            endforeach()
        else()
            add_test(NAME ${test_path} COMMAND ${target})
            set_tests_properties(${test_path} PROPERTIES LABELS unit TIMEOUT 600)
        endif()
    endif()

    # Stack probe: every runnable test translation unit at -O2 with
    # -fstack-usage, independent of the sanitizer profile.
    if(ASTERIA_PROFILE STREQUAL "Debug")
        set(probe ${target}StackProbe)
        add_library(${probe} OBJECT "${relative}")
        asteria_configure_test_unit(${probe} "${relative}")
        if(relative IN_LIST ASTERIA_AMD64_NATIVE_TESTS)
            target_link_libraries(${probe} PRIVATE AsteriaHostAmd64NativeO2)
            target_compile_options(${probe} PRIVATE -mgeneral-regs-only -mno-red-zone)
        endif()
        target_compile_options(${probe} PRIVATE
            ${ASTERIA_HOSTED_FLAGS} ${ASTERIA_WARNING_FLAGS} -O2 -fstack-usage)
        set_target_properties(${probe} PROPERTIES FOLDER StackProbes)
        list(APPEND asteria_stack_probe_objects "$<TARGET_OBJECTS:${probe}>")
    endif()
endforeach()

# The FAT volume library against dosfstools and mtools: FatToolImages.py makes images before
# FatToolInteropTests reads them and keeps the volumes it writes, then checks those with fsck.fat
# and mtype. All three print "SKIP:" and are reported skipped where the tools are absent. The images
# are sparse files under this build tree.
set(asteria_fat_images "${CMAKE_CURRENT_BINARY_DIR}/User/Filesystem/Fat/ToolImages")
set(asteria_fat_tool "${CMAKE_CURRENT_SOURCE_DIR}/User/Filesystem/Fat/FatToolImages.py")
add_test(NAME User/Filesystem/Fat/FatToolImagesMake
    COMMAND "${Python3_EXECUTABLE}" "${asteria_fat_tool}" make "${asteria_fat_images}")
add_test(NAME User/Filesystem/Fat/FatToolImagesCheck
    COMMAND "${Python3_EXECUTABLE}" "${asteria_fat_tool}" check "${asteria_fat_images}")
set_tests_properties(User/Filesystem/Fat/FatToolImagesMake PROPERTIES
    FIXTURES_SETUP AsteriaFatToolImages LABELS unit TIMEOUT 300 SKIP_REGULAR_EXPRESSION "SKIP: ")
set_tests_properties(User/Filesystem/Fat/FatToolInteropTests PROPERTIES
    FIXTURES_REQUIRED AsteriaFatToolImages ENVIRONMENT "ASTERIA_FAT_IMAGE_DIR=${asteria_fat_images}"
    SKIP_REGULAR_EXPRESSION "SKIP: ")
set_tests_properties(User/Filesystem/Fat/FatToolImagesCheck PROPERTIES
    FIXTURES_CLEANUP AsteriaFatToolImages LABELS unit TIMEOUT 600 SKIP_REGULAR_EXPRESSION "SKIP: ")

# The exFAT volume library against exfatprogs, as the FAT block above: ExfatToolImages.py makes the
# mkfs.exfat images the exFAT tests read; ExfatToolInteropTests keeps the volumes it writes, which
# the check runs fsck.exfat and its own decoder on. All of them print "SKIP:" and are reported
# skipped where exfatprogs is absent.
set(asteria_exfat_images "${CMAKE_CURRENT_BINARY_DIR}/User/Filesystem/Exfat/ToolImages")
set(asteria_exfat_tool "${CMAKE_CURRENT_SOURCE_DIR}/User/Filesystem/Exfat/ExfatToolImages.py")
add_test(NAME User/Filesystem/Exfat/ExfatToolImagesMake
    COMMAND "${Python3_EXECUTABLE}" "${asteria_exfat_tool}" make "${asteria_exfat_images}")
add_test(NAME User/Filesystem/Exfat/ExfatToolImagesCheck
    COMMAND "${Python3_EXECUTABLE}" "${asteria_exfat_tool}" check "${asteria_exfat_images}")
set_tests_properties(User/Filesystem/Exfat/ExfatToolImagesMake PROPERTIES
    FIXTURES_SETUP AsteriaExfatToolImages LABELS unit TIMEOUT 300 SKIP_REGULAR_EXPRESSION "SKIP: ")
foreach(test IN ITEMS ExfatToolInteropTests ExfatVolumeTests ExfatVolumeModelTests)
    set_tests_properties(User/Filesystem/Exfat/${test} PROPERTIES
        FIXTURES_REQUIRED AsteriaExfatToolImages ENVIRONMENT "ASTERIA_EXFAT_IMAGE_DIR=${asteria_exfat_images}"
        SKIP_REGULAR_EXPRESSION "SKIP: ")
endforeach()
set_tests_properties(User/Filesystem/Exfat/ExfatToolImagesCheck PROPERTIES
    FIXTURES_CLEANUP AsteriaExfatToolImages LABELS unit TIMEOUT 600 SKIP_REGULAR_EXPRESSION "SKIP: ")

if(ASTERIA_PROFILE STREQUAL "Debug")
    add_test(NAME HostTestStackUsage
        COMMAND "${Python3_EXECUTABLE}" "${ASTERIA_TOOLS}/Build/CheckStackUsage.py"
            --limit ${ASTERIA_HOST_TEST_STACK_BUDGET_BYTES} ${asteria_stack_probe_objects})
    set_tests_properties(HostTestStackUsage PROPERTIES LABELS "static")
endif()



# Fuzz harnesses (Tests/Fuzz/*Fuzz.cpp) define LLVMFuzzerTestOneInput and AsteriaFuzzSeeds. Each
# links the deterministic driver, which replays the harness seeds and applies a fixed number of
# seeded mutations as a regression test. The Fuzz profile (Clang) also links each harness against
# libFuzzer: its smoke test writes the seeds with the driver and fuzzes from them for a bounded
# number of runs; Documentation/Readme.md shows the command for open-ended campaigns.
file(GLOB_RECURSE asteria_fuzz_sources CONFIGURE_DEPENDS
    RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/Fuzz/*Fuzz.cpp")
foreach(relative IN LISTS asteria_fuzz_sources)
    string(REGEX REPLACE "\\.cpp$" "" fuzz_path "${relative}")
    string(MAKE_C_IDENTIFIER "${fuzz_path}" target)
    get_filename_component(fuzz_name "${fuzz_path}" NAME)
    if(ASTERIA_PROFILE STREQUAL "Fuzz")
        set(replay_target ${target}Replay)
        set(replay_name ${fuzz_name}Replay)
    else()
        set(replay_target ${target})
        set(replay_name ${fuzz_name})
    endif()
    add_executable(${replay_target} "${relative}" Support/FuzzDriver.cpp)
    set_target_properties(${replay_target} PROPERTIES
        OUTPUT_NAME ${replay_name}
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/Fuzz")
    target_link_libraries(${replay_target} PRIVATE AsteriaTestSupport AsteriaHostNucleus)
    asteria_apply_host_flags(${replay_target} HOSTED)

    if(NOT ASTERIA_PROFILE STREQUAL "Fuzz")
        set(replay_iterations 20000)
        if(fuzz_name STREQUAL "CryptoKeyFuzz")
            # Fixed-schedule curve parsing is deliberately expensive.
            set(replay_iterations 256)
        endif()
        add_test(NAME ${fuzz_path} COMMAND ${replay_target} --iterations ${replay_iterations})
        set_tests_properties(${fuzz_path} PROPERTIES LABELS fuzz TIMEOUT 300)
        continue()
    endif()

    add_executable(${target} "${relative}")
    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME ${fuzz_name}
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/Fuzz")
    target_link_libraries(${target} PRIVATE AsteriaTestSupport AsteriaHostNucleus)
    asteria_apply_host_flags(${target} HOSTED)
    target_link_options(${target} PRIVATE ${ASTERIA_FUZZER_LINK_FLAGS})
    add_test(NAME ${fuzz_path}
        COMMAND "${CMAKE_COMMAND}"
            -D "REPLAY=$<TARGET_FILE:${replay_target}>"
            -D "FUZZER=$<TARGET_FILE:${target}>"
            -D "SEEDS=${CMAKE_CURRENT_BINARY_DIR}/Fuzz/Seeds/${fuzz_name}"
            -D "WORK=${CMAKE_CURRENT_BINARY_DIR}/Fuzz/Smoke/${fuzz_name}"
            -D RUNS=200000
            -D SECONDS=30
            -P "${CMAKE_CURRENT_SOURCE_DIR}/Fuzz/RunLibFuzzerSmoke.cmake")
    set_tests_properties(${fuzz_path} PROPERTIES LABELS fuzz TIMEOUT 300)
endforeach()

# Clang source-based coverage: every test process writes its raw profile to
# one directory, which a setup fixture empties first, so a report produced by
# Tools/Coverage/Report.py reflects exactly the tests of the last CTest run.
if((ASTERIA_PROFILE STREQUAL "Coverage") AND (ASTERIA_HOST_COMPILER STREQUAL "Clang"))
    set(asteria_profile_directory "${CMAKE_BINARY_DIR}/Coverage/Profiles")
    get_property(asteria_registered_tests DIRECTORY PROPERTY TESTS)
    foreach(test IN LISTS asteria_registered_tests)
        string(MAKE_C_IDENTIFIER "${test}" profile_name)
        set_property(TEST ${test} APPEND PROPERTY ENVIRONMENT
            "LLVM_PROFILE_FILE=${asteria_profile_directory}/${profile_name}-%p.profraw")
        set_property(TEST ${test} APPEND PROPERTY FIXTURES_REQUIRED AsteriaCoverageProfiles)
    endforeach()
    add_test(NAME CoverageProfileReset
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${asteria_profile_directory}")
    set_tests_properties(CoverageProfileReset PROPERTIES
        LABELS static FIXTURES_SETUP AsteriaCoverageProfiles)
endif()

# The public libc surface is also consumed directly by C applications.
add_test(NAME User/Libc/HeaderCompile
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/User/Libc/HeaderCompile.py"
        "${ASTERIA_ROOT}/Source/User/Libc/Include" "${CMAKE_CXX_COMPILER}")
set_tests_properties(User/Libc/HeaderCompile PROPERTIES LABELS static
    SKIP_REGULAR_EXPRESSION "SKIP: " TIMEOUT 60)

# Real static-PIE linker output on both ISAs.
set(asteria_pie_samples "${CMAKE_CURRENT_BINARY_DIR}/User/Loader/StaticPieSamples")
add_custom_command(OUTPUT "${asteria_pie_samples}/amd64.elf" "${asteria_pie_samples}/ia64.elf"
    COMMAND "${Python3_EXECUTABLE}" "${ASTERIA_TOOLS}/Build/BuildStaticPieSamples.py"
        --output "${asteria_pie_samples}"
    DEPENDS "${ASTERIA_TOOLS}/Build/BuildStaticPieSamples.py"
        User/Loader/Samples/StaticPie.cpp User/Loader/Samples/Ia64Entry.lds VERBATIM)
add_custom_target(AsteriaStaticPieSamples ALL
    DEPENDS "${asteria_pie_samples}/amd64.elf" "${asteria_pie_samples}/ia64.elf")
add_dependencies(User_Loader_StaticPieTests AsteriaStaticPieSamples)
set_tests_properties(User/Loader/StaticPieTests PROPERTIES
    ENVIRONMENT "ASTERIA_PIE_SAMPLE_DIR=${asteria_pie_samples}")

set_tests_properties(Acpi/Aml/QemuTablesTests PROPERTIES SKIP_REGULAR_EXPRESSION "SKIP: ")

# Renderer corpora produce the actual host bytes first; the runner compares every byte, not just
# checked-in digests.
set(asteria_render_output "${CMAKE_BINARY_DIR}/Render")
file(MAKE_DIRECTORY "${asteria_render_output}")
foreach(test IN ITEMS User/Session/SceneCorpusTests User/Terminal/TerminalCorpusTests)
    set_tests_properties(${test} PROPERTIES
        ENVIRONMENT "ASTERIA_RENDER_OUTPUT_DIR=${asteria_render_output}"
        FIXTURES_SETUP SessionRenderCorpus)
endforeach()
add_test(NAME User/Session/RenderModelCompare
    COMMAND "${Python3_EXECUTABLE}" -m unittest Tests.test_session_render Tests.test_terminal)
set_tests_properties(User/Session/RenderModelCompare PROPERTIES
    WORKING_DIRECTORY "${ASTERIA_ROOT}/Tools/Vm"
    ENVIRONMENT "ASTERIA_RENDER_OUTPUT_DIR=${asteria_render_output};ASTERIA_UI_TEXT_TOOL=$<TARGET_FILE:UiTextMasks>;TMPDIR=${ASTERIA_ROOT}/Build/Tmp"
    FIXTURES_REQUIRED SessionRenderCorpus LABELS unit TIMEOUT 60)
