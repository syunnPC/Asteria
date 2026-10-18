# SPDX-License-Identifier: Apache-2.0
# Target image build for one ISA. Produces the embedded initial user ELF, the kernel ELF, the PE32+
# UEFI application, and the EFI system partition layout consumed by the VM tests, twice from the
# same kernel objects: the production firmware (Firmware/) and the test firmware (FirmwareTest/),
# whose initial user image is the guest test program of Tests/Guest. Variants that the VM tests need
# (relocated image base, diagnostic probes) are separate build trees selected by cache variables.

set(ASTERIA_FIRMWARE_IMAGE_BASE "0x04000000" CACHE STRING
    "Preferred PE image base; must be a 64 KiB-aligned hexadecimal address")
if(NOT ASTERIA_FIRMWARE_IMAGE_BASE MATCHES "^0x[0-9A-Fa-f]+$")
    message(FATAL_ERROR "ASTERIA_FIRMWARE_IMAGE_BASE must be hexadecimal")
endif()
math(EXPR asteria_image_base_value "${ASTERIA_FIRMWARE_IMAGE_BASE}")
math(EXPR asteria_image_base_misalignment "${asteria_image_base_value} % 65536")
if(NOT asteria_image_base_misalignment EQUAL 0)
    message(FATAL_ERROR "ASTERIA_FIRMWARE_IMAGE_BASE must be 64 KiB-aligned")
endif()

set(ASTERIA_TARGET_WARNING_FLAGS
    -Wall -Wextra -Wpedantic -Werror -Wconversion -Wsign-conversion -Wshadow -Wundef
    -Wcast-align=strict -Wcast-qual -Wold-style-cast -Woverloaded-virtual -Wnon-virtual-dtor)

set(ASTERIA_TARGET_RUNTIME_FLAGS
    -ffreestanding -nostdinc -nostdinc++
    -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit)

include_directories("${CMAKE_BINARY_DIR}")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${ASTERIA_TOOLS}/Build/WriteStopBuildIdentity.py"
    "${ASTERIA_ROOT}" "${CMAKE_BINARY_DIR}/Generated/StopBuildIdentity.hpp"
    COMMAND_ERROR_IS_FATAL ANY)
add_custom_target(AsteriaStopBuildIdentity
    COMMAND "${Python3_EXECUTABLE}" "${ASTERIA_TOOLS}/Build/WriteStopBuildIdentity.py"
        "${ASTERIA_ROOT}" "${CMAKE_BINARY_DIR}/Generated/StopBuildIdentity.hpp"
    BYPRODUCTS "${CMAKE_BINARY_DIR}/Generated/StopBuildIdentity.hpp"
    VERBATIM)

# The per-function stack budget enforced for every target object.
set(ASTERIA_TARGET_STACK_BUDGET_BYTES 512)

option(ASTERIA_COMPILE_CHECKS
    "Build the O0/O2 compile-check objects, closures and user images that the verification checks inspect"
    ON)

if(ASTERIA_TARGET STREQUAL "Ia64")
    set(ASTERIA_IA64_STOP_PROBE 0 CACHE STRING "Inject a stop: 0 off, 1 DisplayBrokerDrained, 2 BeforeStartProcessors, 3 Lockup")
    if(NOT ASTERIA_IA64_STOP_PROBE MATCHES "^[0123]$")
        message(FATAL_ERROR "ASTERIA_IA64_STOP_PROBE must be 0, 1, 2 or 3")
    endif()
    set(ASTERIA_IA64_NATIVE_PREEMPTION_PROBE 0 CACHE STRING
        "Build the optional one-shot IA-64 BSP preemption diagnostic (0 or 1)")
    set(ASTERIA_IA64_SYNTHETIC_USER_PROBE 0 CACHE STRING
        "Build the IA-64 synthetic user-probe diagnostic (0 or 1)")
    foreach(option IN ITEMS ASTERIA_IA64_NATIVE_PREEMPTION_PROBE ASTERIA_IA64_SYNTHETIC_USER_PROBE)
        if(NOT ${option} MATCHES "^[01]$")
            message(FATAL_ERROR "${option} must be 0 or 1")
        endif()
    endforeach()
    # --fatal-warnings turns the assembler's instruction-group dependency
    # violation diagnostics (RAW/WAW and serialization hazards) into errors.
    set(ASTERIA_ISA_CODEGEN_FLAGS -mno-sdata -fno-jump-tables -fno-pic -mtune=itanium2 -Wa,--fatal-warnings)
    # The kernel leaves f32-f127 to user threads: Verify Ia64KernelLowFloatingPartition checks the
    # result.
    set(ASTERIA_KERNEL_COMPILE_FLAGS
        -O2 ${ASTERIA_TARGET_RUNTIME_FLAGS} -fno-common -fno-builtin -fno-ident
        -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables
        -fno-optimize-sibling-calls ${ASTERIA_TARGET_WARNING_FLAGS}
        ${ASTERIA_ISA_CODEGEN_FLAGS} -mfixed-range=f32-f127 -fcallgraph-info=su -fstack-usage)
    set(ASTERIA_KERNEL_DEFINITIONS
        ASTERIA_TARGET_IA64=1
        ASTERIA_IA64_NATIVE_PREEMPTION_PROBE=${ASTERIA_IA64_NATIVE_PREEMPTION_PROBE}
        ASTERIA_IA64_SYNTHETIC_USER_PROBE=${ASTERIA_IA64_SYNTHETIC_USER_PROBE}
        ASTERIA_IA64_STOP_PROBE=${ASTERIA_IA64_STOP_PROBE})
    set(ASTERIA_USER_COMPILE_FLAGS
        -O2 ${ASTERIA_TARGET_RUNTIME_FLAGS} -fno-common -fno-builtin -fno-ident
        -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables
        ${ASTERIA_TARGET_WARNING_FLAGS} ${ASTERIA_ISA_CODEGEN_FLAGS} -fstack-usage)
    # -mno-sdata keeps gp-relative small data out of the code; -fpic uses
    # the image's read-only GOT and descriptors, addressed by its entry gp.
    # The fixed ET_EXEC link resolves those tables; the gate relocates data.
    set(ASTERIA_COMPONENT_COMPILE_FLAGS ${ASTERIA_USER_COMPILE_FLAGS} -fpic)
    set(ASTERIA_USER_DEFINITIONS ASTERIA_TARGET_IA64=1)
    set(ASTERIA_BOOT_FILE_NAME BOOTIA64.EFI)
elseif(ASTERIA_TARGET STREQUAL "Amd64")
    option(ASTERIA_AMD64_X2APIC_HANDOFF_PROBE "Force x2APIC in the test firmware entry (requires CPUID x2APIC)" OFF)
    option(ASTERIA_COMPONENT_WATCHDOG_PROBE "Test display liveness recovery" OFF)
    set(ASTERIA_AMD64_STOP_PROBE 0 CACHE STRING "Inject a stop: 0 off, 1 DisplayBrokerDrained, 2 BeforeStartProcessors, 3 Lockup")
    if(NOT ASTERIA_AMD64_STOP_PROBE MATCHES "^[0123]$")
        message(FATAL_ERROR "ASTERIA_AMD64_STOP_PROBE must be 0, 1, 2 or 3")
    endif()
    set(ASTERIA_AMD64_NATIVE_PREEMPTION_PROBE 0 CACHE STRING
        "Build the optional AMD64 BSP preemption diagnostic (0 or 1)")
    if(NOT ASTERIA_AMD64_NATIVE_PREEMPTION_PROBE MATCHES "^[01]$")
        message(FATAL_ERROR "ASTERIA_AMD64_NATIVE_PREEMPTION_PROBE must be 0 or 1")
    endif()
    set(ASTERIA_ISA_CODEGEN_FLAGS
        -mno-red-zone -mgeneral-regs-only -fno-jump-tables -fno-pic -fno-pie)
    set(ASTERIA_KERNEL_COMPILE_FLAGS
        -O2 ${ASTERIA_TARGET_RUNTIME_FLAGS} -fno-common -fno-builtin -fno-ident
        -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables
        -fno-optimize-sibling-calls ${ASTERIA_TARGET_WARNING_FLAGS}
        ${ASTERIA_ISA_CODEGEN_FLAGS} -fno-merge-constants -mcmodel=large
        -mlarge-data-threshold=4194304 -fstack-usage)
    set(ASTERIA_KERNEL_DEFINITIONS
        ASTERIA_TARGET_AMD64=1
        ASTERIA_AMD64_NATIVE_PREEMPTION_PROBE=${ASTERIA_AMD64_NATIVE_PREEMPTION_PROBE}
        ASTERIA_AMD64_STOP_PROBE=${ASTERIA_AMD64_STOP_PROBE})
    # The AMD64 user flags. -fno-builtin (also implied by -ffreestanding)
    # keeps GCC from turning the loops of the image's Runtime/Memory.cpp into
    # calls of the routines they implement; RuntimeMemoryLeaves checks it.
    set(ASTERIA_USER_COMPILE_FLAGS
        -O2 ${ASTERIA_TARGET_RUNTIME_FLAGS} -fno-common -fno-builtin -fno-ident
        -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables
        ${ASTERIA_TARGET_WARNING_FLAGS} ${ASTERIA_ISA_CODEGEN_FLAGS} -fstack-usage)
    # The images the root service builds may use SSE: every user thread owns an FXSAVE area. The
    # initial image, the pager and the spinner keep -mgeneral-regs-only: their hand-written xmm
    # patterns span compiled code, which SSE codegen would clobber legitimately. Components are
    # optimised for size: the pager image embeds every one, and its staging is a checked budget.
    set(ASTERIA_COMPONENT_COMPILE_FLAGS
        -Os ${ASTERIA_TARGET_RUNTIME_FLAGS} -fno-common -fno-builtin -fno-ident
        -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables
        ${ASTERIA_TARGET_WARNING_FLAGS} -mno-red-zone -fno-jump-tables -fpie -fstack-usage)
    set(ASTERIA_USER_DEFINITIONS ASTERIA_TARGET_AMD64=1)
    set(ASTERIA_BOOT_FILE_NAME BOOTX64.EFI)
else()
    message(FATAL_ERROR "Unsupported ASTERIA_TARGET '${ASTERIA_TARGET}'")
endif()

# Flags of preprocessed assembly (.S) units: warnings are errors, including the
# assembler's (on IA-64 its instruction-group dependency diagnostics).
set(ASTERIA_ASM_FLAGS -Werror -Wa,--fatal-warnings)

# Applies the compile options in ARGN to the C++ sources of TARGET and
# ASTERIA_ASM_FLAGS to its.S sources, which also see the generated layout
# headers of the build tree.
# Sets out to the command that checks the given pager images against the
# boot's staging window for ASTERIA_TARGET (Tools/Build/CheckImageStaging.py).
function(asteria_pager_staging_command out)
    set(checker "${ASTERIA_ROOT}/Tools/Build/CheckImageStaging.py")
    if(ASTERIA_TARGET STREQUAL "Ia64")
        set(command "${Python3_EXECUTABLE}" "${checker}" ${ARGN}
            --header "${ASTERIA_ROOT}/Source/Platform/Ia64/Executable/Ia64InitialUserImageOwner.hpp"
            --segment-limit Ia64InitialUserImageMaximumSegmentPages
            --aggregate-limit Ia64InitialUserImageMaximumLoadPages
            --segment-count-limit Ia64InitialUserImageMaximumSegmentCount)
    else()
        # The AMD64 check takes one image per run.
        set(command "")
        foreach(image IN LISTS ARGN)
            if(command)
                list(APPEND command COMMAND)
            endif()
            list(APPEND command "${Python3_EXECUTABLE}" "${checker}" "${image}"
                --header "${ASTERIA_ROOT}/Source/Platform/Amd64/Executable/Amd64InitialUserImage.hpp")
        endforeach()
    endif()
    set(${out} ${command} PARENT_SCOPE)
endfunction()

function(asteria_target_language_options target)
    # Speculative virtual-function pointer comparisons synthesize a GOT relocation outside the closed
