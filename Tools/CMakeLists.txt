# SPDX-License-Identifier: Apache-2.0
# Host build tools. They run on the build machine and are never linked into
# a target image.

add_executable(ToolkitRender Build/ToolkitRender.cpp)
target_link_libraries(ToolkitRender PRIVATE AsteriaHostNucleus)
asteria_apply_host_flags(ToolkitRender HOSTED)

add_executable(UiTextMasks Build/UiTextMasks.cpp)
target_link_libraries(UiTextMasks PRIVATE AsteriaHostNucleus)
asteria_apply_host_flags(UiTextMasks HOSTED)

add_executable(WriteBootManifest Build/WriteBootManifest.cpp)
target_include_directories(WriteBootManifest PRIVATE "${ASTERIA_ROOT}/Source")
asteria_apply_host_flags(WriteBootManifest HOSTED)

add_executable(WriteIdentityFixture Build/WriteIdentityFixture.cpp)
target_link_libraries(WriteIdentityFixture PRIVATE AsteriaHostNucleus)
asteria_apply_host_flags(WriteIdentityFixture HOSTED)

# Each boot module's SHA-256 for BOOT.CFG trust entries, the hash the boot application computes;
# checked against FIPS 180-4 and hashlib.
add_executable(WriteModuleDigests Build/WriteModuleDigests.cpp)
target_link_libraries(WriteModuleDigests PRIVATE AsteriaHostNucleus)
asteria_apply_host_flags(WriteModuleDigests HOSTED)
add_test(NAME ModuleDigestTool
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/Build/CheckModuleDigests.py"
        $<TARGET_FILE:WriteModuleDigests>)
set_tests_properties(ModuleDigestTool PROPERTIES LABELS unit)

add_executable(NormalizeIa64Pe Firmware/NormalizeIa64Pe.cpp)
asteria_apply_host_flags(NormalizeIa64Pe HOSTED)

# Unit tests of the target verification library; they need no target build.
add_test(NAME VerifyLibraryTests
    COMMAND "${Python3_EXECUTABLE}" -m unittest discover -s Tests -t .
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/Verify")
set_tests_properties(VerifyLibraryTests PROPERTIES LABELS static)

# Unit tests of the mutation tool; they build nothing.
add_test(NAME MutationToolTests
    COMMAND "${Python3_EXECUTABLE}" -m unittest discover -s Tests -t .
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/Mutation")
set_tests_properties(MutationToolTests PROPERTIES LABELS static)

# Unit tests of the VM test runner; they need no VM.
add_test(NAME VmRunnerTests
    COMMAND "${Python3_EXECUTABLE}" -m unittest discover -s Tests -t .
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/Vm")
set_tests_properties(VmRunnerTests PROPERTIES LABELS static)

add_executable(AmlFacts Acpi/AmlFacts.cpp)
target_link_libraries(AmlFacts PRIVATE AsteriaHostNucleus)
asteria_apply_host_flags(AmlFacts HOSTED)

# Native IA-64 variants accompany host-analyze.
if(ASTERIA_PROFILE STREQUAL "Analyze")
    add_test(NAME LockOrder
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/Verify/CheckLockOrder.py"
            --dumps "${CMAKE_BINARY_DIR}/Source"
            --dumps "${ASTERIA_ROOT}/Build/CMake/ia64/Source"
            --json "${CMAKE_BINARY_DIR}/LockOrder.json")
    set_tests_properties(LockOrder PROPERTIES LABELS static)
endif()

# Desktop session personalization: the project's own cursor art is reproducible.
add_test(NAME DefaultCursorGenerator
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/Cursors/Generate.py"
        --source "${PROJECT_SOURCE_DIR}/Data/Cursors/Default/Source.json"
        --check "${PROJECT_SOURCE_DIR}/Data/Cursors/Default/Default.acsr")
set_tests_properties(DefaultCursorGenerator PROPERTIES LABELS unit)
