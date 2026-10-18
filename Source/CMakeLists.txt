# SPDX-License-Identifier: Apache-2.0

# Production sources. The kernel image of each ISA is composed from explicit, ordered source lists
# next to that ISA's platform code. The hosted build compiles the same sources freestanding for the
# test suite; target builds link the UEFI applications.

include(User/Crypto/SharedSha256.cmake)

include(Platform/Ia64/KernelImage.cmake)
include(Platform/Amd64/KernelImage.cmake)

foreach(isa IN ITEMS Ia64 Amd64)
    string(TOUPPER "${isa}" upper)
    set(ASTERIA_${upper}_AUTH_SOURCES
        Runtime/Memory.cpp Runtime/Placement.cpp
        User/System/SystemCallClient.cpp
        User/Platform/${isa}/System/${isa}SystemCallTransport.cpp
        User/Platform/${isa}/System/${isa}SystemCallTransport.S
        User/Platform/${isa}/Component/${isa}ComponentImage.S
        User/Platform/${isa}/Component/${isa}AuthImage.cpp
        User/Identity/AuthService.cpp User/Identity/SeatFrame.cpp
        User/Identity/Format.cpp User/Identity/AccountStore.cpp
        User/Identity/Authentication.cpp User/Identity/AttemptLimiter.cpp
        User/Crypto/Argon2.cpp User/Crypto/Hash.cpp User/Crypto/Sha256.cpp)
    set(ASTERIA_${upper}_LOGIN_SOURCES
        Runtime/Memory.cpp Runtime/Placement.cpp User/System/SystemCallClient.cpp
        User/Platform/${isa}/System/${isa}SystemCallTransport.cpp
        User/Platform/${isa}/System/${isa}SystemCallTransport.S
        User/Platform/${isa}/Component/${isa}ComponentImage.S
        User/Platform/${isa}/Component/${isa}LoginImage.cpp
        User/Identity/LoginService.cpp User/Identity/SeatFrame.cpp
        User/Identity/LoginRenderer.cpp User/Identity/TrustedScreen.cpp User/Identity/SecretEntry.cpp
        User/Identity/Format.cpp User/Identity/Authentication.cpp User/Identity/AccountStore.cpp
        User/Identity/AttemptLimiter.cpp User/Crypto/Argon2.cpp User/Crypto/Hash.cpp User/Crypto/Sha256.cpp
        User/Input/Keymap.cpp)
    list(APPEND ASTERIA_${upper}_PAGER_SOURCES Runtime/Placement.cpp User/Identity/SeatFrame.cpp User/Identity/TrustedScreen.cpp User/Pager/ProductLogin.cpp)
    list(APPEND ASTERIA_${upper}_DISPLAY_SOURCES User/Identity/SeatFrame.cpp User/Identity/TrustedPlane.cpp)
    if(isa STREQUAL "Ia64")
        list(APPEND ASTERIA_${upper}_AUTH_SOURCES Platform/Ia64/Runtime/CompilerArithmetic.cpp Platform/FatalTrap.cpp)
        list(APPEND ASTERIA_${upper}_LOGIN_SOURCES Platform/Ia64/Runtime/CompilerArithmetic.cpp Platform/FatalTrap.cpp)
    endif()
endforeach()

# The reusable application library uses only GCC's freestanding C headers.
set(ASTERIA_LIBC_SOURCES
    User/Libc/CompilerArithmetic.cpp
    User/Libc/LibcHooks.cpp
    User/Libc/Allocator.cpp
    User/Libc/Ctype.cpp
    User/Libc/String.cpp
    User/Libc/Error.cpp
    User/Libc/Integer.cpp
    User/Libc/Sort.cpp
    User/Libc/Float.cpp
    User/Libc/Formatter.cpp
)
if(NOT ASTERIA_TARGET STREQUAL "Host")
    execute_process(COMMAND "${CMAKE_CXX_COMPILER}" -print-file-name=include
        OUTPUT_VARIABLE ASTERIA_COMPILER_C_HEADERS OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY)
    set_source_files_properties(${ASTERIA_LIBC_SOURCES} PROPERTIES
        COMPILE_OPTIONS "-isystem;${ASTERIA_COMPILER_C_HEADERS}")
endif()