# IA-64 KernelElf contract. Keep calls virtual; native descriptors remain ordinary validated
# IPLTLSB.
    if(ASTERIA_TARGET STREQUAL "Ia64")
        target_compile_options(${target} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-fno-devirtualize>")
    endif()
    # Native IA-64 variants accompany host-analyze in the LockOrder derivation. Dumps precede
# optimization and retain the guard's try/finally scope and virtual-table declarations.
    if(ASTERIA_TARGET STREQUAL "Ia64" AND ASTERIA_COMPILE_CHECKS)
        target_compile_options(${target} PRIVATE
            "$<$<COMPILE_LANGUAGE:CXX>:-fdump-tree-gimple-lineno;-fdump-lang-class>")
    endif()
    target_compile_options(${target} PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:${ARGN}>"
        "$<$<COMPILE_LANGUAGE:ASM>:${ASTERIA_ASM_FLAGS}>")
    target_include_directories(${target} PRIVATE "$<$<COMPILE_LANGUAGE:ASM>:${CMAKE_BINARY_DIR}>")
    if(TARGET AsteriaAsmLayout)
        add_dependencies(${target} AsteriaAsmLayout)
    endif()
endfunction()

# Generates the layout headers of the ISA's hand-written assembly with the
# kernel's compiler, flags and definitions (see AsteriaAsmLayout.cmake).
function(asteria_add_asm_layout)
    file(GLOB units CONFIGURE_DEPENDS "${ASTERIA_ROOT}/Source/Platform/${ASTERIA_TARGET}/Layout/*.cpp")
    if(NOT units)
        return()
    endif()
    set(definitions ${ASTERIA_KERNEL_DEFINITIONS})
    list(TRANSFORM definitions PREPEND "-D")
    asteria_add_asm_layout_units(AsteriaAsmLayout ${ASTERIA_TARGET}
        COMPILER "${CMAKE_CXX_COMPILER}"
        UNITS ${units}
        FLAGS ${ASTERIA_KERNEL_COMPILE_FLAGS} ${definitions})
endfunction()

# Compiles SOURCES (relative to Source/) into an object library.
function(asteria_add_target_objects name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "SOURCES;FLAGS;DEFINITIONS")
    list(TRANSFORM arg_SOURCES PREPEND "${ASTERIA_ROOT}/Source/")
    add_library(${name} OBJECT ${arg_SOURCES})
    target_include_directories(${name} PRIVATE "${ASTERIA_ROOT}/Source" "${CMAKE_BINARY_DIR}")
    add_dependencies(${name} AsteriaStopBuildIdentity)
    asteria_target_language_options(${name} ${arg_FLAGS})
    target_compile_definitions(${name} PRIVATE ${arg_DEFINITIONS})
endfunction()

# Links a minimal user ELF image from OBJECTS (object files or generator
# expressions naming them) once the object libraries in TARGETS are built,
# with LINK_FLAGS added to the linker's.
function(asteria_link_user_image elf map)
    cmake_parse_arguments(PARSE_ARGV 2 arg "" "SCRIPT;FIXED_ADDRESS_REASON" "OBJECTS;TARGETS;LINK_FLAGS")
    if(ASTERIA_TARGET STREQUAL "Ia64")
        set(script "${ASTERIA_ROOT}/Source/User/Platform/Ia64/Runtime/Ia64MinimalUserImage.lds")
    else()
        set(script "${ASTERIA_ROOT}/Source/User/Platform/Amd64/Runtime/Amd64MinimalUserImage.lds")
    endif()
    if(arg_SCRIPT)
        set(script "${arg_SCRIPT}")
    endif()
    if(ASTERIA_TARGET STREQUAL "Ia64")
        set(arguments -nostdlib -static --no-undefined --fatal-warnings --build-id=none
            -T "${script}" -Map "${map}")
    else()
        set(arguments --no-undefined --fatal-warnings -z max-page-size=0x1000
            -T "${script}" -Map=${map})
    endif()
    list(APPEND arguments ${arg_LINK_FLAGS})
    # Every native image embedded in the component table is linked again from exactly these objects.
# Only the gate's data segment is writable: descriptors/GOT may need data relocations in a PIE, but
# -z text forbids any relocation in executable text. Production stays fixed-address ET_EXEC with
# sealed read-only data.
    set(gate_commands)
    set(gate_dependencies)
    set(gate_outputs)
    if(NOT arg_FIXED_ADDRESS_REASON)
        set_property(GLOBAL APPEND PROPERTY ASTERIA_SHARED_USER_IMAGE_GATES "${elf}.TextGate")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${script}")
        file(READ "${script}" gate_script_original)
        string(REPLACE "ReadOnlyData PT_LOAD FLAGS(4)" "ReadOnlyData PT_LOAD FLAGS(6)"
            gate_script "${gate_script_original}")
        if(gate_script STREQUAL gate_script_original)
            # Custom session scripts coalesce readonly data into RX for the page grant. The gate
            # separates it so IA-64 descriptors can relocate without permitting text fixups.
            string(REGEX REPLACE "(^|[ \t\n;{])Text PT_LOAD FLAGS\\(5\\);"
                "\\1Text PT_LOAD FLAGS(5); ReadOnlyData PT_LOAD FLAGS(6);"
                gate_script "${gate_script_original}")
            # The landed input-control split also puts constants in InputText. Preserve every code
            # section and separate data, including IA-64 descriptors/linkage tables, so RX loads
            # cannot span them.
            foreach(section IN ITEMS rodata AsteriaUserEntryDescriptor opd got data init_array)
                string(REGEX REPLACE "(\\.${section}[ \t\n]+:[^\{]*\{[^}]*\}[ \t\n]*):[A-Za-z_][A-Za-z0-9_]*" "\\1:ReadOnlyData"
                    gate_script "${gate_script}")
            endforeach()
            if(gate_script STREQUAL gate_script_original OR NOT gate_script MATCHES ":Text"
                OR NOT gate_script MATCHES ":ReadOnlyData")
                message(FATAL_ERROR "Text gate cannot separate executable text in ${script}")
            endif()
        endif()
        set(gate_path "${elf}.TextGate.lds")
        file(CONFIGURE OUTPUT "${gate_path}" CONTENT "${gate_script}" @ONLY)
        set(gate_arguments ${arguments})
        list(REMOVE_ITEM gate_arguments -static "-Map=${map}")
        if(ASTERIA_TARGET STREQUAL "Ia64")
            list(FIND gate_arguments -Map gate_map_index)
            list(REMOVE_AT gate_arguments ${gate_map_index})
            list(REMOVE_ITEM gate_arguments "${map}")
        endif()
        # The gate has its own script, so remove the production -T pair.
        list(FIND gate_arguments -T gate_script_index)
        list(REMOVE_AT gate_arguments ${gate_script_index})
        list(REMOVE_ITEM gate_arguments "${script}")
        set(gate_text_policy text)
        if(ASTERIA_TARGET STREQUAL "Ia64")
            # IA-64 ld diagnoses relocations in read-only *sections*, even
            # when the gate's data PT_LOAD is writable. Its ABI descriptors
            # necessarily relocate; check executable relocation targets below.
            set(gate_text_policy notext)
        endif()
        set(gate_checker "${ASTERIA_ROOT}/Tools/Build/CheckImageTextRelocations.py")
        set(gate_commands COMMAND "${ASTERIA_TARGET_LD}" ${gate_arguments}
            -T "${gate_path}" -Map "${elf}.TextGate.map"
            -pie -z ${gate_text_policy} ${arg_OBJECTS} -o "${elf}.TextGate"
            COMMAND "${Python3_EXECUTABLE}" "${gate_checker}" "${elf}.TextGate")
        set(gate_dependencies "${gate_path}" "${gate_checker}")
        set(gate_outputs "${elf}.TextGate" "${elf}.TextGate.map")
    endif()
    add_custom_command(OUTPUT "${elf}"
        BYPRODUCTS ${gate_outputs}
        ${gate_commands}
        COMMAND "${ASTERIA_TARGET_LD}" ${arguments} ${arg_OBJECTS} -o "${elf}"
        DEPENDS ${arg_TARGETS} ${arg_OBJECTS} "${script}" ${gate_dependencies}
        COMMAND_EXPAND_LISTS VERBATIM)
endfunction()

# Links one firmware from the kernel objects and one initial user image into
# ${CMAKE_BINARY_DIR}/DIRECTORY: the user ELF <USER_NAME>.elf linked from
# USER_OBJECTS (built by USER_TARGETS), the kernel ELF,
# the UEFI application <NAME>.efi and the EFI system partition Esp/, whose
# boot file the custom target TARGET builds. Sets <PREFIX>_USER_ELF,
# <PREFIX>_KERNEL, <PREFIX>_EFI and <PREFIX>_ESP in the
# caller's scope. The IA-64 PE normalizer must already have a rule. An AMD64
# firmware installs stripped user/pager files and SPINNER_ELF when given.
function(asteria_link_firmware directory)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "NAME;USER_NAME;TARGET;PREFIX;PAGER_ELF;SPINNER_ELF"
        "USER_OBJECTS;USER_TARGETS;MODULES")
    set(output "${CMAKE_BINARY_DIR}/${directory}")
    file(MAKE_DIRECTORY "${output}/Esp/EFI/BOOT")
    set(user_elf "${output}/${arg_USER_NAME}.elf")
    set(kernel_elf "${output}/${arg_NAME}.elf")
    set(efi "${output}/${arg_NAME}.efi")
    set(boot_file "${output}/Esp/EFI/BOOT/${ASTERIA_BOOT_FILE_NAME}")

    asteria_link_user_image("${user_elf}" "${output}/${arg_USER_NAME}.map"
            FIXED_ADDRESS_REASON "Bootstrap image staged once; shared executable segments applies to components"
        OBJECTS ${arg_USER_OBJECTS} TARGETS ${arg_USER_TARGETS})
    set(kernel_template "${ASTERIA_ROOT}/Source/Platform/${ASTERIA_TARGET}/Boot/KernelImage.lds")
    set(kernel_layout "${CMAKE_BINARY_DIR}/Generated/Platform/${ASTERIA_TARGET}/KernelNoteAsmLayout.h")
    set(elf_script "${output}/KernelImage.lds")
    add_custom_command(OUTPUT "${elf_script}"
        COMMAND "${Python3_EXECUTABLE}" "${ASTERIA_ROOT}/Tools/Build/WriteKernelLinkerScript.py"
            "${kernel_template}" "${kernel_layout}" "${elf_script}"
        DEPENDS AsteriaAsmLayout "${kernel_template}" "${kernel_layout}"
            "${ASTERIA_ROOT}/Tools/Build/WriteKernelLinkerScript.py" VERBATIM)
    set(kernel_entry_object)
    set(kernel_entry_options)
    if(ASTERIA_TARGET STREQUAL "Amd64" AND ASTERIA_AMD64_X2APIC_HANDOFF_PROBE AND arg_PREFIX STREQUAL "test")
        set(kernel_entry_source "${ASTERIA_ROOT}/Tests/Target/Amd64X2ApicFirmwareEntry.S")
        set(kernel_entry_object "${output}/Amd64X2ApicKernelEntry.o")
        set(kernel_entry_options --wrap=AsteriaAmd64ApplicationProcessorEntry)
        add_custom_command(OUTPUT "${kernel_entry_object}"
            COMMAND "${CMAKE_CXX_COMPILER}" -DASTERIA_KERNEL_AP_ENTRY=1 -c "${kernel_entry_source}"
                -o "${kernel_entry_object}"
            DEPENDS "${kernel_entry_source}" VERBATIM)
    endif()
    set(kernel_elf "${output}/Kernel.elf")
    add_custom_command(OUTPUT "${kernel_elf}"
        COMMAND "${ASTERIA_TARGET_LD}" -nostdlib -static --no-undefined --fatal-warnings --build-id=none
            --emit-relocs -z max-page-size=4096 -T "${elf_script}"
            -Map "${output}/Kernel.map" ${kernel_entry_options} ${ASTERIA_KERNEL_LINK_OBJECTS} ${kernel_entry_object} -o "${kernel_elf}"
        COMMAND "${Python3_EXECUTABLE}" "${ASTERIA_ROOT}/Tools/Build/CanonicalizeKernelElf.py" "${kernel_elf}"
        DEPENDS AsteriaKernelObjects ${ASTERIA_KERNEL_LINK_OBJECTS} ${kernel_entry_object} "${elf_script}"
            "${ASTERIA_ROOT}/Tools/Build/CanonicalizeKernelElf.py" COMMAND_EXPAND_LISTS VERBATIM)
    set(loader_elf "${output}/${arg_NAME}.elf")
    if(ASTERIA_TARGET STREQUAL "Ia64")
        # The linker keeps GP beside the read-only GOT and immutable data.
        set(kernel_script "${ASTERIA_ROOT}/Source/Platform/Ia64/Uefi/Application.lds")
        add_custom_command(OUTPUT "${loader_elf}"
            COMMAND "${ASTERIA_TARGET_LD}" -nostdlib -static --no-undefined --fatal-warnings
                --build-id=none --emit-relocs
                --defsym=AsteriaConfiguredImageBase=${ASTERIA_FIRMWARE_IMAGE_BASE}
                -T "${kernel_script}" -Map "${output}/${arg_NAME}.map"
                ${ASTERIA_LOADER_LINK_OBJECTS} -o "${loader_elf}"
            DEPENDS AsteriaKernelObjects ${ASTERIA_LOADER_LINK_OBJECTS}
                "${kernel_script}"
            COMMAND_EXPAND_LISTS VERBATIM)

        # PE conversion, then the normalizer that rewrites the IA-64 base
        # relocation and section contract the firmware requires.
        set(normalizer "${CMAKE_BINARY_DIR}/HostTools/NormalizeIa64Pe")
        add_custom_command(OUTPUT "${efi}"
            COMMAND "${ASTERIA_TARGET_OBJCOPY}" --strip-all -R .comment -O pei-ia64
                --image-base=${ASTERIA_FIRMWARE_IMAGE_BASE} --section-alignment=0x1000
                --file-alignment=0x200 --subsystem=10 --readonly-text --pure
                --set-section-flags .text=alloc,load,readonly,code
                --set-section-flags .rodata=alloc,load,readonly,data
                --set-section-flags .opd=alloc,load,readonly,data
                --set-section-flags .data=alloc,load,data
                --set-section-flags .reloc=alloc,load,readonly,data
                --set-section-flags .bss=alloc
                "${loader_elf}" "${efi}"
            COMMAND "${normalizer}" --normalize-ia64 "${loader_elf}" "${efi}"
            DEPENDS "${loader_elf}" "${normalizer}" VERBATIM)
    else()
        # Direct PE links from ELF omit base fixups for references to defined
        # ELF weak symbols. Resolve COMDAT first, then localize the surviving
        # weak definitions in the closed core.
        set(core_script "${ASTERIA_ROOT}/Source/Platform/Amd64/Uefi/FirmwareCore.lds")
        set(unlocalized "${output}/Amd64KernelUnlocalized.o")
        set(weak_list "${output}/Amd64ResolvedWeakSymbols.txt")
        set(core "${output}/Amd64Kernel.o")
        set(firmware_entry EfiMain)
        set(entry_object)
        set(entry_link_options)
        if(ASTERIA_AMD64_X2APIC_HANDOFF_PROBE AND arg_PREFIX STREQUAL "test")
            set(firmware_entry AsteriaAmd64X2ApicFirmwareEntry)
            set(entry_source "${ASTERIA_ROOT}/Tests/Target/Amd64X2ApicFirmwareEntry.S")
            set(entry_object "${output}/Amd64X2ApicFirmwareEntry.o")
            add_custom_command(OUTPUT "${entry_object}"
                COMMAND "${CMAKE_CXX_COMPILER}" -c "${entry_source}" -o "${entry_object}"
                DEPENDS "${entry_source}" VERBATIM)
        endif()
        add_custom_command(OUTPUT "${core}"
            COMMAND "${ASTERIA_TARGET_LD}" -r --force-group-allocation --orphan-handling=error
                -T "${core_script}" ${entry_link_options} ${ASTERIA_LOADER_LINK_OBJECTS} ${entry_object}
                -o "${unlocalized}"
            COMMAND "${Python3_EXECUTABLE}" "${ASTERIA_ROOT}/Tools/Build/ListWeakSymbols.py"
                --nm "${ASTERIA_TARGET_NM}" --require-closed "${unlocalized}" --output "${weak_list}"
            COMMAND "${ASTERIA_TARGET_OBJCOPY}" --localize-symbols=${weak_list}
                "${unlocalized}" "${core}"
            DEPENDS AsteriaKernelObjects ${ASTERIA_LOADER_LINK_OBJECTS}
                ${entry_object} "${core_script}"
                "${ASTERIA_ROOT}/Tools/Build/ListWeakSymbols.py"
            COMMAND_EXPAND_LISTS VERBATIM)

        set(pe_script "${ASTERIA_ROOT}/Source/Platform/Amd64/Uefi/Application.pe.lds")
        add_custom_command(OUTPUT "${efi}"
            COMMAND "${ASTERIA_TARGET_LD}" -mi386pep -T "${pe_script}"
                -Map "${output}/${arg_NAME}.pe.map" --subsystem 10
                --image-base ${ASTERIA_FIRMWARE_IMAGE_BASE} --section-alignment 0x1000
                --file-alignment 0x200 --dynamicbase --high-entropy-va --nxcompat
                --no-insert-timestamp
                --stack 0,0 --heap 0,0 --no-undefined --fatal-warnings --strip-all
                -e ${firmware_entry} "${core}" -o "${efi}"
            DEPENDS "${core}" "${pe_script}" VERBATIM)
        set(loader_elf "${core}")
    endif()

    add_custom_command(OUTPUT "${boot_file}"
        COMMAND "${CMAKE_COMMAND}" -E copy "${efi}" "${boot_file}"
        DEPENDS "${efi}" VERBATIM)
    set(boot_directory "${output}/Esp/EFI/ASTERIA")
    file(MAKE_DIRECTORY "${boot_directory}")
    set(installed_kernel "${boot_directory}/KERNEL.ELF")
    add_custom_command(OUTPUT "${installed_kernel}"
        COMMAND "${ASTERIA_TARGET_OBJCOPY}" --strip-unneeded -R .comment "${kernel_elf}" "${installed_kernel}"
        COMMAND "${CMAKE_BINARY_DIR}/HostTools/CheckKernelElf" "${installed_kernel}" "${ASTERIA_TARGET}"
        DEPENDS "${kernel_elf}" "${CMAKE_BINARY_DIR}/HostTools/CheckKernelElf" VERBATIM)
    set(boot_images "${installed_kernel}")
    set(linked_images "${arg_PAGER_ELF}" "${user_elf}")
    set(image_names INIT USER)
    if(arg_SPINNER_ELF)
        list(APPEND linked_images "${arg_SPINNER_ELF}")
        list(APPEND image_names SPINNER)
    endif()
    foreach(linked name IN ZIP_LISTS linked_images image_names)
        set(installed "${boot_directory}/${name}.ELF")
        add_custom_command(OUTPUT "${installed}"
            COMMAND "${ASTERIA_TARGET_OBJCOPY}" --strip-all "${linked}" "${installed}"
            DEPENDS "${linked}" VERBATIM)
        list(APPEND boot_images "${installed}")
    endforeach()
    file(MAKE_DIRECTORY "${boot_directory}/MODULES")
    # Boot file contract already carry opaque checksummed module bytes. The root distinguishes these
    # data receipts by name; no executable grant.
    list(APPEND arg_MODULES ui-bold "${ASTERIA_ROOT}/Data/Fonts/Vegur/Vegur-Bold.otf"
        ui-regular "${ASTERIA_ROOT}/Data/Fonts/Vegur/Vegur-Regular.otf")
    set(module_arguments --modules)
    set(module_files)
    list(LENGTH arg_MODULES module_length)
    math(EXPR module_last "${module_length} - 1")
    foreach(index RANGE 0 ${module_last} 2)
        math(EXPR path_index "${index} + 1")
        list(GET arg_MODULES ${index} name)
        list(GET arg_MODULES ${path_index} path)
        string(TOUPPER "${name}" basename)
        if(name STREQUAL "component")
            set(basename COMP)
        elseif(name STREQUAL "compositor")
            set(basename COMPOSIT)
        elseif(name STREQUAL "input-seat")
            set(basename SEAT)
        elseif(name STREQUAL "toolkit")
            set(basename TOOLKIT)
        elseif(name STREQUAL "pattern")
            set(basename PATTERN)
        elseif(name STREQUAL "face-probe")
            set(basename FACEPROB)
        elseif(name STREQUAL "directory")
            set(basename DIR)
        elseif(name MATCHES "-stub$")
            set(basename INTERP)
        elseif(name MATCHES "-sample$")
            string(REGEX REPLACE "-SAMPLE$" "" basename "${basename}")
        endif()
        set(extension ELF)
        if(name STREQUAL "seat-test")
            set(basename SEATTEST)
            set(extension DAT)
        endif()
        if(name STREQUAL "ui-bold")
            set(basename VEGURB)
            set(extension OTF)
        elseif(name STREQUAL "ui-regular")
            set(basename VEGURR)
            set(extension OTF)
        elseif(name STREQUAL "accounts")
            set(extension DB)
        endif()
        set(installed "${boot_directory}/MODULES/${basename}.${extension}")
        if(path MATCHES "[.]elf$")
            set(install_command "${ASTERIA_TARGET_OBJCOPY}" --strip-all)
        else()
            set(install_command "${CMAKE_COMMAND}" -E copy)
        endif()
        add_custom_command(OUTPUT "${installed}"
            COMMAND ${install_command} "${path}" "${installed}"
            DEPENDS "${path}" VERBATIM)
        list(APPEND module_files "${installed}")
        list(APPEND module_arguments "${name}" "${basename}.${extension}" "${installed}")
    endforeach()
    set(session_arguments)
    if(arg_PREFIX STREQUAL "production")
        set(session_arguments --desktop)
    elseif(arg_PREFIX STREQUAL "login")
        set(session_arguments --login)
    endif()
    set(manifest "${boot_directory}/BOOT.CFG")
    add_custom_command(OUTPUT "${manifest}"
        COMMAND "${CMAKE_BINARY_DIR}/HostTools/WriteBootManifest" "${manifest}" ${boot_images} ${module_arguments} ${session_arguments}
        DEPENDS "${CMAKE_BINARY_DIR}/HostTools/WriteBootManifest" ${boot_images} ${module_files}
        VERBATIM)
    # Each installed module's SHA-256, beside (not in) the ESP: a BOOT.CFG trust entry names a module
