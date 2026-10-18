# SPDX-License-Identifier: Apache-2.0

# Production sources compiled for the hosted verification build. Every nucleus source is compiled
# freestanding with the target language flags; only the sanitizer or analyzer profile differs. Tests
# link the static libraries below, so the linker selects exactly the objects a test needs and no
# per-test source list exists.

file(GLOB_RECURSE asteria_all_sources CONFIGURE_DEPENDS
    RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
# Layout units only generate headers for target assembly (see
# asteria_add_asm_layout); hand-written assembly itself lives in .S files.
list(FILTER asteria_all_sources EXCLUDE REGEX "^Platform/[^/]+/Layout/")
file(GLOB_RECURSE asteria_all_headers CONFIGURE_DEPENDS
    RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp")

# Sources that only have a target implementation: boot runtimes, firmware
# diagnostics, and the embedded user images. The Analyze profile still
# analyzes them with their target configuration macros.
set(ASTERIA_TARGET_ONLY_AMD64_SOURCES
    Platform/Amd64/Diagnostics/Amd64Stop.cpp
    Platform/Amd64/Boot/Amd64BootRuntime.cpp
    Platform/Amd64/Device/Amd64BootstrapDevices.cpp
    Platform/Amd64/Device/Amd64PortIo.cpp
    Platform/Amd64/Device/Amd64VtdHardware.cpp
    Platform/Amd64/Firmware/BootEntropySources.cpp
    Platform/Amd64/Firmware/Diagnostic.cpp
    Platform/Amd64/Firmware/FirmwarePortIo.cpp
    Platform/Amd64/Scheduler/Amd64ApplicationProcessorRuntime.cpp
    Platform/Amd64/Scheduler/Amd64NativePreemptionProbe.cpp
    Platform/Amd64/Scheduler/Amd64ProcessorRunLoop.cpp
    Platform/Amd64/System/Amd64WaitRuntime.cpp
    Platform/Amd64/System/Amd64PagerRuntime.cpp
    Platform/Amd64/System/Amd64SpinnerRuntime.cpp
    Platform/Amd64/System/Amd64UserThreadTable.cpp
    Platform/Amd64/Time/Amd64ApicTimer.cpp
    User/Platform/Amd64/Runtime/Amd64BssZeroInitializationProbe.cpp
    User/Platform/Amd64/Runtime/Amd64MinimalUserImage.cpp
    User/Platform/Amd64/Runtime/Amd64MinimalUserImageRuntime.cpp
    User/Platform/Amd64/System/Amd64SystemCallTransport.cpp
    User/Platform/Amd64/Pager/Amd64PagerImage.cpp
    User/Platform/Amd64/Spinner/Amd64SpinnerImage.cpp
    # The `usb` module's entry defines the component entry's main as the
    # component image's does; only one image links either.
    User/Platform/Amd64/Component/Amd64UsbHostImage.cpp
    User/Platform/Amd64/Component/Amd64DisplayImage.cpp
    User/Platform/Amd64/Component/Amd64SessionImage.cpp
    User/Platform/Amd64/Component/Amd64AuthImage.cpp
    User/Platform/Amd64/Component/Amd64LoginImage.cpp
    User/Platform/Amd64/Component/Amd64ToolkitImage.cpp
)
set(ASTERIA_TARGET_ONLY_IA64_SOURCES
    Platform/Ia64/Firmware/BootEntropySources.cpp
    User/Platform/Ia64/Component/Ia64XhciDriver.cpp
    User/Platform/Ia64/Component/Ia64UsbHostImage.cpp
    User/Platform/Ia64/Component/Ia64DisplayImage.cpp
    User/Platform/Ia64/Component/Ia64SessionImage.cpp
    User/Platform/Ia64/Component/Ia64AuthImage.cpp
    User/Platform/Ia64/Component/Ia64LoginImage.cpp
    User/Platform/Ia64/Component/Ia64ToolkitImage.cpp
    User/Platform/Ia64/Runtime/Ia64MinimalUserImage.cpp
    User/Platform/Ia64/Pager/Ia64PagerImage.cpp
    User/Platform/Ia64/System/Ia64SystemCallTransport.cpp
)
set(ASTERIA_TARGET_ONLY_COMMON_SOURCES
    Platform/FatalTrap.cpp
    Runtime/CxxAbi.cpp
    Runtime/Placement.cpp
)

# The AMD64 UEFI application is verified on the host with its target
# configuration because its hosted fixture reproduces the AMD64 UEFI ABI. The
# shared AHCI capture is verified the same way: IA-64 compiles the same code
# without the AMD64 calling-convention attribute.
set(ASTERIA_AMD64_UEFI_SOURCES
    Boot/Uefi/GraphicsModeChoice.cpp
    Boot/Uefi/BootConsole.cpp
    Boot/Uefi/AcpiReportVolume.cpp
    Boot/Uefi/FatBootReport.cpp
    Boot/Uefi/FatReportVolume.cpp
    Boot/Uefi/AmlBootReport.cpp
    Boot/Uefi/BootFileLoader.cpp
    Boot/Uefi/AcpiTableDump.cpp
    Boot/Uefi/BootReport.cpp
    Boot/Uefi/TopologyReport.cpp
    Boot/Uefi/PeripheralReport.cpp
    Boot/Uefi/RootBridgeReport.cpp
    Boot/Uefi/BootApplication.cpp
    Boot/Uefi/LoaderDiagnostic.cpp
    Boot/Uefi/KernelLoader.cpp
    Kernel/Boot/KernelEntry.cpp
    Runtime/Placement.cpp
    Boot/Uefi/BootEntropyGathering.cpp
    Boot/Uefi/BootTransition.cpp
    Platform/Firmware/AhciPciResourceCapture.cpp
)

# The AMD64 cooperative-switch path runs natively on an x86-64 host. It is
# compiled with the target code-generation constraints and without sanitizer
# runtimes, which cannot describe a kernel stack replacement.
set(ASTERIA_AMD64_NATIVE_SOURCES
    Runtime/Placement.cpp
    Kernel/Device/DeviceRegion.cpp
    Kernel/Device/IommuDomain.cpp
    Kernel/Ipc/Endpoint.cpp
    Kernel/Ipc/IpcDeliveryCodec.cpp
    Kernel/Ipc/MessageCodec.cpp
    Kernel/Memory/AddressSpace.cpp
    Kernel/Memory/FaultFrameMapping.cpp
    Kernel/Memory/FaultToken.cpp
    Kernel/Memory/Frame.cpp
    Kernel/Memory/InitialMappingPlan.cpp
    Kernel/Memory/MemoryObject.cpp
    Kernel/Memory/PinnedFrameMapping.cpp
    Kernel/Memory/ProcessorAddressSpaceController.cpp
    Kernel/Object/CapabilitySpace.cpp
    Kernel/Object/CapabilityTransfer.cpp
    Kernel/Object/ObjectPin.cpp
    Kernel/Object/ObjectRegistry.cpp
    Kernel/Object/RevocationRegistry.cpp
    Kernel/Object/RootCapabilityIssuer.cpp
    Kernel/Protection/ProtectionDomain.cpp
    Kernel/Resource/ResourceDomain.cpp
    Kernel/Scheduler/DispatchCoordinator.cpp
    Kernel/Scheduler/ProcessorRunController.cpp
    Kernel/Scheduler/ProcessorScheduler.cpp
    Kernel/Scheduler/ReadyQueue.cpp
    Kernel/Scheduler/SchedulingContext.cpp
    Kernel/Scheduler/Thread.cpp
    Kernel/System/IpcSystemCallDispatcher.cpp
    Kernel/System/SystemCallDispatcher.cpp
    Kernel/System/SystemCallIngress.cpp
    Kernel/Time/ProcessorDeadlineController.cpp
    Kernel/Time/Timer.cpp
    Kernel/Time/TimerQueue.cpp
    Platform/Amd64/Scheduler/Amd64KernelContext.cpp
    Platform/Amd64/Scheduler/Amd64KernelContext.S
    Platform/Amd64/Scheduler/Amd64ProcessorRunLoop.cpp
    Platform/Synchronization/AtomicReferenceCount.cpp
    Platform/Synchronization/SpinLock.cpp
    Platform/Synchronization/ProcessorContext.cpp
)

# AMD64 platform models whose hosted build replaces privileged instructions
# with stand-ins (their target code is selected by ASTERIA_TARGET_AMD64, which
# this library does not define). They are kept out of the nucleus library so
# the stand-ins cannot satisfy any other test.
set(ASTERIA_AMD64_HOSTED_MODEL_SOURCES
    Platform/Amd64/Device/Amd64MmioWindow.cpp
    Platform/Amd64/Executable/Amd64InitialUserImage.cpp
    Platform/Amd64/Interrupt/Amd64ApplicationProcessorStartup.cpp
    Platform/Amd64/Interrupt/Amd64UserEntry.cpp
)

# Hosted discovery includes the pure user libraries. Target source lists in
# CMakeLists.txt provide the corresponding compile and stack checks for both ISAs.
set(asteria_host_sources ${asteria_all_sources})
list(REMOVE_ITEM asteria_host_sources
    ${ASTERIA_TARGET_ONLY_AMD64_SOURCES}
    ${ASTERIA_TARGET_ONLY_IA64_SOURCES}
    ${ASTERIA_TARGET_ONLY_COMMON_SOURCES}
    ${ASTERIA_AMD64_HOSTED_MODEL_SOURCES}
    Boot/Uefi/BootApplication.cpp
    Kernel/Boot/KernelEntry.cpp
    Platform/Firmware/AhciPciResourceCapture.cpp
)

# Static compositor construction uses the project's placement adapter in hosted owner tests as well
# as in its freestanding module.
list(APPEND asteria_host_sources Runtime/Placement.cpp)

# Per-source requirements that hold wherever the file is compiled.
set_source_files_properties(
    Runtime/Memory.cpp
    Platform/Ia64/Firmware/SalSystemTableCapture.cpp
    Boot/Uefi/BootApplication.cpp
    Kernel/Boot/KernelEntry.cpp
    PROPERTIES COMPILE_OPTIONS -fno-builtin)
# These owners reference IA-64 leaf routines from functions that hosted tests
# never call; section-granular garbage collection drops them at link time.
set_source_files_properties(
    Platform/Ia64/Executable/Ia64InitialUserImageMappingOwner.cpp
    Platform/Ia64/Executable/Ia64InitialUserImageMemoryOwner.cpp
    Platform/Ia64/Diagnostics/Ia64Stop.cpp
    Platform/Ia64/Memory/Ia64BspRuntimeMemory.cpp
    Platform/Ia64/Memory/Ia64RuntimeAddressSpaceOwner.cpp
    PROPERTIES COMPILE_OPTIONS -ffunction-sections)
add_library(AsteriaHostNucleus STATIC ${asteria_host_sources}
    Platform/Amd64/Firmware/ProcessorInventory.S)
target_include_directories(AsteriaHostNucleus PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
asteria_apply_host_flags(AsteriaHostNucleus FREESTANDING)

add_library(AsteriaHostAmd64Models STATIC ${ASTERIA_AMD64_HOSTED_MODEL_SOURCES})
target_include_directories(AsteriaHostAmd64Models PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
asteria_apply_host_flags(AsteriaHostAmd64Models FREESTANDING)

add_library(AsteriaHostAmd64Uefi STATIC ${ASTERIA_AMD64_UEFI_SOURCES})
target_include_directories(AsteriaHostAmd64Uefi PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(AsteriaHostAmd64Uefi PUBLIC ASTERIA_TARGET_AMD64=1)
asteria_apply_host_flags(AsteriaHostAmd64Uefi FREESTANDING)

# Amd64KernelContext.S includes a generated layout header. Target trees
# generate it with the target compiler; here the host compiler does, since the
# x86-64 System V layout is the target's.
asteria_add_asm_layout_units(AsteriaHostAsmLayout Amd64
    COMPILER "${CMAKE_CXX_COMPILER}"
    UNITS "${CMAKE_CURRENT_SOURCE_DIR}/Platform/Amd64/Layout/Amd64KernelContextAsmLayout.cpp"
    FLAGS ${ASTERIA_FREESTANDING_FLAGS} -DASTERIA_TARGET_AMD64=1)

foreach(optimization IN ITEMS O0 O2)
    set(library AsteriaHostAmd64Native${optimization})
    add_library(${library} STATIC ${ASTERIA_AMD64_NATIVE_SOURCES})
    target_include_directories(${library} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_include_directories(${library} PRIVATE "$<$<COMPILE_LANGUAGE:ASM>:${CMAKE_BINARY_DIR}>")
    add_dependencies(${library} AsteriaHostAsmLayout)
    target_compile_definitions(${library} PUBLIC ASTERIA_TARGET_AMD64=1)
    target_compile_options(${library} PUBLIC -mgeneral-regs-only -mno-red-zone)
    asteria_apply_native_flags(${library} FREESTANDING ${optimization})
endforeach()

# Every header must compile on its own with the nucleus language flags.
set(asteria_header_units)
foreach(header IN LISTS asteria_all_headers)
    string(MAKE_C_IDENTIFIER "${header}" unit_name)
    set(unit "${CMAKE_CURRENT_BINARY_DIR}/HeaderUnits/${unit_name}.cpp")
    file(CONFIGURE OUTPUT "${unit}" CONTENT "#include \"${header}\"\n")
    list(APPEND asteria_header_units "${unit}")
endforeach()
add_library(AsteriaHeaderSelfContainment OBJECT ${asteria_header_units})
target_include_directories(AsteriaHeaderSelfContainment PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
asteria_apply_host_flags(AsteriaHeaderSelfContainment FREESTANDING)

if(ASTERIA_PROFILE STREQUAL "Analyze")
    # Target-only code paths are analyzed with their configuration macros so
    # neither architecture can hide diagnostics in preprocessing.
    add_library(AsteriaAnalyzeAmd64 OBJECT
        ${ASTERIA_TARGET_ONLY_AMD64_SOURCES}
        ${ASTERIA_AMD64_UEFI_SOURCES}
        Platform/Amd64/Memory/InitialAddressSpacePreparation.cpp
        Platform/Amd64/Memory/Amd64TlbShootdownInterruptBinding.cpp
        Platform/Amd64/Memory/Amd64X2ApicTlbShootdownNotifier.cpp
        Platform/Amd64/Scheduler/Amd64KernelContext.cpp
        Platform/Amd64/Time/Amd64LocalTimer.cpp
        Platform/Amd64/Device/Amd64MmioWindow.cpp
        Platform/Amd64/Interrupt/Amd64UserEntry.cpp
        Platform/Amd64/Interrupt/Amd64ApplicationProcessorStartup.cpp)
    target_include_directories(AsteriaAnalyzeAmd64 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_definitions(AsteriaAnalyzeAmd64 PRIVATE ASTERIA_TARGET_AMD64=1)
    asteria_apply_host_flags(AsteriaAnalyzeAmd64 FREESTANDING)

    # BootApplication.cpp is compiled again under a second object name.
    set(ia64_boot_unit "${CMAKE_CURRENT_BINARY_DIR}/AnalyzeUnits/BootApplicationIa64.cpp")
    file(CONFIGURE OUTPUT "${ia64_boot_unit}"
        CONTENT "#include \"${CMAKE_CURRENT_SOURCE_DIR}/Boot/Uefi/BootApplication.cpp\"\n")
    set(ia64_kernel_unit "${CMAKE_CURRENT_BINARY_DIR}/AnalyzeUnits/KernelEntryIa64.cpp")
    file(CONFIGURE OUTPUT "${ia64_kernel_unit}"
        CONTENT "#include \"${CMAKE_CURRENT_SOURCE_DIR}/Kernel/Boot/KernelEntry.cpp\"\n")
    add_library(AsteriaAnalyzeIa64 OBJECT
        ${ASTERIA_TARGET_ONLY_IA64_SOURCES}
        "${ia64_boot_unit}"
        "${ia64_kernel_unit}"
        Platform/Ia64/Device/Ia64PhysicalMemoryAccess.cpp
        Platform/Ia64/Firmware/Diagnostic.cpp
        Platform/Ia64/Interrupt/Ia64ExternalPreemptionEntry.cpp
        Platform/Ia64/Interrupt/Ia64ExternalPreemptionFrame.cpp
        Platform/Ia64/Scheduler/Ia64BspKernelRuntime.cpp
        Platform/Ia64/Scheduler/Ia64KernelContext.cpp
        Platform/Ia64/Scheduler/Ia64ProcessorRunLoop.cpp
        Platform/Ia64/System/Ia64SystemCallEntry.cpp
        Platform/Ia64/System/Ia64SystemCallIngress.cpp
        Platform/Ia64/System/Ia64SystemCallWaitContinuation.cpp
        Platform/Ia64/System/Ia64PagerRuntime.cpp
        Platform/Ia64/System/Ia64UserThreadTable.cpp
        User/Platform/Ia64/Runtime/Ia64BssZeroInitializationProbe.cpp
        User/Platform/Ia64/Runtime/Ia64MinimalUserImageRuntime.cpp)
    target_include_directories(AsteriaAnalyzeIa64 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_definitions(AsteriaAnalyzeIa64 PRIVATE ASTERIA_TARGET_IA64=1)
    set_source_files_properties("${ia64_boot_unit}" PROPERTIES COMPILE_OPTIONS -fno-builtin)
    asteria_apply_host_flags(AsteriaAnalyzeIa64 FREESTANDING)

    add_library(AsteriaAnalyzeCommon OBJECT ${ASTERIA_TARGET_ONLY_COMMON_SOURCES})
    target_include_directories(AsteriaAnalyzeCommon PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    asteria_apply_host_flags(AsteriaAnalyzeCommon FREESTANDING)
endif()