# Native status units and POSIX translation units, compiled with the user ABI on both ISAs.
set(ASTERIA_NATIVE_STATUS_SOURCES
    User/Runtime/GenericStatus.cpp
    User/Runtime/KernelStatus.cpp
    User/Runtime/MemoryStatus.cpp
    User/Runtime/TimeStatus.cpp
    User/Runtime/ConsoleStatus.cpp
    User/Runtime/FileStatus.cpp
    User/Runtime/DatagramStatus.cpp
    User/Runtime/IpcStatus.cpp
    User/Runtime/ProcessStatus.cpp
    User/Runtime/Status.cpp
    User/Runtime/ReplyStatus.cpp
    User/Runtime/StatusRegistry.cpp
)
set(ASTERIA_POSIX_STATUS_SOURCES
    User/Posix/Errno.cpp
    User/Posix/ErrnoGeneric.cpp
    User/Posix/ErrnoKernel.cpp
    User/Posix/ErrnoMemory.cpp
    User/Posix/ErrnoTime.cpp
    User/Posix/ErrnoConsole.cpp
    User/Posix/ErrnoFile.cpp
    User/Posix/ErrnoDatagram.cpp
    User/Posix/ErrnoIpc.cpp
    User/Posix/ErrnoProcess.cpp
)
if(NOT ASTERIA_TARGET STREQUAL "Host")
    set_source_files_properties(${ASTERIA_NATIVE_STATUS_SOURCES} ${ASTERIA_POSIX_STATUS_SOURCES}
        PROPERTIES COMPILE_OPTIONS "-isystem;${ASTERIA_COMPILER_C_HEADERS}")
endif()