# and this digest; the VM runner writes its trusted cases' entries from it.
    set(digests "${output}/ModuleDigests.txt")
    set(digest_arguments ${module_arguments})
    list(REMOVE_AT digest_arguments 0)
    add_custom_command(OUTPUT "${digests}"
        COMMAND "${CMAKE_BINARY_DIR}/HostTools/WriteModuleDigests" "${digests}" ${digest_arguments}
        DEPENDS "${CMAKE_BINARY_DIR}/HostTools/WriteModuleDigests" ${module_files}
        VERBATIM)
    set(font_notice "${boot_directory}/MODULES/FONTNOTE.TXT")
    add_custom_command(OUTPUT "${font_notice}"
        COMMAND "${CMAKE_COMMAND}" -E copy "${ASTERIA_ROOT}/Data/Fonts/Vegur/NOTICE.md" "${font_notice}"
        DEPENDS "${ASTERIA_ROOT}/Data/Fonts/Vegur/NOTICE.md" VERBATIM)
    add_custom_target(${arg_TARGET} ALL DEPENDS "${boot_file}" "${manifest}" "${digests}" "${font_notice}")

    set(${arg_PREFIX}_USER_ELF "${user_elf}" PARENT_SCOPE)
    set(${arg_PREFIX}_KERNEL "${kernel_elf}" PARENT_SCOPE)
    set(${arg_PREFIX}_LOADER "${loader_elf}" PARENT_SCOPE)
    set(${arg_PREFIX}_EFI "${efi}" PARENT_SCOPE)
    set(${arg_PREFIX}_ESP "${output}/Esp" PARENT_SCOPE)
endfunction()

# Builds the ISA's two firmwares from one set of kernel objects. The
# production firmware (${CMAKE_BINARY_DIR}/Firmware) embeds the user image
# linked from USER_SOURCES. The test firmware (FirmwareTest) embeds the guest
# test program instead: the same user objects without USER_MAIN, the unit
# that defines the image's C++ entry, plus every.cpp of Tests/Guest and every
#.cpp and.S of Tests/Guest/<Isa>, compiled like the user image. Outputs are
# exported as ASTERIA_FIRMWARE_* and ASTERIA_FIRMWARE_TEST_* variables. Only
# the test ESP contains the spinner image linked from SPINNER_SOURCES.

function(asteria_add_firmware)
    set(ui_tool "${CMAKE_BINARY_DIR}/HostTools/UiTextMasks")
    set(ui_tool_sources "${ASTERIA_ROOT}/Tools/Build/UiTextMasks.cpp")
    foreach(source IN ITEMS Font Cff Type2 Outline Layout LayoutTables Raster Cache)
        list(APPEND ui_tool_sources "${ASTERIA_ROOT}/Source/User/Font/${source}.cpp")
    endforeach()
    list(APPEND ui_tool_sources "${ASTERIA_ROOT}/Source/User/Session/UiText.cpp"
        "${ASTERIA_ROOT}/Source/User/Session/Decorations.cpp")
    file(GLOB ui_tool_headers CONFIGURE_DEPENDS "${ASTERIA_ROOT}/Source/User/Font/*.hpp"
        "${ASTERIA_ROOT}/Source/User/Session/*.hpp")
    add_custom_command(OUTPUT "${ui_tool}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/HostTools"
        COMMAND "${ASTERIA_HOST_CXX}" -std=c++23 -O2 -fno-exceptions -fno-rtti -Wall -Wextra -Werror
            -I "${ASTERIA_ROOT}/Source" ${ui_tool_sources} -o "${ui_tool}"
        DEPENDS ${ui_tool_sources} ${ui_tool_headers} VERBATIM)
    add_custom_target(AsteriaUiTextMasks ALL DEPENDS "${ui_tool}")
    set(kernel_check_tool "${CMAKE_BINARY_DIR}/HostTools/CheckKernelElf")
    add_custom_command(OUTPUT "${kernel_check_tool}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/HostTools"
        COMMAND "${ASTERIA_HOST_CXX}" -std=c++23 -O2 -Wall -Wextra -Werror
            -I "${ASTERIA_ROOT}/Source" "${ASTERIA_ROOT}/Tools/Build/CheckKernelElf.cpp"
            "${ASTERIA_ROOT}/Source/Boot/KernelElf.cpp" -o "${kernel_check_tool}"
        DEPENDS "${ASTERIA_ROOT}/Tools/Build/CheckKernelElf.cpp"
            "${ASTERIA_ROOT}/Source/Boot/KernelElf.cpp" "${ASTERIA_ROOT}/Source/Boot/KernelElf.hpp"
            "${ASTERIA_ROOT}/Source/Boot/BootInfo.hpp" "${ASTERIA_ROOT}/Source/Boot/KernelEntryAbi.hpp"
        VERBATIM)
    set(digest_tool "${CMAKE_BINARY_DIR}/HostTools/WriteModuleDigests")
    add_custom_command(OUTPUT "${digest_tool}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/HostTools"
        COMMAND "${ASTERIA_HOST_CXX}" -std=c++23 -O2 -fno-exceptions -fno-rtti -fno-common
            ${ASTERIA_TARGET_WARNING_FLAGS}
            -I "${ASTERIA_ROOT}/Source" "${ASTERIA_ROOT}/Tools/Build/WriteModuleDigests.cpp"
            "${ASTERIA_ROOT}/Source/Kernel/Security/Sha256.cpp" -o "${digest_tool}"
        DEPENDS "${ASTERIA_ROOT}/Tools/Build/WriteModuleDigests.cpp"
            "${ASTERIA_ROOT}/Source/Kernel/Security/Sha256.cpp" "${ASTERIA_ROOT}/Source/Kernel/Security/Sha256.hpp"
        VERBATIM)
    set(manifest_tool "${CMAKE_BINARY_DIR}/HostTools/WriteBootManifest")
    add_custom_command(OUTPUT "${manifest_tool}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/HostTools"
        COMMAND "${ASTERIA_HOST_CXX}" -std=c++23 -O2 -fno-exceptions -fno-rtti -fno-common ${ASTERIA_TARGET_WARNING_FLAGS}
            -I "${ASTERIA_ROOT}/Source" "${ASTERIA_ROOT}/Tools/Build/WriteBootManifest.cpp" -o "${manifest_tool}"
        DEPENDS "${ASTERIA_ROOT}/Tools/Build/WriteBootManifest.cpp"
            "${ASTERIA_ROOT}/Source/User/Filesystem/Crc32c.hpp" VERBATIM)

    cmake_parse_arguments(PARSE_ARGV 0 arg "" "USER_MAIN"
        "KERNEL_SOURCES;USER_SOURCES;PAGER_SOURCES;SPINNER_SOURCES;COMPONENT_SOURCES;COMPONENT_SIZE_SOURCES;DIRECTORY_SOURCES;CLIENT_SOURCES;INTERPRETER_SOURCES;USB_SOURCES;DISPLAY_SOURCES;COMPOSITOR_SOURCES;AUTH_SOURCES;LOGIN_SOURCES")
    if(NOT arg_USER_MAIN IN_LIST arg_USER_SOURCES)
        message(FATAL_ERROR "USER_MAIN ${arg_USER_MAIN} is not one of the USER_SOURCES")
    endif()

    asteria_add_target_objects(AsteriaUserObjects
        SOURCES ${arg_USER_SOURCES}
        FLAGS ${ASTERIA_USER_COMPILE_FLAGS}
        DEFINITIONS ${ASTERIA_USER_DEFINITIONS})
    asteria_add_target_objects(AsteriaKernelObjects
        SOURCES ${arg_KERNEL_SOURCES}
        FLAGS ${ASTERIA_KERNEL_COMPILE_FLAGS}
        DEFINITIONS ${ASTERIA_KERNEL_DEFINITIONS})
    set(kernel_objects)
    set(loader_objects)
    foreach(source IN LISTS arg_KERNEL_SOURCES)
        set(is_loader FALSE)
        if(source MATCHES "^Boot/Uefi/|^Boot/Edid[.]cpp$|^Boot/KernelElf[.]cpp$|^Boot/BootManifest[.]cpp$|^Boot/Entropy/|^Acpi/Aml/|^Inventory/(ReportText|XhciReport|EhciRmrrReport)[.]cpp$|^Boot/Acpi/(AcpiDefinitionBlocks|AcpiTableParser|DebugPortTables)[.]cpp$"
                OR source MATCHES "^Platform/Firmware/AhciPciResourceCapture[.]cpp$|^Platform/[^/]+/Firmware/(ProcessorInventory|BootEntropySources|SalSystemTableCapture|SalSystemTableLocator|Ia64ImageRelocation|FirmwarePortIo)[.]"
                OR source MATCHES "^Platform/Ia64/Uefi/")
            set(is_loader TRUE)
        endif()
        set(shared FALSE)
        if(source MATCHES "^Runtime/|^Platform/FatalTrap[.]cpp$|^Boot/BootInfoValidation[.]cpp$|^Kernel/Security/Sha256[.]cpp$|^Boot/Acpi/Ivrs[.]cpp$"
                OR source MATCHES "^Platform/Amd64/(Firmware/(DiagnosticSinkSelection|Cpuid|Amd64LoadedImageLayout)|Memory/InitialPagingCapabilityCapture|Device/(AmdViTopology|VtdFirmwareRanges))[.]"
                OR (ASTERIA_TARGET STREQUAL "Amd64" AND source MATCHES "^Platform/Device/VtdTopology[.]")
                OR source MATCHES "^Platform/Ia64/(Runtime/CompilerArithmetic|Time/Ia64LocalTimer|Firmware/(Ia64LoadedImageLayout|SalSystemTableParser|Ia64DebugUart))[.]"
                OR source MATCHES "^Platform/Firmware/Diagnostic[.]|^Platform/[^/]+/Firmware/Diagnostic[.]")
            set(shared TRUE)
        endif()
        string(REPLACE "." "[.]" escaped "${source}")
        set(object "$<FILTER:$<TARGET_OBJECTS:AsteriaKernelObjects>,INCLUDE,/${escaped}[.]>")
        if(is_loader OR shared)
            list(APPEND loader_objects "${object}")
        endif()
        if(NOT is_loader)
            list(APPEND kernel_objects "${object}")
        endif()
    endforeach()
    set(ASTERIA_LOADER_LINK_OBJECTS "${loader_objects}")
    set(ASTERIA_KERNEL_LINK_OBJECTS "${kernel_objects}")
    # The components the boot pager builds in the domains it creates, embedded in the pager image in
    # its own read-only section,.AsteriaComponentImage, which each pager script places, between
    # AsteriaComponentImageStart and AsteriaComponentImageEnd: a table (a count and a zero word,
    # then per image a 16-byte name, its offset from the table and its size; see
    # User/Pager/ComponentImage.hpp), then the images. Their segments sit at 4 KiB file alignment
    # (the loader needs no more), so the embedded files stay small.
    set(component_object "")
    if(arg_COMPONENT_SOURCES)
        asteria_add_target_objects(AsteriaComponentObjects
            SOURCES ${arg_COMPONENT_SOURCES}
            # collect unused helpers on the combined tree to retain the
            # segment bound, as the seat module does.
            FLAGS ${ASTERIA_COMPONENT_COMPILE_FLAGS} -ffunction-sections -fdata-sections
            DEFINITIONS ${ASTERIA_USER_DEFINITIONS})
        # Sources of the same image compiled for size (the platform's
        # COMPONENT_SIZE_SOURCES; its size level overrides the flags' level).
        set(component_objects $<TARGET_OBJECTS:AsteriaComponentObjects>)
        set(component_targets AsteriaComponentObjects)
        if(arg_COMPONENT_SIZE_SOURCES)
            set(component_size_level -Os)
            if(ASTERIA_TARGET STREQUAL "Amd64")
                # Read-only PIC jump tables pass the image text-relocation gate.
                # Keep shared helpers outside callers and omit frame pointers to fit
                # the component segment bound. Stack checks cover every object.
                set(component_size_level -Oz -fjump-tables -fno-inline-functions-called-once -fno-partial-inlining -fomit-frame-pointer)
            endif()
            asteria_add_target_objects(AsteriaComponentSizeObjects
                SOURCES ${arg_COMPONENT_SIZE_SOURCES}
                FLAGS ${ASTERIA_COMPONENT_COMPILE_FLAGS} ${component_size_level} -ffunction-sections -fdata-sections
                DEFINITIONS ${ASTERIA_USER_DEFINITIONS})
            list(APPEND component_objects $<TARGET_OBJECTS:AsteriaComponentSizeObjects>)
            list(APPEND component_targets AsteriaComponentSizeObjects)
        endif()
        file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/Component")
        set(component_elf "${CMAKE_BINARY_DIR}/Component/ComponentImage.elf")
        set(component_script "${ASTERIA_ROOT}/Source/User/Platform/${ASTERIA_TARGET}/Component/${ASTERIA_TARGET}ComponentImage.lds")
        if(ASTERIA_TARGET STREQUAL "Amd64")
            # Keep each segment within 32 pages by sealing input control as a second RX segment.
            file(READ "${component_script}" component_script_text)
            string(REPLACE "    Text PT_LOAD FLAGS(5);"
                "    Text PT_LOAD FLAGS(5);\n    InputText PT_LOAD FLAGS(5);" component_script_text "${component_script_text}")
            string(REPLACE "    .text : { *(.text*) } :Text"
                "    .text : { *(EXCLUDE_FILE (*InputBroker.cpp.obj *InputBrokerLogic.cpp.obj *Ps2Driver.cpp.obj) .text*) } :Text\n    . = ALIGN(0x1000);\n    .input_text : { *InputBroker.cpp.obj(.text*) *InputBrokerLogic.cpp.obj(.text*) *Ps2Driver.cpp.obj(.text*) } :InputText"
                component_script_text "${component_script_text}")
            # Immutable constants share the sealed control segment, as in
            # the UI image; preserve the loader's three-segment format.
            string(REPLACE "    ReadOnlyData PT_LOAD FLAGS(4);" "" component_script_text "${component_script_text}")
            string(REPLACE ":ReadOnlyData" ":InputText" component_script_text "${component_script_text}")
            set(component_script "${CMAKE_BINARY_DIR}/Component/Component.lds")
            file(WRITE "${component_script}" "${component_script_text}")
        endif()
        set(component_link_flags "")
        if(ASTERIA_TARGET STREQUAL "Ia64")
            set(component_link_flags -z max-page-size=0x1000)
        endif()
        asteria_link_user_image("${component_elf}" "${CMAKE_BINARY_DIR}/Component/ComponentImage.map"
            SCRIPT "${component_script}"
            OBJECTS ${component_objects} TARGETS ${component_targets}
            LINK_FLAGS ${component_link_flags} --gc-sections)
        set(component_images component "${component_elf}")
        # The Directory a root service starts, linked with the component script, whose entry it
        # defines.
        set(directory_elf "")
        if(arg_DIRECTORY_SOURCES)
            asteria_add_target_objects(AsteriaDirectoryObjects
                SOURCES ${arg_DIRECTORY_SOURCES}
                FLAGS ${ASTERIA_COMPONENT_COMPILE_FLAGS}
                DEFINITIONS ${ASTERIA_USER_DEFINITIONS})
            set(directory_elf "${CMAKE_BINARY_DIR}/Component/DirectoryImage.elf")
            asteria_link_user_image("${directory_elf}" "${CMAKE_BINARY_DIR}/Component/DirectoryImage.map"
                SCRIPT "${ASTERIA_ROOT}/Source/User/Platform/${ASTERIA_TARGET}/Component/${ASTERIA_TARGET}ComponentImage.lds"
                OBJECTS $<TARGET_OBJECTS:AsteriaDirectoryObjects> TARGETS AsteriaDirectoryObjects
                LINK_FLAGS ${component_link_flags})
            list(APPEND component_images directory "${directory_elf}")
        endif()
        # The client of those services.
        set(client_elf "")
        if(arg_CLIENT_SOURCES)
            asteria_add_target_objects(AsteriaClientObjects
                SOURCES ${arg_CLIENT_SOURCES}
                FLAGS ${ASTERIA_COMPONENT_COMPILE_FLAGS}
                DEFINITIONS ${ASTERIA_USER_DEFINITIONS})
            set(client_elf "${CMAKE_BINARY_DIR}/Component/ClientImage.elf")
            asteria_link_user_image("${client_elf}" "${CMAKE_BINARY_DIR}/Component/ClientImage.map"
                SCRIPT "${ASTERIA_ROOT}/Source/User/Platform/${ASTERIA_TARGET}/Component/${ASTERIA_TARGET}ComponentImage.lds"
                OBJECTS $<TARGET_OBJECTS:AsteriaClientObjects> TARGETS AsteriaClientObjects
                LINK_FLAGS ${component_link_flags})
            list(APPEND component_images client "${client_elf}")
        endif()
        # The `usb` module: the USB 1.1/2.0 host group driver, an image of its own because the component
# image fills its segment limit, linked with the component script and entry, whose main it defines.
        set(usb_elf "")
        if(arg_USB_SOURCES)
            asteria_add_target_objects(AsteriaUsbHostObjects
                SOURCES ${arg_USB_SOURCES}
                FLAGS ${ASTERIA_COMPONENT_COMPILE_FLAGS}
                DEFINITIONS ${ASTERIA_USER_DEFINITIONS})
            set(usb_elf "${CMAKE_BINARY_DIR}/Component/UsbHostImage.elf")
            asteria_link_user_image("${usb_elf}" "${CMAKE_BINARY_DIR}/Component/UsbHostImage.map"
                SCRIPT "${ASTERIA_ROOT}/Source/User/Platform/${ASTERIA_TARGET}/Component/${ASTERIA_TARGET}ComponentImage.lds"
                OBJECTS $<TARGET_OBJECTS:AsteriaUsbHostObjects> TARGETS AsteriaUsbHostObjects
                LINK_FLAGS ${component_link_flags})
            list(APPEND component_images usb "${usb_elf}")
        endif()
        # Composition is compiled for speed on both ISAs.
        set(display_compile_flags ${ASTERIA_COMPONENT_COMPILE_FLAGS})
        list(REMOVE_ITEM display_compile_flags -Os)
        set(display_elf "${CMAKE_BINARY_DIR}/Component/DisplayImage.elf")
        asteria_add_target_objects(AsteriaDisplayObjects
            SOURCES ${arg_DISPLAY_SOURCES}
            FLAGS ${display_compile_flags} -O2
            DEFINITIONS ${ASTERIA_USER_DEFINITIONS})
        target_compile_definitions(AsteriaDisplayObjects PRIVATE ASTERIA_COMPONENT_WATCHDOG_PROBE=$<BOOL:${ASTERIA_COMPONENT_WATCHDOG_PROBE}>)
        asteria_link_user_image("${display_elf}" "${CMAKE_BINARY_DIR}/Component/DisplayImage.map"
            SCRIPT "${ASTERIA_ROOT}/Source/User/Platform/${ASTERIA_TARGET}/Component/${ASTERIA_TARGET}ComponentImage.lds"
            OBJECTS $<TARGET_OBJECTS:AsteriaDisplayObjects> TARGETS AsteriaDisplayObjects
            LINK_FLAGS ${component_link_flags})
        list(APPEND component_images display "${display_elf}")
        # An independent full-profile image; the 64 MiB arena is charged at runtime, never a giant ELF
# segment.
        set(auth_elf "${CMAKE_BINARY_DIR}/Component/AuthImage.elf")
        asteria_add_target_objects(AsteriaAuthObjects SOURCES ${arg_AUTH_SOURCES}
            FLAGS ${display_compile_flags} -O2 -ffunction-sections -fdata-sections
            DEFINITIONS ${ASTERIA_USER_DEFINITIONS})
        asteria_link_user_image("${auth_elf}" "${CMAKE_BINARY_DIR}/Component/AuthImage.map"
            SCRIPT "${ASTERIA_ROOT}/Source/User/Platform/${ASTERIA_TARGET}/Component/${ASTERIA_TARGET}ComponentImage.lds"
            OBJECTS $<TARGET_OBJECTS:AsteriaAuthObjects> TARGETS AsteriaAuthObjects
            LINK_FLAGS ${component_link_flags} --gc-sections)
        add_custom_target(AsteriaAuthImage ALL DEPENDS "${auth_elf}")
        set(login_elf "${CMAKE_BINARY_DIR}/Component/LoginImage.elf")
        asteria_add_target_objects(AsteriaLoginObjects SOURCES ${arg_LOGIN_SOURCES}
            FLAGS ${display_compile_flags} -Os -ffunction-sections -fdata-sections
            DEFINITIONS ${ASTERIA_USER_DEFINITIONS})
        asteria_link_user_image("${login_elf}" "${CMAKE_BINARY_DIR}/Component/LoginImage.map"
            SCRIPT "${ASTERIA_ROOT}/Source/User/Platform/${ASTERIA_TARGET}/Component/${ASTERIA_TARGET}ComponentImage.lds"
            OBJECTS $<TARGET_OBJECTS:AsteriaLoginObjects> TARGETS AsteriaLoginObjects
            LINK_FLAGS ${component_link_flags} --gc-sections)
        add_custom_target(AsteriaLoginImage ALL DEPENDS "${login_elf}")
        set(fixture_tool "${CMAKE_BINARY_DIR}/HostTools/WriteIdentityFixture")
        set(fixture_sources "${ASTERIA_ROOT}/Tools/Build/WriteIdentityFixture.cpp")
        foreach(unit IN ITEMS Identity/Format Identity/AccountStore Identity/Authentication
                Identity/AttemptLimiter Crypto/Argon2 Crypto/Hash Crypto/Sha256)
            list(APPEND fixture_sources "${ASTERIA_ROOT}/Source/User/${unit}.cpp")
        endforeach()
        file(GLOB fixture_headers CONFIGURE_DEPENDS "${ASTERIA_ROOT}/Source/User/Identity/*.hpp"
            "${ASTERIA_ROOT}/Source/User/Crypto/*.hpp")
        add_custom_command(OUTPUT "${fixture_tool}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/HostTools"
            COMMAND "${ASTERIA_HOST_CXX}" -std=c++23 -O2 -fno-exceptions -fno-rtti -DASTERIA_HOSTED=1
                ${ASTERIA_TARGET_WARNING_FLAGS} -I "${ASTERIA_ROOT}/Source"
                -I "${CMAKE_BINARY_DIR}/Source" ${fixture_sources} -o "${fixture_tool}"
            DEPENDS ${fixture_sources} ${fixture_headers}
                "${CMAKE_BINARY_DIR}/Source/CryptoShared/Sha256.inc" VERBATIM)
        set(accounts_fixture "${CMAKE_BINARY_DIR}/Component/Accounts.db")
        add_custom_command(OUTPUT "${accounts_fixture}" COMMAND "${fixture_tool}" "${accounts_fixture}"
            DEPENDS "${fixture_tool}" VERBATIM)
        add_custom_target(AsteriaIdentityFixture ALL DEPENDS "${accounts_fixture}")
        # Test-only broker: withhold FaceOpen while retaining the real console.
        asteria_add_target_objects(AsteriaDisplayConsoleOnlyObjects
            SOURCES ${arg_DISPLAY_SOURCES}
            FLAGS ${display_compile_flags} -O2
            DEFINITIONS ${ASTERIA_USER_DEFINITIONS} ASTERIA_DISPLAY_CONSOLE_ONLY=1)
        set(display_console_elf "${CMAKE_BINARY_DIR}/Component/DisplayConsoleOnlyImage.elf")
        asteria_link_user_image("${display_console_elf}" "${CMAKE_BINARY_DIR}/Component/DisplayConsoleOnlyImage.map"
            SCRIPT "${ASTERIA_ROOT}/Source/User/Platform/${ASTERIA_TARGET}/Component/${ASTERIA_TARGET}ComponentImage.lds"
            OBJECTS $<TARGET_OBJECTS:AsteriaDisplayConsoleOnlyObjects> TARGETS AsteriaDisplayConsoleOnlyObjects
            LINK_FLAGS ${component_link_flags})
        add_custom_target(AsteriaDisplayConsoleOnlyImage ALL DEPENDS "${display_console_elf}")
        # The loader's demonstration: a native stub interpreter for foreign executables, linked with
        # the component script, and the foreign executables the build writes, which are data to the
        # pager.
        set(interpreter_elf "")
        set(foreign_samples "")
        if(arg_INTERPRETER_SOURCES)
            asteria_add_target_objects(AsteriaInterpreterObjects
                SOURCES ${arg_INTERPRETER_SOURCES}
                FLAGS ${ASTERIA_COMPONENT_COMPILE_FLAGS}
                DEFINITIONS ${ASTERIA_USER_DEFINITIONS})
            set(interpreter_elf "${CMAKE_BINARY_DIR}/Component/StubInterpreterImage.elf")
            asteria_link_user_image("${interpreter_elf}" "${CMAKE_BINARY_DIR}/Component/StubInterpreterImage.map"
                SCRIPT "${ASTERIA_ROOT}/Source/User/Platform/${ASTERIA_TARGET}/Component/${ASTERIA_TARGET}ComponentImage.lds"
                OBJECTS $<TARGET_OBJECTS:AsteriaInterpreterObjects> TARGETS AsteriaInterpreterObjects
                LINK_FLAGS ${component_link_flags})
            set(sample_directory "${CMAKE_BINARY_DIR}/Component/Samples")
            if(ASTERIA_TARGET STREQUAL "Ia64")
                set(guest_machine amd64)
            else()
                set(guest_machine ia64)
            endif()
            set(foreign_samples "${sample_directory}/${guest_machine}-sample.bin" "${sample_directory}/aarch64-sample.bin"
                "${sample_directory}/pe-sample.bin")
            add_custom_command(OUTPUT ${foreign_samples}
                COMMAND "${Python3_EXECUTABLE}" "${ASTERIA_ROOT}/Tools/Build/WriteForeignSamples.py"
                    --output "${sample_directory}" --native-machine "${ASTERIA_TARGET}"
                DEPENDS "${ASTERIA_ROOT}/Tools/Build/WriteForeignSamples.py" VERBATIM)
            list(APPEND component_images ${guest_machine}-stub "${interpreter_elf}"
                ${guest_machine}-sample "${sample_directory}/${guest_machine}-sample.bin"
                aarch64-sample "${sample_directory}/aarch64-sample.bin"
                pe-sample "${sample_directory}/pe-sample.bin")
        endif()
        # Test-only probe. The session VM cases rename its module receipt to
        # `session-probe`; ordinary PagerTest and product startup stay unchanged.
        set(probe_elf "${CMAKE_BINARY_DIR}/Component/SessionProbeImage.elf")
        asteria_add_target_objects(AsteriaSessionProbeObjects
            SOURCES Runtime/Memory.cpp User/System/SystemCallClient.cpp
                User/Platform/${ASTERIA_TARGET}/System/${ASTERIA_TARGET}SystemCallTransport.cpp
                User/Platform/${ASTERIA_TARGET}/System/${ASTERIA_TARGET}SystemCallTransport.S
                User/Session/SessionFaceFrame.cpp
                User/Platform/${ASTERIA_TARGET}/Component/${ASTERIA_TARGET}ComponentImage.S
                ../Tests/Guest/SessionProbe.cpp
            FLAGS ${ASTERIA_COMPONENT_COMPILE_FLAGS} -I${ASTERIA_ROOT}/Tests
            DEFINITIONS ${ASTERIA_USER_DEFINITIONS})
        asteria_link_user_image("${probe_elf}" "${CMAKE_BINARY_DIR}/Component/SessionProbeImage.map"
            SCRIPT "${ASTERIA_ROOT}/Source/User/Platform/${ASTERIA_TARGET}/Component/${ASTERIA_TARGET}ComponentImage.lds"
            OBJECTS $<TARGET_OBJECTS:AsteriaSessionProbeObjects> TARGETS AsteriaSessionProbeObjects
            LINK_FLAGS ${component_link_flags})
        list(APPEND component_images face-probe "${probe_elf}")
        # Compositor has its own speed image.
        set(compositor_elf "${CMAKE_BINARY_DIR}/Component/CompositorImage.elf")
        # AMD64's integrated -O2 text alone is 35 pages, beyond its 32-page
        # component segment limit; cold control needs size compilation too.
        set(ui_control_compile_option -Os)
        set(ui_font_compile_option -O2)
        if(ASTERIA_TARGET STREQUAL "Ia64")
            # IA-64 compositor code fits the boot staging bound at -O2.
            set(ui_control_compile_option -O2)
        endif()
        asteria_add_target_objects(AsteriaCompositorObjects
            SOURCES Runtime/Memory.cpp Runtime/Placement.cpp User/System/SystemCallClient.cpp
                User/Platform/${ASTERIA_TARGET}/System/${ASTERIA_TARGET}SystemCallTransport.cpp
                User/Platform/${ASTERIA_TARGET}/System/${ASTERIA_TARGET}SystemCallTransport.S
                User/Session/Scene.cpp User/Session/Decorations.cpp User/Session/UiText.cpp User/Session/UiTextDrawing.cpp
                User/Graphics/Composition/Composition.cpp
                User/Platform/${ASTERIA_TARGET}/Component/${ASTERIA_TARGET}ComponentImage.S
            FLAGS ${display_compile_flags} -O2 -ffunction-sections -fdata-sections DEFINITIONS ${ASTERIA_USER_DEFINITIONS})
        asteria_add_target_objects(AsteriaSessionControlObjects
            SOURCES User/Session/SessionFrame.cpp User/Session/SessionFaceFrame.cpp
                User/Session/ClientEvents.cpp User/Session/WindowManager.cpp
                User/Session/InputOrder.cpp User/Input/Keymap.cpp User/Component/SessionLiveness.cpp ${arg_COMPOSITOR_SOURCES}
            FLAGS ${display_compile_flags} ${ui_control_compile_option} -ffunction-sections -fdata-sections DEFINITIONS ${ASTERIA_USER_DEFINITIONS})
        # Raster scanning stays speed compiled; Both ISAs speed compile all font validation/shaping.
        asteria_add_target_objects(AsteriaUiFontObjects
            SOURCES User/Font/Font.cpp User/Font/Cff.cpp User/Font/Type2.cpp
                User/Font/Layout.cpp User/Font/LayoutTables.cpp User/Font/Cache.cpp
            FLAGS ${display_compile_flags} ${ui_font_compile_option} -ffunction-sections -fdata-sections DEFINITIONS ${ASTERIA_USER_DEFINITIONS})
        asteria_add_target_objects(AsteriaUiRasterObjects
            SOURCES User/Font/Raster.cpp
            FLAGS ${display_compile_flags} -O2 -ffunction-sections -fdata-sections DEFINITIONS ${ASTERIA_USER_DEFINITIONS})
        file(READ "${ASTERIA_ROOT}/Source/User/Platform/${ASTERIA_TARGET}/Component/${ASTERIA_TARGET}ComponentImage.lds" ui_script)
        string(REPLACE "    ReadOnlyData PT_LOAD FLAGS(4);" "" ui_script "${ui_script}")
        string(REPLACE ":ReadOnlyData" ":Text" ui_script "${ui_script}")
        file(WRITE "${CMAKE_BINARY_DIR}/Component/UiCompositor.lds" "${ui_script}")
        asteria_link_user_image("${compositor_elf}" "${CMAKE_BINARY_DIR}/Component/CompositorImage.map"
            SCRIPT "${CMAKE_BINARY_DIR}/Component/UiCompositor.lds"
            OBJECTS $<TARGET_OBJECTS:AsteriaCompositorObjects> $<TARGET_OBJECTS:AsteriaUiFontObjects> $<TARGET_OBJECTS:AsteriaUiRasterObjects> $<TARGET_OBJECTS:AsteriaSessionControlObjects>
            TARGETS AsteriaCompositorObjects AsteriaUiFontObjects AsteriaUiRasterObjects AsteriaSessionControlObjects LINK_FLAGS ${component_link_flags} --gc-sections)
        # Test-only compositor: healthy admission, then withheld watchdog replies.
        asteria_add_target_objects(AsteriaSessionLivenessFaultObjects
            SOURCES ../Tests/Guest/SessionLivenessFault.cpp
            FLAGS ${display_compile_flags} ${ui_control_compile_option} -ffunction-sections -fdata-sections
            DEFINITIONS ${ASTERIA_USER_DEFINITIONS})
        set(compositor_fault_elf "${CMAKE_BINARY_DIR}/Component/CompositorLivenessFaultImage.elf")
        asteria_link_user_image("${compositor_fault_elf}" "${CMAKE_BINARY_DIR}/Component/CompositorLivenessFaultImage.map"
            SCRIPT "${CMAKE_BINARY_DIR}/Component/UiCompositor.lds"
            OBJECTS $<TARGET_OBJECTS:AsteriaCompositorObjects> $<TARGET_OBJECTS:AsteriaUiFontObjects> $<TARGET_OBJECTS:AsteriaUiRasterObjects>
                "$<FILTER:$<TARGET_OBJECTS:AsteriaSessionControlObjects>,EXCLUDE,/SessionLiveness[.]cpp[.]>" $<TARGET_OBJECTS:AsteriaSessionLivenessFaultObjects>
            TARGETS AsteriaCompositorObjects AsteriaUiFontObjects AsteriaUiRasterObjects AsteriaSessionControlObjects AsteriaSessionLivenessFaultObjects
            LINK_FLAGS ${component_link_flags} --gc-sections)
        add_custom_target(AsteriaCompositorLivenessFaultImage ALL DEPENDS "${compositor_fault_elf}")
        # Test-only bounded scene/input scenario: one shared RX segment keeps
        # the twenty-object IA-64 grant. No writable page gains Execute.
        file(READ "${ASTERIA_ROOT}/Source/User/Platform/${ASTERIA_TARGET}/Component/${ASTERIA_TARGET}ComponentImage.lds" session_test_script)
        string(REPLACE "    ReadOnlyData PT_LOAD FLAGS(4);" "" session_test_script "${session_test_script}")
        string(REPLACE ":ReadOnlyData" ":Text" session_test_script "${session_test_script}")
        file(WRITE "${CMAKE_BINARY_DIR}/Component/SessionTest.lds" "${session_test_script}")
        set(input_seat_elf "${CMAKE_BINARY_DIR}/Component/InputSeatImage.elf")
        asteria_add_target_objects(AsteriaInputSeatObjects
            SOURCES Runtime/Memory.cpp Runtime/Placement.cpp User/System/SystemCallClient.cpp
                User/Platform/${ASTERIA_TARGET}/System/${ASTERIA_TARGET}SystemCallTransport.cpp
                User/Platform/${ASTERIA_TARGET}/System/${ASTERIA_TARGET}SystemCallTransport.S
                User/Component/InputBroker.cpp User/Component/InputBrokerLogic.cpp User/Component/ServiceScratch.cpp
                User/Component/Ps2Driver.cpp User/Input/Ps2Controller.cpp User/Input/Ps2ControllerSetup.cpp
                User/Input/Ps2EventQueue.cpp User/Input/Ps2Keyboard.cpp User/Input/Ps2Mouse.cpp
                ../Tests/Guest/InputSeat.cpp ../Tests/Guest/Session/Ps2PublicationNotice.cpp
                User/Platform/${ASTERIA_TARGET}/Component/${ASTERIA_TARGET}ComponentImage.S
            FLAGS ${ASTERIA_COMPONENT_COMPILE_FLAGS} -ffunction-sections -fdata-sections DEFINITIONS ${ASTERIA_USER_DEFINITIONS} ASTERIA_PS2_SHARED_SCRATCH=1)
        asteria_link_user_image("${input_seat_elf}" "${CMAKE_BINARY_DIR}/Component/InputSeatImage.map"
            SCRIPT "${CMAKE_BINARY_DIR}/Component/SessionTest.lds"
            OBJECTS $<TARGET_OBJECTS:AsteriaInputSeatObjects> TARGETS AsteriaInputSeatObjects LINK_FLAGS ${component_link_flags} --gc-sections)
        set(pattern_elf "${CMAKE_BINARY_DIR}/Component/PatternImage.elf")
        asteria_add_target_objects(AsteriaPatternObjects
            SOURCES Runtime/Memory.cpp User/System/SystemCallClient.cpp
                User/Platform/${ASTERIA_TARGET}/System/${ASTERIA_TARGET}SystemCallTransport.cpp
                User/Platform/${ASTERIA_TARGET}/System/${ASTERIA_TARGET}SystemCallTransport.S
                Inventory/ReportText.cpp User/Session/SessionFrame.cpp User/Session/SessionFaceFrame.cpp ../Tests/Guest/PatternClient.cpp
                User/Platform/${ASTERIA_TARGET}/Component/${ASTERIA_TARGET}ComponentImage.S
            FLAGS ${ASTERIA_COMPONENT_COMPILE_FLAGS} -ffunction-sections -fdata-sections -I${ASTERIA_ROOT}/Tests DEFINITIONS ${ASTERIA_USER_DEFINITIONS})
        asteria_link_user_image("${pattern_elf}" "${CMAKE_BINARY_DIR}/Component/PatternImage.map"
            SCRIPT "${CMAKE_BINARY_DIR}/Component/SessionTest.lds"
            OBJECTS $<TARGET_OBJECTS:AsteriaPatternObjects> TARGETS AsteriaPatternObjects LINK_FLAGS ${component_link_flags} --gc-sections)
        list(APPEND component_images compositor "${compositor_elf}" pattern "${pattern_elf}" input-seat "${input_seat_elf}")
        # Own component image and domain, native-mode pools; no CM yet.
        file(GLOB toolkit_sources CONFIGURE_DEPENDS RELATIVE "${ASTERIA_ROOT}/Source" "${ASTERIA_ROOT}/Source/User/Toolkit/*.cpp")
        file(GLOB toolkit_font_sources CONFIGURE_DEPENDS RELATIVE "${ASTERIA_ROOT}/Source" "${ASTERIA_ROOT}/Source/User/Font/*.cpp")
        file(GLOB toolkit_graphics_sources CONFIGURE_DEPENDS RELATIVE "${ASTERIA_ROOT}/Source" "${ASTERIA_ROOT}/Source/User/Graphics/*.cpp" "${ASTERIA_ROOT}/Source/User/Graphics/Composition/*.cpp")
        asteria_add_target_objects(AsteriaToolkitObjects
            SOURCES ${toolkit_sources} ${toolkit_font_sources} ${toolkit_graphics_sources}
                Inventory/ReportText.cpp
                Runtime/Memory.cpp Runtime/Placement.cpp User/System/SystemCallClient.cpp
                User/Session/SessionFrame.cpp User/Session/ClientConnection.cpp
                User/Input/Keymap.cpp User/Input/KeyRepeat.cpp
                User/Platform/${ASTERIA_TARGET}/System/${ASTERIA_TARGET}SystemCallTransport.cpp
                User/Platform/${ASTERIA_TARGET}/System/${ASTERIA_TARGET}SystemCallTransport.S
                User/Platform/${ASTERIA_TARGET}/Component/${ASTERIA_TARGET}ToolkitImage.cpp
                User/Platform/${ASTERIA_TARGET}/Component/${ASTERIA_TARGET}ComponentImage.S
            FLAGS ${display_compile_flags} -Os -ffunction-sections -fdata-sections
            DEFINITIONS ${ASTERIA_USER_DEFINITIONS})
        set(toolkit_elf "${CMAKE_BINARY_DIR}/Component/ToolkitImage.elf")
        asteria_link_user_image("${toolkit_elf}" "${CMAKE_BINARY_DIR}/Component/ToolkitImage.map"
            SCRIPT "${CMAKE_BINARY_DIR}/Component/UiCompositor.lds"
            OBJECTS $<TARGET_OBJECTS:AsteriaToolkitObjects> TARGETS AsteriaToolkitObjects
            LINK_FLAGS ${component_link_flags} --gc-sections)
        list(APPEND component_images toolkit "${toolkit_elf}")


        # Recompute the platform grant peaks from the linked PT_LOADs before
        # installation: image growth must fail here instead of ResourceExhausted
        # inside a demonstration. CTest repeats the same check independently.
        set(grant_checker "${ASTERIA_ROOT}/Tools/Build/CheckPagerGrant.py")
        set(grant_images "${component_elf}" "${directory_elf}" "${client_elf}" "${interpreter_elf}" ${usb_elf})
        set(grant_command "${Python3_EXECUTABLE}" "${grant_checker}" --root "${ASTERIA_ROOT}"
            --isa "${ASTERIA_TARGET}" --display "${display_elf}" --probe "${probe_elf}" --compositor "${compositor_elf}" --pattern "${pattern_elf}" --input-seat "${input_seat_elf}" --toolkit "${toolkit_elf}" ${grant_images})
        add_custom_target(AsteriaPagerGrantCheck ALL
            COMMAND ${grant_command}
            DEPENDS ${grant_images} "${display_elf}" "${probe_elf}" "${compositor_elf}" "${pattern_elf}" "${input_seat_elf}" "${toolkit_elf}" ${foreign_samples} "${grant_checker}" VERBATIM)
        add_test(NAME TargetPagerGrant COMMAND ${grant_command})
        set_tests_properties(TargetPagerGrant PROPERTIES LABELS static)
        set(product_images component "${component_elf}" directory "${directory_elf}" display "${display_elf}" toolkit "${toolkit_elf}")
        if(usb_elf)
            list(APPEND product_images usb "${usb_elf}")
        endif()
    endif()
    # The IA-64 test pager's CPU-bound spinner (Tests/Guest/Pager/Ia64Spinner.S),
    # linked with the component image script and embedded in the test pager
    # as read-only data, which the pager copies to an RX page and runs as a
    # created thread. It is not a boot file: the ESP carries no SPINNER.ELF.
    set(ia64_spinner_blob "")
    set(ia64_spinner_elf "")
    if(ASTERIA_TARGET STREQUAL "Ia64")
        add_library(AsteriaIa64TestSpinnerObjects OBJECT "${ASTERIA_ROOT}/Tests/Guest/Pager/Ia64Spinner.S")
        target_include_directories(AsteriaIa64TestSpinnerObjects PRIVATE "${CMAKE_BINARY_DIR}")
        asteria_target_language_options(AsteriaIa64TestSpinnerObjects ${ASTERIA_USER_COMPILE_FLAGS})
        add_dependencies(AsteriaIa64TestSpinnerObjects AsteriaAsmLayout)
        set(ia64_spinner_elf "${CMAKE_BINARY_DIR}/Spinner/SpinnerImage.elf")
        file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/Spinner")
        asteria_link_user_image("${ia64_spinner_elf}" "${CMAKE_BINARY_DIR}/Spinner/SpinnerImage.map"
            SCRIPT "${ASTERIA_ROOT}/Source/User/Platform/Ia64/Component/Ia64ComponentImage.lds"
            OBJECTS $<TARGET_OBJECTS:AsteriaIa64TestSpinnerObjects> TARGETS AsteriaIa64TestSpinnerObjects)
        set(ia64_spinner_blob "${CMAKE_BINARY_DIR}/Spinner/SpinnerBlob.S")
        file(GENERATE OUTPUT "${ia64_spinner_blob}" CONTENT
            ".section .rodata.SpinnerImage,\"a\",@progbits\n.align 16\n.global AsteriaIa64SpinnerImageBytes\n.global AsteriaIa64SpinnerImageEnd\nAsteriaIa64SpinnerImageBytes:\n.incbin \"${ia64_spinner_elf}\"\nAsteriaIa64SpinnerImageEnd:\n")
        set_source_files_properties("${ia64_spinner_blob}" PROPERTIES OBJECT_DEPENDS "${ia64_spinner_elf}")
        set(ASTERIA_IA64_TEST_SPINNER_ELF "${ia64_spinner_elf}" PARENT_SCOPE)
        set(ASTERIA_IA64_TEST_SPINNER_BLOB "${ia64_spinner_blob}" PARENT_SCOPE)
    endif()
    # A test-only shared-root worker image, embedded in the test pager; product media contains no SMP