# Hosted sources excluded from native image link lists.
set(ASTERIA_UNLINKED_SOURCES
    # OS-independent libc.
    ${ASTERIA_LIBC_SOURCES}
)
# User libraries compiled for both ISAs with the user ABI.
set(ASTERIA_UNLINKED_USER_SOURCES
    # Account/authentication libraries.
    User/Identity/Format.cpp
    User/Identity/AccountStore.cpp
    User/Identity/AttemptLimiter.cpp
    User/Identity/Authentication.cpp
    User/Identity/TrustedScreen.cpp
    User/Identity/SecretEntry.cpp
    User/Identity/SeatFrame.cpp
    User/Identity/LoginRenderer.cpp
    # Pure settings storage library; component wiring is separate.
    User/Settings/Database.cpp
    User/Settings/Format.cpp
    # Pure cryptographic primitives; native service wiring is separate.
    User/Crypto/Sha256.cpp
    User/Crypto/Hash.cpp
    User/Crypto/Symmetric.cpp
    User/Crypto/Curve.cpp
    User/Crypto/Argon2.cpp
    User/Crypto/Drbg.cpp
    ${ASTERIA_NATIVE_STATUS_SOURCES}
    ${ASTERIA_POSIX_STATUS_SOURCES}
    # Pure TZif and civil-time library.
    User/Time/Civil.cpp
    User/Time/Posix.cpp
    User/Time/TimeZone.cpp
    # Imaging, retained toolkit and session bindings.
    User/Imaging/Inflate.cpp
    User/Imaging/Png.cpp
    User/Imaging/Bmp.cpp
    User/Imaging/Ico.cpp
    User/Imaging/CursorSet.cpp
    User/Toolkit/Imaging.cpp
    User/Toolkit/Theme.cpp
    User/Toolkit/Graphics.cpp
    User/Toolkit/Arithmetic.cpp
    User/Toolkit/Application.cpp
    User/Toolkit/Control.cpp
    User/Toolkit/Form.cpp
    User/Toolkit/Controls.cpp
    User/Toolkit/TextBox.cpp
    User/Toolkit/Layout.cpp
    User/Toolkit/Extended.cpp
    User/Toolkit/Dialogs.cpp
    User/Toolkit/Demo.cpp
    User/Toolkit/SessionAdapter.cpp
    User/Toolkit/SessionComponent.cpp
    User/Session/ClientConnection.cpp
    # Font parsing, layout and rasterization.
    User/Font/Arithmetic.cpp
    User/Font/Font.cpp
    User/Font/Cff.cpp
    User/Font/Type2.cpp
    User/Font/Outline.cpp
    User/Font/LayoutTables.cpp
    User/Font/Layout.cpp
    User/Font/Raster.cpp
    User/Font/Cache.cpp
    User/Font/Draw.cpp
    # Pure shell core; launcher binding is separate.
    User/Shell/Text.cpp
    User/Shell/Tokenizer.cpp
    User/Shell/Parser.cpp
    User/Shell/Expansion.cpp
    User/Shell/Arithmetic.cpp
    User/Shell/Engine.cpp
    User/Shell/Builtins.cpp
    User/Input/HidBoot.cpp
    User/Input/HidRelativeMouse.cpp
    User/Input/UsbConfiguration.cpp
    User/Input/UsbDescriptors.cpp
    User/Input/UsbDevice.cpp
    User/Usb/XhciMemory.cpp
    User/Usb/XhciEvent.cpp
    User/Usb/Xhci.cpp
    User/Usb/Usb1.cpp
    User/Usb/OhciStatus.cpp
    User/Usb/Ohci.cpp
    User/Usb/UhciStatus.cpp
    User/Usb/Uhci.cpp
    User/Usb/EhciStatus.cpp
    User/Usb/Ehci.cpp
    User/Usb/UsbHub.cpp
    User/Usb/Bot.cpp
    User/Usb/Scsi.cpp
    User/Usb/MassStorage.cpp
    # Compositor protocol, scene, window policy, client state and address plan, plus the terminal.
    User/Input/Keymap.cpp
    User/Session/AddressPlan.cpp
    User/Session/ClientEvents.cpp
    User/Session/Decorations.cpp
    User/Session/InputV3Frame.cpp
    User/Session/SessionFaceFrame.cpp
    User/Session/InputOrder.cpp
    User/Component/SessionLiveness.cpp
    User/Session/UiText.cpp
    User/Session/UiTextDrawing.cpp
    User/Session/Scene.cpp
    User/Session/SessionFrame.cpp
    User/Session/WindowManager.cpp
    User/Terminal/Terminal.cpp
    User/Terminal/TerminalKeys.cpp
    User/Terminal/TerminalParser.cpp
    User/Terminal/TerminalRenderer.cpp
    User/Terminal/TerminalScreen.cpp
)
set(ASTERIA_IA64_UNLINKED_SOURCES
    # IA-64 has no I/O-port configuration mechanism; it writes ECAM.
    Platform/Device/PortPciConfigurationSpace.cpp
    Platform/Ia64/Memory/Ia64RuntimeTranslationCacheInserter.cpp
    Platform/Ia64/Memory/Ia64RuntimeTranslationCacheInserter.S
    # Only AMD64 keeps EHCIs an RMRR reserves: IA-64's VT-d refuses a DMAR with RMRRs.
    Inventory/EhciRmrrReport.cpp
    # IA-64's virtio-iommu backend enforces its reserved regions itself.
    Platform/Device/FirmwareDmaReservations.cpp
)
# Hosted user sources excluded from IA-64 embedded images: the spinner runs only
# in AMD64 test firmware, and IA-64 uses an xHCI driver stub.
set(ASTERIA_IA64_UNLINKED_USER_SOURCES
    User/Component/XhciDriver.cpp
    User/Component/XhciStorage.cpp
    User/Spinner/SpinnerProgram.cpp
)
set(ASTERIA_AMD64_UNLINKED_SOURCES
    # The IA-64 loader's debug-port tables; AMD64 keeps its SPCR/COM1 sinks.
    Boot/Acpi/DebugPortTables.cpp
    Platform/Ia64/Runtime/CompilerArithmetic.cpp
    Platform/Device/VirtioPciTransport.cpp
    Platform/Device/VirtioIommuDomainBackend.cpp
    Platform/Amd64/Memory/Amd64TlbShootdownInterruptBinding.cpp
    Platform/Amd64/Memory/Amd64TlbShootdownInterruptBinding.S
    Platform/Amd64/Time/Amd64LocalTimer.cpp
    Platform/Amd64/Time/Amd64LocalTimer.S
)

list(APPEND ASTERIA_IA64_UNLINKED_USER_SOURCES User/Platform/Ia64/Component/Ia64ToolkitImage.cpp)