# probe.
    set(amd64_smp_blob "")
    if(ASTERIA_TARGET STREQUAL "Amd64")
        add_library(AsteriaAmd64SmpObjects OBJECT "${ASTERIA_ROOT}/Tests/Guest/Pager/Amd64SmpWorkers.S")
        asteria_target_language_options(AsteriaAmd64SmpObjects ${ASTERIA_USER_COMPILE_FLAGS})
        set(amd64_smp_elf "${CMAKE_BINARY_DIR}/Smp/SmpWorkers.elf")
        file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/Smp")
        asteria_link_user_image("${amd64_smp_elf}" "${CMAKE_BINARY_DIR}/Smp/SmpWorkers.map"
            SCRIPT "${ASTERIA_ROOT}/Source/User/Platform/Amd64/Component/Amd64ComponentImage.lds"
            OBJECTS $<TARGET_OBJECTS:AsteriaAmd64SmpObjects> TARGETS AsteriaAmd64SmpObjects)
        set(amd64_smp_blob "${CMAKE_BINARY_DIR}/Smp/SmpBlob.S")
        file(GENERATE OUTPUT "${amd64_smp_blob}" CONTENT
            ".section .rodata.SmpImage,\"a\",@progbits\n.align 16\n.global AsteriaAmd64SmpImageBytes\n.global AsteriaAmd64SmpImageEnd\nAsteriaAmd64SmpImageBytes:\n.incbin \"${amd64_smp_elf}\"\nAsteriaAmd64SmpImageEnd:\n")
        set_source_files_properties("${amd64_smp_blob}" PROPERTIES OBJECT_DEPENDS "${amd64_smp_elf}")
        set(ASTERIA_AMD64_TEST_SMP_ELF "${amd64_smp_elf}" PARENT_SCOPE)
    endif()
    # The boot pager image installed beside each firmware's initial image.
    set(pager_elf "")
    set(pager_test_elf "")
    if(arg_PAGER_SOURCES)
        asteria_add_target_objects(AsteriaPagerObjects
            SOURCES ${arg_PAGER_SOURCES}
            FLAGS ${ASTERIA_USER_COMPILE_FLAGS}
            DEFINITIONS ${ASTERIA_USER_DEFINITIONS})
        # Root control and AP/entry probes must fit bounded boot staging. Pixel work and hashing have
        # separate object targets.
        if(ASTERIA_TARGET STREQUAL "Amd64")
            target_compile_options(AsteriaPagerObjects PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Os>)
        endif()
        file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/Pager")
        set(pager_elf "${CMAKE_BINARY_DIR}/Pager/PagerImage.elf")
        asteria_link_user_image("${pager_elf}" "${CMAKE_BINARY_DIR}/Pager/PagerImage.map"
            FIXED_ADDRESS_REASON "Bootstrap image staged once; shared executable segments applies to components"
            SCRIPT "${ASTERIA_ROOT}/Source/User/Platform/${ASTERIA_TARGET}/Pager/${ASTERIA_TARGET}PagerImage.lds"
            OBJECTS $<TARGET_OBJECTS:AsteriaPagerObjects> TARGETS AsteriaPagerObjects)
        file(GLOB pager_test_sources CONFIGURE_DEPENDS "${ASTERIA_ROOT}/Tests/Guest/Pager/*.cpp")
        if(ASTERIA_TARGET STREQUAL "Ia64")
            list(FILTER pager_test_sources EXCLUDE REGEX "/(Amd64PagerTestImage|PortsDemo)[.]cpp$")
        else()
            list(FILTER pager_test_sources EXCLUDE REGEX "/Ia64PagerTestImage[.]cpp$")
        endif()
        if(EXISTS "${ASTERIA_ROOT}/Tests/Guest/Pager/${ASTERIA_TARGET}FaultScenarios.S")
            list(APPEND pager_test_sources "${ASTERIA_ROOT}/Tests/Guest/Pager/${ASTERIA_TARGET}FaultScenarios.S")
        endif()
        if(amd64_smp_blob)
            list(APPEND pager_test_sources "${amd64_smp_blob}")
        endif()
        if(ia64_spinner_blob)
            list(APPEND pager_test_sources "${ia64_spinner_blob}")
        endif()
        add_library(AsteriaPagerTestObjects OBJECT ${pager_test_sources})
        target_include_directories(AsteriaPagerTestObjects PRIVATE "${ASTERIA_ROOT}/Source" "${ASTERIA_ROOT}/Tests")
        asteria_target_language_options(AsteriaPagerTestObjects ${ASTERIA_USER_COMPILE_FLAGS})
        if(ASTERIA_TARGET STREQUAL "Amd64")
            target_compile_options(AsteriaPagerTestObjects PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Os>)
        endif()
        target_compile_definitions(AsteriaPagerTestObjects PRIVATE ${ASTERIA_USER_DEFINITIONS} ASTERIA_COMPONENT_WATCHDOG_PROBE=$<BOOL:${ASTERIA_COMPONENT_WATCHDOG_PROBE}>)
        set(pager_shared_objects
            "$<FILTER:$<TARGET_OBJECTS:AsteriaPagerObjects>,EXCLUDE,/(${ASTERIA_TARGET}PagerImage|ProductInit)[.]cpp[.]>")
        set(pager_test_elf "${CMAKE_BINARY_DIR}/Pager/PagerTestImage.elf")
        asteria_link_user_image("${pager_test_elf}" "${CMAKE_BINARY_DIR}/Pager/PagerTestImage.map"
            FIXED_ADDRESS_REASON "Bootstrap image staged once; shared executable segments applies to components"
            SCRIPT "${ASTERIA_ROOT}/Source/User/Platform/${ASTERIA_TARGET}/Pager/${ASTERIA_TARGET}PagerImage.lds"
            OBJECTS ${pager_shared_objects} $<TARGET_OBJECTS:AsteriaPagerTestObjects>
            TARGETS AsteriaPagerObjects AsteriaPagerTestObjects)
    endif()
    # The boot stages each pager image in a bounded window (AMD64: one slot
    # beside its stack; IA-64: segment, total-page and segment-count limits).
    # Growth past it would fail only as the boot stages the image, so the
    # build fails here, printing the sizes; CTest repeats the same check.
    if(pager_elf)
        asteria_pager_staging_command(pager_staging_command "${pager_elf}" "${pager_test_elf}")
        add_custom_target(AsteriaPagerStagingCheck ALL
            COMMAND ${pager_staging_command}
            DEPENDS "${pager_elf}" "${pager_test_elf}" "${ASTERIA_ROOT}/Tools/Build/CheckImageStaging.py" VERBATIM)
    endif()
    # Both are user images Tools/Verify judges as compiled PL3 code.
    set(ASTERIA_PAGER_ELF "${pager_elf}" PARENT_SCOPE)
    set(ASTERIA_PAGER_TEST_ELF "${pager_test_elf}" PARENT_SCOPE)
    if(arg_COMPONENT_SOURCES)
        set(ASTERIA_COMPONENT_ELF "${component_elf}" PARENT_SCOPE)
        set(ASTERIA_DIRECTORY_ELF "${directory_elf}" PARENT_SCOPE)
        set(ASTERIA_CLIENT_ELF "${client_elf}" PARENT_SCOPE)
        set(ASTERIA_INTERPRETER_ELF "${interpreter_elf}" PARENT_SCOPE)
        set(ASTERIA_USB_ELF "${usb_elf}" PARENT_SCOPE)
        set(ASTERIA_DISPLAY_ELF "${display_elf}" PARENT_SCOPE)
        set(ASTERIA_AUTH_ELF "${auth_elf}" PARENT_SCOPE)
        set(ASTERIA_LOGIN_ELF "${login_elf}" PARENT_SCOPE)
        set(ASTERIA_DISPLAY_CONSOLE_ELF "${display_console_elf}" PARENT_SCOPE)
        set(ASTERIA_COMPOSITOR_ELF "${compositor_elf}" PARENT_SCOPE)
        set(ASTERIA_COMPOSITOR_FAULT_ELF "${compositor_fault_elf}" PARENT_SCOPE)
        set(ASTERIA_PATTERN_ELF "${pattern_elf}" PARENT_SCOPE)
        set(ASTERIA_TOOLKIT_ELF "${toolkit_elf}" PARENT_SCOPE)
        set(ASTERIA_INPUT_SEAT_ELF "${input_seat_elf}" PARENT_SCOPE)
        set(ASTERIA_SESSION_PROBE_ELF "${probe_elf}" PARENT_SCOPE)
    else()
        set(ASTERIA_COMPONENT_ELF "" PARENT_SCOPE)
        set(ASTERIA_DIRECTORY_ELF "" PARENT_SCOPE)
        set(ASTERIA_CLIENT_ELF "" PARENT_SCOPE)
        set(ASTERIA_INTERPRETER_ELF "" PARENT_SCOPE)
        set(ASTERIA_USB_ELF "" PARENT_SCOPE)
    endif()
    # The CPU-bound spinner of the test firmware.
    set(spinner_elf "")
    if(arg_SPINNER_SOURCES)
        asteria_add_target_objects(AsteriaSpinnerObjects
            SOURCES ${arg_SPINNER_SOURCES}
            FLAGS ${ASTERIA_USER_COMPILE_FLAGS}
            DEFINITIONS ${ASTERIA_USER_DEFINITIONS})
        file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/Spinner")
        set(spinner_elf "${CMAKE_BINARY_DIR}/Spinner/SpinnerImage.elf")
        asteria_link_user_image("${spinner_elf}" "${CMAKE_BINARY_DIR}/Spinner/SpinnerImage.map"
            FIXED_ADDRESS_REASON "Bootstrap image staged once; shared executable segments applies to components"
            SCRIPT "${ASTERIA_ROOT}/Source/User/Platform/${ASTERIA_TARGET}/Spinner/${ASTERIA_TARGET}SpinnerImage.lds"
            OBJECTS $<TARGET_OBJECTS:AsteriaSpinnerObjects> TARGETS AsteriaSpinnerObjects)
    endif()

    file(GLOB guest_sources CONFIGURE_DEPENDS
        "${ASTERIA_ROOT}/Tests/Guest/*.cpp"
        "${ASTERIA_ROOT}/Tests/Guest/${ASTERIA_TARGET}/*.cpp"
        "${ASTERIA_ROOT}/Tests/Guest/${ASTERIA_TARGET}/*.S")
    list(FILTER guest_sources EXCLUDE REGEX "/SessionProbe[.]cpp$")
    list(FILTER guest_sources EXCLUDE REGEX "/PatternClient[.]cpp$")
    list(FILTER guest_sources EXCLUDE REGEX "/InputSeat[.]cpp$")
    add_library(AsteriaGuestObjects OBJECT ${guest_sources})
    target_include_directories(AsteriaGuestObjects PRIVATE "${ASTERIA_ROOT}/Source" "${ASTERIA_ROOT}/Tests")
    asteria_target_language_options(AsteriaGuestObjects ${ASTERIA_USER_COMPILE_FLAGS})
    target_compile_definitions(AsteriaGuestObjects PRIVATE ${ASTERIA_USER_DEFINITIONS})

    if(ASTERIA_TARGET STREQUAL "Ia64")
        # The normalizer that rewrites the IA-64 base relocation and section
        # contract of both PE conversions.
        set(normalizer "${CMAKE_BINARY_DIR}/HostTools/NormalizeIa64Pe")
        add_custom_command(OUTPUT "${normalizer}"
            COMMAND "${ASTERIA_HOST_CXX}" -std=c++23 -O2 -fno-exceptions -fno-rtti -fno-common
                -Wall -Wextra -Wpedantic -Werror -Wconversion -Wsign-conversion -Wshadow
                -Wcast-align=strict -Wcast-qual -Wold-style-cast
                "${ASTERIA_ROOT}/Tools/Firmware/NormalizeIa64Pe.cpp" -o "${normalizer}"
            DEPENDS "${ASTERIA_ROOT}/Tools/Firmware/NormalizeIa64Pe.cpp" VERBATIM)
        # Host build of the production translation planners driven by the
        # final-image residue proof mode of InitialTranslationResourcePlanTests.
        set(residue_proof "${CMAKE_BINARY_DIR}/HostTools/Ia64FinalImageTranslationResidueProof")
        set(residue_sources
            "${ASTERIA_ROOT}/Source/Kernel/Memory/InitialMappingPlan.cpp"
            "${ASTERIA_ROOT}/Source/Platform/Ia64/Memory/BootstrapMappingPlan.cpp"
            "${ASTERIA_ROOT}/Source/Platform/Ia64/Memory/InitialTranslationEncoder.cpp"
            "${ASTERIA_ROOT}/Source/Platform/Ia64/Memory/InitialTranslationResourcePlan.cpp"
            "${ASTERIA_ROOT}/Tests/Platform/Ia64/Memory/InitialTranslationResourcePlanTests.cpp")
        file(GLOB_RECURSE residue_headers CONFIGURE_DEPENDS
            "${ASTERIA_ROOT}/Source/*.hpp" "${ASTERIA_ROOT}/Tests/Support/*.hpp")
        add_custom_command(OUTPUT "${residue_proof}"
            COMMAND "${ASTERIA_HOST_CXX}" -std=c++23 -O2 -ffreestanding -fno-exceptions -fno-rtti
                -fno-threadsafe-statics -fno-use-cxa-atexit -fno-common ${ASTERIA_TARGET_WARNING_FLAGS}
                -I "${ASTERIA_ROOT}/Source" -I "${ASTERIA_ROOT}/Tests"
                ${residue_sources} -o "${residue_proof}"
            DEPENDS ${residue_sources} ${residue_headers}
            VERBATIM)
        add_custom_target(AsteriaResidueProof ALL DEPENDS "${residue_proof}")
        set(relocation_proof "${CMAKE_BINARY_DIR}/HostTools/Ia64ImageRelocationProof")
        set(relocation_sources
            "${ASTERIA_ROOT}/Source/Platform/Ia64/Firmware/Ia64LoadedImageLayout.cpp"
            "${ASTERIA_ROOT}/Source/Platform/Ia64/Firmware/Ia64ImageRelocation.cpp"
            "${ASTERIA_ROOT}/Tests/Platform/Ia64/Firmware/Ia64ImageRelocationTests.cpp")
        add_custom_command(OUTPUT "${relocation_proof}"
            COMMAND "${ASTERIA_HOST_CXX}" -std=c++23 -O2 ${ASTERIA_TARGET_WARNING_FLAGS}
                -I "${ASTERIA_ROOT}/Source" -I "${ASTERIA_ROOT}/Tests"
                ${relocation_sources} -o "${relocation_proof}"
            DEPENDS ${relocation_sources} ${residue_headers} VERBATIM)
        add_custom_target(AsteriaRelocationProof ALL DEPENDS "${relocation_proof}")
        add_test(NAME Ia64ImageRelocationSynthetic COMMAND "${relocation_proof}")
        add_test(NAME Ia64ImageRelocationProduction
            COMMAND "${relocation_proof}" "${CMAKE_BINARY_DIR}/Firmware/AsteriaBoot.efi")
        add_test(NAME Ia64ImageRelocationGuest
            COMMAND "${relocation_proof}" "${CMAKE_BINARY_DIR}/FirmwareTest/AsteriaBootTest.efi")
        set_tests_properties(Ia64ImageRelocationSynthetic Ia64ImageRelocationProduction
            Ia64ImageRelocationGuest PROPERTIES LABELS static)
        set(ASTERIA_FIRMWARE_RESIDUE_PROOF "${residue_proof}" PARENT_SCOPE)
    endif()

    list(APPEND product_images compositor "${compositor_elf}" auth "${auth_elf}" login "${login_elf}")
    # CMake names the object of Source/<unit>.../AsteriaUserObjects.dir/<unit>.obj
    # (with a./ component under Ninja), so /<unit>. matches only USER_MAIN.
    string(REPLACE "." "[.]" main "${arg_USER_MAIN}")
    set(runtime_objects "$<FILTER:$<TARGET_OBJECTS:AsteriaUserObjects>,EXCLUDE,/${main}[.]>")
    set(test_user_objects "${runtime_objects}" "$<TARGET_OBJECTS:AsteriaGuestObjects>")
    asteria_link_firmware(Firmware NAME AsteriaBoot USER_NAME MinimalUserImage TARGET AsteriaFirmware
        PREFIX production PAGER_ELF "${pager_elf}" MODULES ${product_images}
        USER_OBJECTS $<TARGET_OBJECTS:AsteriaUserObjects> USER_TARGETS AsteriaUserObjects)
    # Synthetic login is a separate test medium until owner provisioning is
    # settled. Production BOOT.CFG explicitly retains login=off.
    asteria_link_firmware(FirmwareLogin NAME AsteriaBootLogin USER_NAME LoginTestUserImage TARGET AsteriaFirmwareLogin
        PREFIX login PAGER_ELF "${pager_elf}" MODULES ${product_images} accounts "${accounts_fixture}" seat-test "${ASTERIA_ROOT}/Data/Identity/SeatTest.txt"
        USER_OBJECTS $<TARGET_OBJECTS:AsteriaUserObjects> USER_TARGETS AsteriaUserObjects)
    set(optional_spinner)
    if(spinner_elf)
        set(optional_spinner SPINNER_ELF "${spinner_elf}")
    endif()
    asteria_link_firmware(FirmwareTest NAME AsteriaBootTest USER_NAME GuestTests TARGET AsteriaFirmwareTest
        PREFIX test PAGER_ELF "${pager_test_elf}" MODULES ${component_images} ${optional_spinner}
        USER_OBJECTS ${test_user_objects} USER_TARGETS AsteriaUserObjects AsteriaGuestObjects)

    foreach(output IN ITEMS USER_ELF KERNEL LOADER EFI ESP)
        set(ASTERIA_FIRMWARE_${output} "${production_${output}}" PARENT_SCOPE)
        set(ASTERIA_FIRMWARE_TEST_${output} "${test_${output}}" PARENT_SCOPE)
        set(ASTERIA_FIRMWARE_LOGIN_${output} "${login_${output}}" PARENT_SCOPE)
    endforeach()
    set(ASTERIA_FIRMWARE_TEST_USER_OBJECTS "${test_user_objects}" PARENT_SCOPE)
endfunction()

# Every header, including the other ISA's platform headers, must compile on
# its own with this target's compiler and ABI.
function(asteria_add_target_header_units)
    file(GLOB_RECURSE headers CONFIGURE_DEPENDS
        RELATIVE "${ASTERIA_ROOT}/Source" "${ASTERIA_ROOT}/Source/*.hpp")
    set(units)
    foreach(header IN LISTS headers)
        string(MAKE_C_IDENTIFIER "${header}" unit_name)
        set(unit "${CMAKE_BINARY_DIR}/HeaderUnits/${unit_name}.cpp")
        file(CONFIGURE OUTPUT "${unit}" CONTENT "#include \"${header}\"\n")
        if(header MATCHES "^User/(Libc|Runtime)/")
            set_source_files_properties("${unit}" PROPERTIES
                COMPILE_OPTIONS "-isystem;${ASTERIA_COMPILER_C_HEADERS}")
        endif()
        list(APPEND units "${unit}")
    endforeach()
    add_library(AsteriaTargetHeaderSelfContainment OBJECT ${units})
    target_include_directories(AsteriaTargetHeaderSelfContainment PRIVATE "${ASTERIA_ROOT}/Source")
    target_compile_options(AsteriaTargetHeaderSelfContainment PRIVATE ${ASTERIA_KERNEL_COMPILE_FLAGS})
    target_compile_definitions(AsteriaTargetHeaderSelfContainment PRIVATE ${ASTERIA_KERNEL_DEFINITIONS})
endfunction()