# Every production source of an ISA must be assigned to its kernel image, its
# embedded user image, or an unlinked list, so a new file cannot silently be
# left out of the native build. Layout units (Platform/<Isa>/Layout) generate
# build-time headers for assembly and are not image code.
file(GLOB_RECURSE asteria_production_sources CONFIGURE_DEPENDS
    RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/*.S")
list(FILTER asteria_production_sources EXCLUDE REGEX "^Platform/[^/]+/Layout/")
foreach(isa IN ITEMS Ia64 Amd64)
    string(TOUPPER "${isa}" upper)
    if(isa STREQUAL "Ia64")
        set(foreign Amd64)
    else()
        set(foreign Ia64)
    endif()
    set(assigned
        ${ASTERIA_${upper}_KERNEL_SOURCES}
        ${ASTERIA_${upper}_USER_SOURCES}
        ${ASTERIA_${upper}_PAGER_SOURCES}
        ${ASTERIA_${upper}_SPINNER_SOURCES}
        ${ASTERIA_${upper}_COMPONENT_SOURCES}
        ${ASTERIA_${upper}_COMPONENT_SIZE_SOURCES}
        ${ASTERIA_${upper}_DIRECTORY_SOURCES}
        ${ASTERIA_${upper}_CLIENT_SOURCES}
        ${ASTERIA_${upper}_INTERPRETER_SOURCES}
        ${ASTERIA_${upper}_USB_SOURCES}
        ${ASTERIA_${upper}_DISPLAY_SOURCES}
        ${ASTERIA_${upper}_COMPOSITOR_SOURCES}
        ${ASTERIA_${upper}_AUTH_SOURCES}
        ${ASTERIA_${upper}_LOGIN_SOURCES}
        ${ASTERIA_UNLINKED_SOURCES}
        ${ASTERIA_UNLINKED_USER_SOURCES}
        ${ASTERIA_${upper}_UNLINKED_SOURCES}
        ${ASTERIA_${upper}_UNLINKED_USER_SOURCES})
    foreach(source IN LISTS asteria_production_sources)
        if(source MATCHES "(^|/)Platform/${foreign}/")
            continue()
        endif()
        if(NOT source IN_LIST assigned)
            message(FATAL_ERROR
                "Source/${source} is not assigned to the ${isa} kernel or user image "
                "or to an unlinked list in Source/CMakeLists.txt")
        endif()
    endforeach()
endforeach()

if(ASTERIA_TARGET STREQUAL "Host")
    include(Hosted.cmake)
else()
    string(TOUPPER "${ASTERIA_TARGET}" isa)
    asteria_add_asm_layout()
    asteria_add_firmware(
        KERNEL_SOURCES ${ASTERIA_${isa}_KERNEL_SOURCES}
        USER_SOURCES ${ASTERIA_${isa}_USER_SOURCES}
        PAGER_SOURCES ${ASTERIA_${isa}_PAGER_SOURCES}
        SPINNER_SOURCES ${ASTERIA_${isa}_SPINNER_SOURCES}
        COMPONENT_SOURCES ${ASTERIA_${isa}_COMPONENT_SOURCES}
        COMPONENT_SIZE_SOURCES ${ASTERIA_${isa}_COMPONENT_SIZE_SOURCES}
        DIRECTORY_SOURCES ${ASTERIA_${isa}_DIRECTORY_SOURCES}
        CLIENT_SOURCES ${ASTERIA_${isa}_CLIENT_SOURCES}
        INTERPRETER_SOURCES ${ASTERIA_${isa}_INTERPRETER_SOURCES}
        USB_SOURCES ${ASTERIA_${isa}_USB_SOURCES}
        DISPLAY_SOURCES ${ASTERIA_${isa}_DISPLAY_SOURCES}
        COMPOSITOR_SOURCES ${ASTERIA_${isa}_COMPOSITOR_SOURCES}
        AUTH_SOURCES ${ASTERIA_${isa}_AUTH_SOURCES}
        LOGIN_SOURCES ${ASTERIA_${isa}_LOGIN_SOURCES}
        USER_MAIN ${ASTERIA_${isa}_USER_MAIN_SOURCE})
    asteria_add_target_header_units()
    asteria_add_target_checks()
    if(ASTERIA_COMPILE_CHECKS)
        asteria_add_compile_checks()
    endif()
    asteria_add_target_verification()
endif()

# Check the cryptographic timing structure on both ISAs.
add_test(NAME CryptoStructure
    COMMAND "${Python3_EXECUTABLE}" "${ASTERIA_ROOT}/Tools/Verify/CheckCryptoStructure.py"
        --root "${ASTERIA_ROOT}")
set_tests_properties(CryptoStructure PROPERTIES LABELS static)
add_test(NAME CryptoStructureMutations
    COMMAND "${Python3_EXECUTABLE}" "${ASTERIA_ROOT}/Tests/User/Crypto/StructureTests.py")
set_tests_properties(CryptoStructureMutations PROPERTIES LABELS static)

add_test(NAME SourceTestSeparation
    COMMAND "${Python3_EXECUTABLE}" "${ASTERIA_TOOLS}/Build/CheckSeparation.py"
        --root "${ASTERIA_ROOT}")
set_tests_properties(SourceTestSeparation PROPERTIES LABELS "static")