# Registers the static checks of the firmware objects with CTest.
function(asteria_add_target_checks)
    get_property(image_gates GLOBAL PROPERTY ASTERIA_SHARED_USER_IMAGE_GATES)
    if(NOT image_gates)
        message(FATAL_ERROR "No shared user images registered for the second-link gate")
    endif()
    add_test(NAME TargetUserImageGates
        COMMAND "${Python3_EXECUTABLE}" "${ASTERIA_ROOT}/Tools/Build/CheckImageTextRelocations.py" ${image_gates})
    set_tests_properties(TargetUserImageGates PROPERTIES LABELS static)
    set(pager_objects)
    if(TARGET AsteriaPagerObjects)
        set(pager_objects "$<TARGET_OBJECTS:AsteriaPagerObjects>")
    endif()
    if(TARGET AsteriaPagerTestObjects)
        list(APPEND pager_objects "$<TARGET_OBJECTS:AsteriaPagerTestObjects>")
    endif()
    if(TARGET AsteriaSpinnerObjects)
        list(APPEND pager_objects "$<TARGET_OBJECTS:AsteriaSpinnerObjects>")
    endif()
    # The component the root service builds joins the budget: its services keep their delivery
    # buffers and request tables static.
    foreach(component_target IN ITEMS AsteriaComponentObjects AsteriaComponentSizeObjects AsteriaDirectoryObjects
            AsteriaClientObjects AsteriaInterpreterObjects AsteriaUsbHostObjects AsteriaDisplayObjects AsteriaSessionProbeObjects AsteriaCompositorObjects AsteriaSessionControlObjects AsteriaUiFontObjects AsteriaUiRasterObjects AsteriaPatternObjects AsteriaInputSeatObjects AsteriaAuthObjects AsteriaLoginObjects)
        if(TARGET ${component_target})
            list(APPEND pager_objects "$<TARGET_OBJECTS:${component_target}>")
        endif()
    endforeach()
    add_test(NAME TargetStackUsage
        COMMAND "${Python3_EXECUTABLE}" "${ASTERIA_ROOT}/Tools/Build/CheckStackUsage.py"
            --limit ${ASTERIA_TARGET_STACK_BUDGET_BYTES}
            "$<TARGET_OBJECTS:AsteriaKernelObjects>" "$<TARGET_OBJECTS:AsteriaUserObjects>"
            "$<TARGET_OBJECTS:AsteriaGuestObjects>" ${pager_objects})
    set_tests_properties(TargetStackUsage PROPERTIES LABELS static)
    # The boot stages the pager image's loadable segments in a bounded window (AMD64: a fixed slot
    # beside its stack, Documentation/Architecture/FilesystemProtocol.md; IA-64:
    # Ia64InitialUserImageOwner's limits): an image that does not fit fails here, not when a machine
    # boots it.
    if(ASTERIA_PAGER_ELF)
        asteria_pager_staging_command(pager_staging_command "${ASTERIA_PAGER_ELF}")
        add_test(NAME TargetPagerStaging COMMAND ${pager_staging_command})
        set_tests_properties(TargetPagerStaging PROPERTIES LABELS static)
        asteria_pager_staging_command(pager_test_staging_command "${ASTERIA_PAGER_TEST_ELF}")
        add_test(NAME TargetPagerTestStaging COMMAND ${pager_test_staging_command})
        set_tests_properties(TargetPagerTestStaging PROPERTIES LABELS static)
    endif()
    # Each component image's segment is staged through one Frame and the pager's window: one larger
    # than its platform's limit fails here, not when the root service loads it.
    set(component_elves "")
    foreach(component_elf IN ITEMS "${ASTERIA_COMPONENT_ELF}" "${ASTERIA_DIRECTORY_ELF}" "${ASTERIA_CLIENT_ELF}"
            "${ASTERIA_INTERPRETER_ELF}" "${ASTERIA_USB_ELF}" "${ASTERIA_DISPLAY_ELF}" "${ASTERIA_SESSION_PROBE_ELF}" "${ASTERIA_COMPOSITOR_ELF}" "${ASTERIA_PATTERN_ELF}" "${ASTERIA_INPUT_SEAT_ELF}" "${ASTERIA_AUTH_ELF}" "${ASTERIA_LOGIN_ELF}" "${ASTERIA_TOOLKIT_ELF}")
        if(component_elf)
            list(APPEND component_elves "${component_elf}")
        endif()
    endforeach()
    if(component_elves)
        add_test(NAME TargetComponentSegments
            COMMAND "${Python3_EXECUTABLE}" "${ASTERIA_ROOT}/Tools/Build/CheckImageStaging.py" ${component_elves}
                --header "${ASTERIA_ROOT}/Source/User/Pager/ComponentImage.hpp"
                --segment-limit "${ASTERIA_TARGET}ComponentSegmentPages")
        set_tests_properties(TargetComponentSegments PROPERTIES LABELS static)
    endif()
endfunction()

# Writes AsteriaVerify.json, the artifact manifest read by
# Tools/Verify/VerifyTarget.py, and registers the verification run.
function(asteria_add_target_verification)
    set(firmware_artifacts
        "    \"firmware-login:kernel_elf\": \"${ASTERIA_FIRMWARE_LOGIN_KERNEL}\",\n"
        "    \"firmware-login:loader_elf\": \"${ASTERIA_FIRMWARE_LOGIN_LOADER}\",\n"
        "    \"firmware-login:efi\": \"${ASTERIA_FIRMWARE_LOGIN_EFI}\",\n"
        "    \"firmware-login:user_elf\": \"${ASTERIA_FIRMWARE_LOGIN_USER_ELF}\",\n"
        "    \"firmware-login:esp\": \"${ASTERIA_FIRMWARE_LOGIN_ESP}\",\n"
        "    \"firmware:kernel_elf\": \"${ASTERIA_FIRMWARE_KERNEL}\",\n"
        "    \"firmware:loader_elf\": \"${ASTERIA_FIRMWARE_LOADER}\",\n"
        "    \"firmware:efi\": \"${ASTERIA_FIRMWARE_EFI}\",\n"
        "    \"firmware:user_elf\": \"${ASTERIA_FIRMWARE_USER_ELF}\",\n"
        "    \"firmware:esp\": \"${ASTERIA_FIRMWARE_ESP}\",\n"
        "    \"firmware-test:kernel_elf\": \"${ASTERIA_FIRMWARE_TEST_KERNEL}\",\n"
        "    \"firmware-test:loader_elf\": \"${ASTERIA_FIRMWARE_TEST_LOADER}\",\n"
        "    \"firmware-test:efi\": \"${ASTERIA_FIRMWARE_TEST_EFI}\",\n"
        "    \"firmware-test:user_elf\": \"${ASTERIA_FIRMWARE_TEST_USER_ELF}\",\n"
        "    \"firmware-test:esp\": \"${ASTERIA_FIRMWARE_TEST_ESP}\"")
    if(ASTERIA_TARGET STREQUAL "Ia64")
        list(APPEND firmware_artifacts
            ",\n    \"firmware:residue_proof\": \"${ASTERIA_FIRMWARE_RESIDUE_PROOF}\"")
    endif()
    # The boot pager's image and the component it embeds, which both
    # firmwares carry.
    if(ASTERIA_PAGER_ELF)
        list(APPEND firmware_artifacts ",\n    \"firmware:pager_elf\": \"${ASTERIA_PAGER_ELF}\"")
    endif()
    if(ASTERIA_PAGER_TEST_ELF)
        list(APPEND firmware_artifacts ",\n    \"firmware-test:pager_elf\": \"${ASTERIA_PAGER_TEST_ELF}\"")
    endif()
    if(ASTERIA_IA64_TEST_SPINNER_ELF)
        list(APPEND firmware_artifacts ",\n    \"firmware-test:spinner_elf\": \"${ASTERIA_IA64_TEST_SPINNER_ELF}\"")
    endif()
    if(ASTERIA_AMD64_TEST_SMP_ELF)
        list(APPEND firmware_artifacts ",\n    \"firmware-test:smp_elf\": \"${ASTERIA_AMD64_TEST_SMP_ELF}\"")
    endif()
    if(ASTERIA_COMPONENT_ELF)
        list(APPEND firmware_artifacts ",\n    \"firmware:component_elf\": \"${ASTERIA_COMPONENT_ELF}\"")
    endif()
    if(ASTERIA_DIRECTORY_ELF)
        list(APPEND firmware_artifacts ",\n    \"firmware:directory_elf\": \"${ASTERIA_DIRECTORY_ELF}\"")
    endif()
    if(ASTERIA_CLIENT_ELF)
        list(APPEND firmware_artifacts ",\n    \"firmware:client_elf\": \"${ASTERIA_CLIENT_ELF}\"")
    endif()
    if(ASTERIA_INTERPRETER_ELF)
        list(APPEND firmware_artifacts ",\n    \"firmware:interpreter_elf\": \"${ASTERIA_INTERPRETER_ELF}\"")
    endif()
    if(ASTERIA_COMPOSITOR_FAULT_ELF)
        list(APPEND firmware_artifacts ",\n    \"firmware-test:compositor_fault_elf\": \"${ASTERIA_COMPOSITOR_FAULT_ELF}\"")
    endif()
    if(ASTERIA_DISPLAY_CONSOLE_ELF)
        list(APPEND firmware_artifacts ",\n    \"firmware-test:display_console_elf\": \"${ASTERIA_DISPLAY_CONSOLE_ELF}\"")
    endif()
    if(ASTERIA_DISPLAY_ELF)
        list(APPEND firmware_artifacts ",\n    \"firmware:display_elf\": \"${ASTERIA_DISPLAY_ELF}\"")
    endif()
    if(ASTERIA_COMPOSITOR_ELF)
        list(APPEND firmware_artifacts ",\n    \"firmware-test:compositor_elf\": \"${ASTERIA_COMPOSITOR_ELF}\"")
        list(APPEND firmware_artifacts ",\n    \"firmware-test:pattern_elf\": \"${ASTERIA_PATTERN_ELF}\"")
        list(APPEND firmware_artifacts ",\n    \"firmware-test:input_seat_elf\": \"${ASTERIA_INPUT_SEAT_ELF}\"")
    endif()
    if(ASTERIA_SESSION_PROBE_ELF)
        list(APPEND firmware_artifacts ",\n    \"firmware-test:session_probe_elf\": \"${ASTERIA_SESSION_PROBE_ELF}\"")
    endif()
    if(ASTERIA_USB_ELF)
        list(APPEND firmware_artifacts ",\n    \"firmware:usb_elf\": \"${ASTERIA_USB_ELF}\"")
    endif()
    string(CONCAT firmware_artifacts ${firmware_artifacts})
    if(ASTERIA_COMPILE_CHECKS)
        set(compile_checks true)
    else()
        set(compile_checks false)
    endif()
    string(CONCAT manifest
        "{\n"
        "  \"format\": 2,\n"
        "  \"isa\": \"${ASTERIA_TARGET}\",\n"
        "  \"root\": \"${ASTERIA_ROOT}\",\n"
        "  \"compile_checks\": ${compile_checks},\n"
        "  \"stack_budget_bytes\": ${ASTERIA_TARGET_STACK_BUDGET_BYTES},\n"
        "  \"tools\": {\n"
        "    \"cxx\": \"${CMAKE_CXX_COMPILER}\",\n"
        "    \"ld\": \"${ASTERIA_TARGET_LD}\",\n"
        "    \"objcopy\": \"${ASTERIA_TARGET_OBJCOPY}\",\n"
        "    \"objdump\": \"${ASTERIA_TARGET_OBJDUMP}\",\n"
        "    \"nm\": \"${ASTERIA_TARGET_NM}\",\n"
        "    \"readelf\": \"${ASTERIA_TARGET_READELF}\"\n"
        "  },\n"
        "  \"configuration\": {\n"
        "    \"image_base\": \"${ASTERIA_FIRMWARE_IMAGE_BASE}\",\n"
        "    \"ia64_native_preemption_probe\": \"${ASTERIA_IA64_NATIVE_PREEMPTION_PROBE}\",\n"
        "    \"ia64_synthetic_user_probe\": \"${ASTERIA_IA64_SYNTHETIC_USER_PROBE}\",\n"
        "    \"ia64_stop_probe\": \"${ASTERIA_IA64_STOP_PROBE}\",\n"
        "    \"amd64_native_preemption_probe\": \"${ASTERIA_AMD64_NATIVE_PREEMPTION_PROBE}\",\n"
        "    \"amd64_x2apic_handoff_probe\": \"$<BOOL:${ASTERIA_AMD64_X2APIC_HANDOFF_PROBE}>\",\n"
        "    \"amd64_stop_probe\": \"${ASTERIA_AMD64_STOP_PROBE}\"\n"
        "  },\n"
        "  \"artifacts\": {\n"
        "${ASTERIA_COMPILE_CHECK_ARTIFACTS_JSON}"
        "${firmware_artifacts}\n"
        "  },\n"
        "  \"objects\": {\n"
        "${ASTERIA_COMPILE_CHECK_OBJECTS_JSON}"
        "    \"firmware\": \"$<TARGET_OBJECTS:AsteriaKernelObjects>\",\n"
        "    \"firmware-user\": \"$<TARGET_OBJECTS:AsteriaUserObjects>\",\n"
        "    \"firmware-test\": \"$<TARGET_OBJECTS:AsteriaKernelObjects>\",\n"
        "    \"firmware-test-user\": \"${ASTERIA_FIRMWARE_TEST_USER_OBJECTS}\"\n"
        "  }\n"
        "}\n")
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/AsteriaVerify.json" CONTENT "${manifest}")
    set(required firmware firmware-test)
    if(ASTERIA_COMPILE_CHECKS)
        list(APPEND required O0 O2)
    endif()
    list(JOIN required "," required)
    add_test(NAME TargetVerification
        COMMAND "${Python3_EXECUTABLE}" "${ASTERIA_ROOT}/Tools/Verify/VerifyTarget.py"
            --build "${CMAKE_BINARY_DIR}" --require-variants ${required})
    set_tests_properties(TargetVerification PROPERTIES LABELS static)
endfunction()

# Compile-check configurations: every target-compilable production source at
# O0 and at O2 with the compile-check flags, plus the target fixtures and the
# closure links whose properties Tools/Verify checks. These builds differ from
# the firmware build in code model and optimization and exist only to verify
# that all code, including sources no image links yet, is valid target code.
function(asteria_add_compile_checks)
    string(TOUPPER "${ASTERIA_TARGET}" isa)
    set(nucleus ${ASTERIA_${isa}_KERNEL_SOURCES})
    list(REMOVE_ITEM nucleus Boot/KernelNoteHeader.S)
    foreach(source IN LISTS ASTERIA_UNLINKED_SOURCES)
        if(NOT source MATCHES "^(Executive/|User/Libc/)")
            list(APPEND nucleus "${source}")
        endif()
    endforeach()
    list(APPEND nucleus ${ASTERIA_${isa}_UNLINKED_SOURCES})
    set(executive
        Executive/ObjectDirectory/ObjectDirectoryProtocol.cpp
        Executive/ObjectDirectory/ObjectDirectoryCore.cpp
        Executive/ObjectDirectory/ObjectDirectoryServiceCore.cpp
        User/System/SystemCallClient.cpp)
    set(base_flags
        -ffreestanding -nostdinc -nostdinc++ -fno-exceptions -fno-rtti -fno-threadsafe-statics
        -fno-use-cxa-atexit -fno-common -fno-builtin -fno-stack-protector -fno-unwind-tables
        -fno-asynchronous-unwind-tables ${ASTERIA_TARGET_WARNING_FLAGS})
    if(ASTERIA_TARGET STREQUAL "Ia64")
        set(target_flags -mno-sdata -fno-jump-tables -fno-pic -mtune=itanium2 -mfixed-range=f32-f127
            -Wa,--fatal-warnings)
        set(unit_flags -fcallgraph-info=su -fstack-usage)
    else()
        set(target_flags -mno-red-zone -mgeneral-regs-only -fno-jump-tables -fno-pic -fno-pie -mcmodel=kernel)
        set(unit_flags -fstack-usage)
    endif()

    set(check_outputs)
    set(stack_objects)
    set(artifacts_json)
    set(objects_json)
    foreach(optimization IN ITEMS O0 O2)
        set(output "${CMAKE_BINARY_DIR}/CompileCheck/${optimization}")
        file(MAKE_DIRECTORY "${output}")

        # One object library per compilation policy. UNITS groups are the
        # translation units whose stack usage is budgeted; PLAIN groups are
        # fixtures and contracts that only have to compile.
        set(groups Nucleus Executive RuntimeMemory TimerFixtures Abi Libc)
        set(Nucleus_policy UNITS)
        set(Nucleus_sources ${nucleus})
        set(Executive_policy UNITS)
        set(Executive_sources ${executive})
        set(RuntimeMemory_policy UNITS)
        set(RuntimeMemory_sources Runtime/Memory.cpp)
        set(TimerFixtures_policy UNITS)
        set(TimerFixtures_sources
            ../Tests/Kernel/Time/TimerNotificationDeliveryTests.cpp
            ../Tests/Kernel/Time/DeferredTimerWorkerTests.cpp)
        # Libc is an application library with its own standard memory symbols. Compile it independently of
# the kernel closure and with the user FP ABI.
        set(Libc_policy UNITS)
        set(Libc_sources ${ASTERIA_LIBC_SOURCES})
        set(libc_target_flags ${target_flags})
        list(REMOVE_ITEM libc_target_flags -mgeneral-regs-only -mcmodel=kernel -mfixed-range=f32-f127)
        set(Abi_policy PLAIN)
        set(Abi_sources ../Tests/Target/AbiContractTests.cpp)
        if(ASTERIA_TARGET STREQUAL "Ia64")
            list(APPEND groups ProbeBoot SalFixtures)
            set(ProbeBoot_policy UNITS)
            set(ProbeBoot_sources Kernel/Boot/KernelEntry.cpp)
            set(SalFixtures_policy PLAIN)
            set(SalFixtures_sources
                ../Tests/Platform/Ia64/Interrupt/Ia64SalBootRendezvousHandlerTests.cpp
                ../Tests/Platform/Ia64/Firmware/SalBootRendezvousRegistrationTests.cpp)
        endif()
        foreach(name IN LISTS groups)
            set(policy ${${name}_policy})
            set(sources ${${name}_sources})
            set(library AsteriaCompileCheck${name}${optimization})
            list(TRANSFORM sources PREPEND "${ASTERIA_ROOT}/Source/")
            list(TRANSFORM sources REPLACE "/Source/\\.\\./" "/")
            add_library(${library} OBJECT ${sources})
            target_include_directories(${library} PRIVATE "${ASTERIA_ROOT}/Source")
            target_compile_definitions(${library} PRIVATE ASTERIA_TARGET_${isa}=1)
            if(name STREQUAL "Libc")
                asteria_target_language_options(${library} ${base_flags} ${libc_target_flags} -${optimization})
            else()
                asteria_target_language_options(${library} ${base_flags} ${target_flags} -${optimization})
            endif()
            if(policy STREQUAL "UNITS")
                target_compile_options(${library} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:${unit_flags}>")
                list(APPEND stack_objects "$<TARGET_OBJECTS:${library}>")
            endif()
            if(name STREQUAL "ProbeBoot")
                target_compile_definitions(${library} PRIVATE
                    ASTERIA_IA64_NATIVE_PREEMPTION_PROBE=1 ASTERIA_IA64_SYNTHETIC_USER_PROBE=1)
            endif()
        endforeach()

        # The user image at the same optimization level. The AMD64 nucleus
        # and IA-64 compile closures keep it separate for user-image checks.
        set(user_library AsteriaCompileCheckUser${optimization})
        set(user_sources ${ASTERIA_${isa}_USER_SOURCES})
        list(TRANSFORM user_sources PREPEND "${ASTERIA_ROOT}/Source/")
        add_library(${user_library} OBJECT ${user_sources})
        target_include_directories(${user_library} PRIVATE "${ASTERIA_ROOT}/Source")
        target_compile_definitions(${user_library} PRIVATE ${ASTERIA_USER_DEFINITIONS})
        set(user_flags ${ASTERIA_USER_COMPILE_FLAGS})
        list(REMOVE_ITEM user_flags -O2)
        asteria_target_language_options(${user_library} ${user_flags} -${optimization})
        list(APPEND stack_objects "$<TARGET_OBJECTS:${user_library}>")
        # Pure common user libraries, compiled with the user ABI but never linked
        # into an image. Both optimization levels participate in stack checks.
        set(unlinked_user_library AsteriaCompileCheckUnlinkedUser${optimization})
        set(unlinked_user_sources ${ASTERIA_UNLINKED_USER_SOURCES})
        list(TRANSFORM unlinked_user_sources PREPEND "${ASTERIA_ROOT}/Source/")
        add_library(${unlinked_user_library} OBJECT ${unlinked_user_sources})
        target_include_directories(${unlinked_user_library} PRIVATE "${ASTERIA_ROOT}/Source")
        target_compile_definitions(${unlinked_user_library} PRIVATE ${ASTERIA_USER_DEFINITIONS})
        asteria_target_language_options(${unlinked_user_library} ${user_flags} -${optimization})
        list(APPEND stack_objects "$<TARGET_OBJECTS:${unlinked_user_library}>")
        # Owners must obey the same 512-byte stack gate at O0 and O2.
        set(session_library AsteriaCompileCheckSession${optimization})
        set(session_sources ${ASTERIA_${isa}_COMPOSITOR_SOURCES})
        list(TRANSFORM session_sources PREPEND "${ASTERIA_ROOT}/Source/")
        list(APPEND session_sources "${ASTERIA_ROOT}/Tests/Guest/PatternClient.cpp")
        add_library(${session_library} OBJECT ${session_sources})
        target_include_directories(${session_library} PRIVATE "${ASTERIA_ROOT}/Source" "${ASTERIA_ROOT}/Tests")
        target_compile_definitions(${session_library} PRIVATE ${ASTERIA_USER_DEFINITIONS})
        asteria_target_language_options(${session_library} ${user_flags} -${optimization})
        list(APPEND stack_objects "$<TARGET_OBJECTS:${session_library}>")
        set(user_elf "${output}/MinimalUserImage.elf")
        asteria_link_user_image("${user_elf}" "${output}/MinimalUserImage.map"
            FIXED_ADDRESS_REASON "Bootstrap image staged once; shared executable segments applies to components"
            OBJECTS $<TARGET_OBJECTS:${user_library}> TARGETS ${user_library})
        list(APPEND check_outputs "${user_elf}")

        # The boot pager image at the same optimization level.
        set(pager_library AsteriaCompileCheckPager${optimization})
        set(pager_sources ${ASTERIA_${isa}_PAGER_SOURCES})
        list(TRANSFORM pager_sources PREPEND "${ASTERIA_ROOT}/Source/")
        add_library(${pager_library} OBJECT ${pager_sources})
        target_include_directories(${pager_library} PRIVATE "${ASTERIA_ROOT}/Source")
        target_compile_definitions(${pager_library} PRIVATE ${ASTERIA_USER_DEFINITIONS})
        asteria_target_language_options(${pager_library} ${user_flags} -${optimization})
        list(APPEND stack_objects "$<TARGET_OBJECTS:${pager_library}>")
        set(pager_elf "${output}/PagerImage.elf")
        asteria_link_user_image("${pager_elf}" "${output}/PagerImage.map"
            FIXED_ADDRESS_REASON "Bootstrap image staged once; shared executable segments applies to components"
            SCRIPT "${ASTERIA_ROOT}/Source/User/Platform/${ASTERIA_TARGET}/Pager/${ASTERIA_TARGET}PagerImage.lds"
            OBJECTS $<TARGET_OBJECTS:${pager_library}> TARGETS ${pager_library})
        list(APPEND check_outputs "${pager_elf}")

        file(GLOB scenario_sources CONFIGURE_DEPENDS "${ASTERIA_ROOT}/Tests/Guest/Pager/*.cpp")
        list(FILTER scenario_sources EXCLUDE REGEX "PagerTestImage[.]cpp$")
        list(APPEND scenario_sources "${ASTERIA_ROOT}/Tests/Guest/Pager/${ASTERIA_TARGET}PagerTestImage.cpp")
        if(EXISTS "${ASTERIA_ROOT}/Tests/Guest/Pager/${ASTERIA_TARGET}FaultScenarios.S")
            list(APPEND scenario_sources "${ASTERIA_ROOT}/Tests/Guest/Pager/${ASTERIA_TARGET}FaultScenarios.S")
        endif()
        if(ASTERIA_IA64_TEST_SPINNER_BLOB)
            list(APPEND scenario_sources "${ASTERIA_IA64_TEST_SPINNER_BLOB}")
        endif()
        set(scenario_library AsteriaCompileCheckPagerTest${optimization})
        add_library(${scenario_library} OBJECT ${scenario_sources})
        target_include_directories(${scenario_library} PRIVATE "${ASTERIA_ROOT}/Source" "${ASTERIA_ROOT}/Tests")
        target_compile_definitions(${scenario_library} PRIVATE ${ASTERIA_USER_DEFINITIONS})
        asteria_target_language_options(${scenario_library} ${user_flags} -${optimization})
        list(APPEND stack_objects "$<TARGET_OBJECTS:${scenario_library}>")

        # The spinner image, where the ISA has one.
        set(spinner_elf "")
        if(ASTERIA_${isa}_SPINNER_SOURCES)
            set(spinner_library AsteriaCompileCheckSpinner${optimization})
            set(spinner_sources ${ASTERIA_${isa}_SPINNER_SOURCES})
            list(TRANSFORM spinner_sources PREPEND "${ASTERIA_ROOT}/Source/")
            add_library(${spinner_library} OBJECT ${spinner_sources})
            target_include_directories(${spinner_library} PRIVATE "${ASTERIA_ROOT}/Source")
            target_compile_definitions(${spinner_library} PRIVATE ${ASTERIA_USER_DEFINITIONS})
            asteria_target_language_options(${spinner_library} ${user_flags} -${optimization})
            list(APPEND stack_objects "$<TARGET_OBJECTS:${spinner_library}>")
            set(spinner_elf "${output}/SpinnerImage.elf")
            asteria_link_user_image("${spinner_elf}" "${output}/SpinnerImage.map"
            FIXED_ADDRESS_REASON "Bootstrap image staged once; shared executable segments applies to components"
                SCRIPT "${ASTERIA_ROOT}/Source/User/Platform/${ASTERIA_TARGET}/Spinner/${ASTERIA_TARGET}SpinnerImage.lds"
                OBJECTS $<TARGET_OBJECTS:${spinner_library}> TARGETS ${spinner_library})
            list(APPEND check_outputs "${spinner_elf}")
        endif()

        set(nucleus_objects "$<TARGET_OBJECTS:AsteriaCompileCheckNucleus${optimization}>")
        set(nucleus_core "${output}/NucleusCore.o")
        add_custom_command(OUTPUT "${nucleus_core}"
            COMMAND "${ASTERIA_TARGET_LD}" -r ${nucleus_objects} -o "${nucleus_core}"
            DEPENDS AsteriaCompileCheckNucleus${optimization} ${nucleus_objects}
            COMMAND_EXPAND_LISTS VERBATIM)
        set(closures "${nucleus_core}")
        foreach(fixture IN ITEMS TimerNotificationDelivery DeferredTimerWorker)
            set(closure "${output}/${fixture}TestClosure.o")
            set(fixture_object
                "$<FILTER:$<TARGET_OBJECTS:AsteriaCompileCheckTimerFixtures${optimization}>,INCLUDE,/${fixture}Tests[.]cpp>")
            add_custom_command(OUTPUT "${closure}"
                COMMAND "${ASTERIA_TARGET_LD}" -r "${nucleus_core}" ${fixture_object} -o "${closure}"
                DEPENDS "${nucleus_core}" AsteriaCompileCheckTimerFixtures${optimization}
                    $<TARGET_OBJECTS:AsteriaCompileCheckTimerFixtures${optimization}>
                COMMAND_EXPAND_LISTS VERBATIM)
            list(APPEND closures "${closure}")
        endforeach()
        set(executive_production "${output}/ExecutiveProduction.o")
        set(executive_core "${output}/ExecutiveCore.o")
        add_custom_command(OUTPUT "${executive_production}"
            COMMAND "${ASTERIA_TARGET_LD}" -r $<TARGET_OBJECTS:AsteriaCompileCheckExecutive${optimization}>
                -o "${executive_production}"
            DEPENDS AsteriaCompileCheckExecutive${optimization}
                $<TARGET_OBJECTS:AsteriaCompileCheckExecutive${optimization}>
            COMMAND_EXPAND_LISTS VERBATIM)
        add_custom_command(OUTPUT "${executive_core}"
            COMMAND "${ASTERIA_TARGET_LD}" -r "${executive_production}"
                $<TARGET_OBJECTS:AsteriaCompileCheckRuntimeMemory${optimization}> -o "${executive_core}"
            DEPENDS "${executive_production}" AsteriaCompileCheckRuntimeMemory${optimization}
                $<TARGET_OBJECTS:AsteriaCompileCheckRuntimeMemory${optimization}>
            COMMAND_EXPAND_LISTS VERBATIM)
        list(APPEND closures "${executive_production}" "${executive_core}")

        set(closure_image "${output}/NucleusCore.elf")
        if(ASTERIA_TARGET STREQUAL "Ia64")
            # Every closure lets __gp follow the read-only GOT (.rodata).
            set(image_script "${ASTERIA_ROOT}/Source/Platform/Ia64/Uefi/Application.lds")
            set(image_arguments
                --defsym AsteriaConfiguredImageBase=0x4000000000000000
                -T "${image_script}")
        else()
            set(image_script)
            set(image_arguments -Ttext 0xffffffff80000000)
        endif()
        add_custom_command(OUTPUT "${closure_image}"
            COMMAND "${ASTERIA_TARGET_LD}" -nostdlib -static --no-undefined --fatal-warnings
                --build-id=none -e 0 ${image_arguments}
                --defsym=AsteriaKernelTextStart=0 --defsym=AsteriaKernelTextEnd=0
                -Map "${output}/NucleusCore.map"
                "${nucleus_core}" -o "${closure_image}"
            DEPENDS "${nucleus_core}" ${image_script} VERBATIM)
        list(APPEND check_outputs ${closures} "${closure_image}")

        set(prefix "    \"${optimization}")
        string(APPEND artifacts_json
            "${prefix}:nucleus_o\": \"${nucleus_core}\",\n"
            "${prefix}:nucleus_elf\": \"${closure_image}\",\n"
            "${prefix}:nucleus_map\": \"${output}/NucleusCore.map\",\n"
            "${prefix}:timer_notification_closure\": \"${output}/TimerNotificationDeliveryTestClosure.o\",\n"
            "${prefix}:deferred_timer_closure\": \"${output}/DeferredTimerWorkerTestClosure.o\",\n"
            "${prefix}:executive_production\": \"${executive_production}\",\n"
            "${prefix}:executive_core\": \"${executive_core}\",\n"
            "${prefix}:user_elf\": \"${user_elf}\",\n"
            "${prefix}:pager_elf\": \"${pager_elf}\",\n")
        set(fixture_objects
            "$<TARGET_OBJECTS:AsteriaCompileCheckTimerFixtures${optimization}>;$<TARGET_OBJECTS:AsteriaCompileCheckAbi${optimization}>")
        if(ASTERIA_TARGET STREQUAL "Ia64")
            string(APPEND fixture_objects ";$<TARGET_OBJECTS:AsteriaCompileCheckSalFixtures${optimization}>")
            string(APPEND objects_json
                "${prefix}-probe\": \"$<TARGET_OBJECTS:AsteriaCompileCheckProbeBoot${optimization}>\",\n")
        endif()
        string(APPEND objects_json
            "${prefix}\": \"$<TARGET_OBJECTS:AsteriaCompileCheckNucleus${optimization}>\",\n"
            "${prefix}-executive\": \"$<TARGET_OBJECTS:AsteriaCompileCheckExecutive${optimization}>\",\n"
            "${prefix}-runtime\": \"$<TARGET_OBJECTS:AsteriaCompileCheckRuntimeMemory${optimization}>\",\n"
            "${prefix}-libc\": \"$<TARGET_OBJECTS:AsteriaCompileCheckLibc${optimization}>\",\n"
            "${prefix}-fixtures\": \"${fixture_objects}\",\n"
            "${prefix}-user\": \"$<TARGET_OBJECTS:${user_library}>\",\n")
    endforeach()
    add_custom_target(AsteriaCompileChecks ALL DEPENDS ${check_outputs})
    set(ASTERIA_COMPILE_CHECK_ARTIFACTS_JSON "${artifacts_json}" PARENT_SCOPE)
    set(ASTERIA_COMPILE_CHECK_OBJECTS_JSON "${objects_json}" PARENT_SCOPE)

    add_test(NAME CompileCheckStackUsage
        COMMAND "${Python3_EXECUTABLE}" "${ASTERIA_ROOT}/Tools/Build/CheckStackUsage.py"
            --limit ${ASTERIA_TARGET_STACK_BUDGET_BYTES} ${stack_objects})
    set_tests_properties(CompileCheckStackUsage PROPERTIES LABELS static)

    # The ABI contract must refuse ambiguous or missing target selections, and
    # must do so through its #error guards rather than an unrelated failure.
    if(ASTERIA_TARGET STREQUAL "Amd64")
        set(contract "${ASTERIA_ROOT}/Tests/Target/AbiContractTests.cpp")
        set(index 0)
        foreach(definitions IN ITEMS
                "-DASTERIA_TARGET_IA64=1;-DASTERIA_TARGET_AMD64=1"
                "-DASTERIA_TARGET_AMD64=0"
                "")
            math(EXPR index "${index} + 1")
            add_test(NAME RejectedTargetDefinition${index}
                COMMAND "${CMAKE_CXX_COMPILER}" -std=c++23 ${base_flags} ${target_flags} -I "${ASTERIA_ROOT}/Source"
                    ${definitions} -fsyntax-only "${contract}")
            set_tests_properties(RejectedTargetDefinition${index} PROPERTIES
                LABELS static PASS_REGULAR_EXPRESSION "#error")
        endforeach()
    endif()
endfunction()
