// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_BOOT_UEFI_UEFI_TYPES_HPP_INCLUDED
#define ASTERIA_BOOT_UEFI_UEFI_TYPES_HPP_INCLUDED

#include "Foundation/Types.hpp"

#if defined(ASTERIA_TARGET_AMD64)
#define ASTERIA_UEFI_API __attribute__((ms_abi))
#else
#define ASTERIA_UEFI_API
#endif

namespace Asteria::Boot::Uefi
{
using Boolean = Foundation::UInt8;
using Handle = void*;
using Status = Foundation::UInt64;

constexpr Status Success{0U};
constexpr Status ErrorBit{Foundation::UInt64{1U} << 63U};
constexpr Status InvalidParameter{ErrorBit | 2U};
constexpr Status Unsupported{ErrorBit | 3U};
constexpr Status BufferTooSmall{ErrorBit | 5U};
constexpr Status DeviceError{ErrorBit | 7U};
constexpr Status NotFound{ErrorBit | 14U};
constexpr Foundation::UInt32 MemoryDescriptorVersion{1U};

constexpr Foundation::UInt64 SystemTableSignature{0x5453'5953'2049'4249ULL};
constexpr Foundation::UInt64 BootServicesSignature{0x5652'4553'544F'4F42ULL};
constexpr Foundation::UInt64 RuntimeServicesSignature{0x5652'4553'544E'5552ULL};

struct alignas(8) Guid
{
    Foundation::UInt32 Data1;
    Foundation::UInt16 Data2;
    Foundation::UInt16 Data3;
    Foundation::UInt8 Data4[8U];
};

struct TableHeader
{
    Foundation::UInt64 Signature;
    Foundation::UInt32 Revision;
    Foundation::UInt32 HeaderSize;
    Foundation::UInt32 Crc32;
    Foundation::UInt32 Reserved;
};

struct SimpleTextOutputProtocol;
struct GraphicsOutputProtocol;
struct SystemTable;

using TextReset = Status(ASTERIA_UEFI_API*)(SimpleTextOutputProtocol*, Boolean);
using TextOutputString = Status(ASTERIA_UEFI_API*)(SimpleTextOutputProtocol*, Foundation::UInt16*);
using TextTestString = Status(ASTERIA_UEFI_API*)(SimpleTextOutputProtocol*, Foundation::UInt16*);
using TextQueryMode = Status(ASTERIA_UEFI_API*)(SimpleTextOutputProtocol*, Foundation::Size, Foundation::Size*,
                                                Foundation::Size*);
using TextSetMode = Status(ASTERIA_UEFI_API*)(SimpleTextOutputProtocol*, Foundation::Size);
using TextSetAttribute = Status(ASTERIA_UEFI_API*)(SimpleTextOutputProtocol*, Foundation::Size);
using TextClearScreen = Status(ASTERIA_UEFI_API*)(SimpleTextOutputProtocol*);
using TextSetCursorPosition = Status(ASTERIA_UEFI_API*)(SimpleTextOutputProtocol*, Foundation::Size, Foundation::Size);
using TextEnableCursor = Status(ASTERIA_UEFI_API*)(SimpleTextOutputProtocol*, Boolean);

struct SimpleTextOutputMode
{
    Foundation::Int32 MaximumMode;
    Foundation::Int32 Mode;
    Foundation::Int32 Attribute;
    Foundation::Int32 CursorColumn;
    Foundation::Int32 CursorRow;
    Boolean CursorVisible;
};

struct SimpleTextOutputProtocol
{
    TextReset Reset;
    TextOutputString OutputString;
    TextTestString TestString;
    TextQueryMode QueryMode;
    TextSetMode SetMode;
    TextSetAttribute SetAttribute;
    TextClearScreen ClearScreen;
    TextSetCursorPosition SetCursorPosition;
    TextEnableCursor EnableCursor;
    SimpleTextOutputMode* Mode;
};

using GetMemoryMap = Status(ASTERIA_UEFI_API*)(Foundation::Size*, void*, Foundation::Size*, Foundation::Size*,
                                               Foundation::UInt32*);
enum class AllocateType : Foundation::UInt32
{
    AnyPages,
    MaximumAddress,
    Address,
};
enum class MemoryType : Foundation::UInt32
{
    Reserved,
    LoaderCode,
    LoaderData,
    BootServicesCode,
    BootServicesData,
    RuntimeServicesCode,
    RuntimeServicesData,
    Conventional,
    Unusable,
    AcpiReclaimable,
    AcpiNonVolatile,
    MemoryMappedIo,
    MemoryMappedIoPort,
    PalCode,
    Persistent,
    Unaccepted,
};
using AllocatePages = Status(ASTERIA_UEFI_API*)(AllocateType, MemoryType, Foundation::Size, Foundation::UInt64*);
using FreePages = Status(ASTERIA_UEFI_API*)(Foundation::UInt64, Foundation::Size);
using FreePool = Status(ASTERIA_UEFI_API*)(void*);
using HandleProtocol = Status(ASTERIA_UEFI_API*)(Handle, Guid*, void**);
using Stall = Status(ASTERIA_UEFI_API*)(Foundation::Size);
enum class LocateSearchType : Foundation::UInt32
{
    AllHandles,
    ByRegisterNotify,
    ByProtocol,
};
using LocateHandleBuffer = Status(ASTERIA_UEFI_API*)(LocateSearchType, Guid*, void*, Foundation::Size*, Handle**);
using LocateProtocol = Status(ASTERIA_UEFI_API*)(Guid*, void*, void**);
using CalculateCrc32 = Status(ASTERIA_UEFI_API*)(void*, Foundation::Size, Foundation::UInt32*);
using ExitBootServices = Status(ASTERIA_UEFI_API*)(Handle, Foundation::Size);
using SetWatchdogTimer = Status(ASTERIA_UEFI_API*)(Foundation::Size, Foundation::UInt64, Foundation::Size,
                                                   Foundation::UInt16*);
using GetNextMonotonicCount = Status(ASTERIA_UEFI_API*)(Foundation::UInt64*);

struct BootServices
{
    TableHeader Header;
    void* RaiseTpl;
    void* RestoreTpl;
    AllocatePages AllocatePagesFunction;
    FreePages FreePagesFunction;
    GetMemoryMap GetMemoryMapFunction;
    void* AllocatePool;
    FreePool FreePoolFunction;
    void* CreateEvent;
    void* SetTimer;
    void* WaitForEvent;
    void* SignalEvent;
    void* CloseEvent;
    void* CheckEvent;
    void* InstallProtocolInterface;
    void* ReinstallProtocolInterface;
    void* UninstallProtocolInterface;
    HandleProtocol HandleProtocolFunction;
    void* Reserved;
    void* RegisterProtocolNotify;
    void* LocateHandle;
    void* LocateDevicePath;
    void* InstallConfigurationTable;
    void* LoadImage;
    void* StartImage;
    void* Exit;
    void* UnloadImage;
    ExitBootServices ExitBootServicesFunction;
    GetNextMonotonicCount GetNextMonotonicCountFunction;
    Stall StallFunction;
    SetWatchdogTimer SetWatchdogTimerFunction;
    void* ConnectController;
    void* DisconnectController;
    void* OpenProtocol;
    void* CloseProtocol;
    void* OpenProtocolInformation;
    void* ProtocolsPerHandle;
    LocateHandleBuffer LocateHandleBufferFunction;
    LocateProtocol LocateProtocolFunction;
    void* InstallMultipleProtocolInterfaces;
    void* UninstallMultipleProtocolInterfaces;
    CalculateCrc32 CalculateCrc32Function;
    void* CopyMem;
    void* SetMem;
    void* CreateEventEx;
};

/// EFI_TIME.
struct Time
{
    Foundation::UInt16 Year;
    Foundation::UInt8 Month;
    Foundation::UInt8 Day;
    Foundation::UInt8 Hour;
    Foundation::UInt8 Minute;
    Foundation::UInt8 Second;
    Foundation::UInt8 Pad1;
    Foundation::UInt32 Nanosecond;
    Foundation::Int16 TimeZone;
    Foundation::UInt8 Daylight;
    Foundation::UInt8 Pad2;
};

using GetTime = Status(ASTERIA_UEFI_API*)(Time*, void*);

/// The EFI 1.10 prefix of EFI_RUNTIME_SERVICES; the application calls only
/// GetTime, and only before ExitBootServices.
struct RuntimeServices
{
    TableHeader Header;
    GetTime GetTimeFunction;
    void* SetTime;
    void* GetWakeupTime;
    void* SetWakeupTime;
    void* SetVirtualAddressMap;
    void* ConvertPointer;
    void* GetVariable;
    void* GetNextVariableName;
    void* SetVariable;
    void* GetNextHighMonotonicCount;
    void* ResetSystem;
};

struct RngProtocol;

using RngGetInfo = Status(ASTERIA_UEFI_API*)(RngProtocol*, Foundation::Size*, Guid*);
using RngGetRng = Status(ASTERIA_UEFI_API*)(RngProtocol*, Guid*, Foundation::Size, Foundation::UInt8*);

/// EFI_RNG_PROTOCOL.
struct RngProtocol
{
    RngGetInfo GetInfo;
    RngGetRng GetRng;
};

struct ConfigurationTable
{
    Guid VendorGuid;
    void* VendorTable;
};

struct SystemTable
{
    TableHeader Header;
    Foundation::UInt16* FirmwareVendor;
    Foundation::UInt32 FirmwareRevision;
    Handle ConsoleInHandle;
    void* ConsoleIn;
    Handle ConsoleOutHandle;
    SimpleTextOutputProtocol* ConsoleOut;
    Handle StandardErrorHandle;
    SimpleTextOutputProtocol* StandardError;
    void* RuntimeServices;
    BootServices* BootServicesTable;
    Foundation::Size NumberOfTableEntries;
    ConfigurationTable* ConfigurationTables;
};

using ImageEntryPoint = Status(ASTERIA_UEFI_API*)(Handle, SystemTable*);

struct LoadedImageProtocol
{
    Foundation::UInt32 Revision;
    Foundation::UInt32 Reserved0;
    Handle ParentHandle;
    SystemTable* ImageSystemTable;
    Handle DeviceHandle;
    void* FilePath;
    void* Reserved;
    Foundation::UInt32 LoadOptionsSize;
    Foundation::UInt32 Reserved1;
    void* LoadOptions;
    void* ImageBase;
    Foundation::UInt64 ImageSize;
    Foundation::UInt32 ImageCodeType;
    Foundation::UInt32 ImageDataType;
    void* Unload;
};

enum class GraphicsOutputBltOperation : Foundation::UInt32
{
    VideoFill,
    VideoToBuffer,
    BufferToVideo,
    VideoToVideo,
};

struct GraphicsOutputBltPixel
{
    Foundation::UInt8 Blue;
    Foundation::UInt8 Green;
    Foundation::UInt8 Red;
    Foundation::UInt8 Reserved;
};

struct GraphicsOutputModeInformation
{
    Foundation::UInt32 Version;
    Foundation::UInt32 HorizontalResolution;
    Foundation::UInt32 VerticalResolution;
    Foundation::UInt32 PixelFormat;
    Foundation::UInt32 RedMask;
    Foundation::UInt32 GreenMask;
    Foundation::UInt32 BlueMask;
    Foundation::UInt32 ReservedMask;
    Foundation::UInt32 PixelsPerScanLine;
};

struct GraphicsOutputProtocolMode
{
    Foundation::UInt32 MaximumMode;
    Foundation::UInt32 Mode;
    GraphicsOutputModeInformation* Information;
    Foundation::Size InformationSize;
    Foundation::UInt64 FrameBufferBase;
    Foundation::Size FrameBufferSize;
};

using GraphicsQueryMode = Status(ASTERIA_UEFI_API*)(GraphicsOutputProtocol*, Foundation::UInt32, Foundation::Size*,
                                                    GraphicsOutputModeInformation**);
using GraphicsSetMode = Status(ASTERIA_UEFI_API*)(GraphicsOutputProtocol*, Foundation::UInt32);
using GraphicsBlt = Status(ASTERIA_UEFI_API*)(GraphicsOutputProtocol*, GraphicsOutputBltPixel*,
                                              GraphicsOutputBltOperation, Foundation::Size, Foundation::Size,
                                              Foundation::Size, Foundation::Size, Foundation::Size, Foundation::Size,
                                              Foundation::Size);

// EFI_EDID_ACTIVE_PROTOCOL and EFI_EDID_DISCOVERED_PROTOCOL share this ABI.
struct EdidProtocol
{
    Foundation::UInt32 SizeOfEdid;
    Foundation::UInt8* Edid;
};

struct GraphicsOutputProtocol
{
    GraphicsQueryMode QueryMode;
    GraphicsSetMode SetMode;
    GraphicsBlt Blt;
    GraphicsOutputProtocolMode* Mode;
};

static_assert(sizeof(void*) == sizeof(void (*)()));
static_assert(sizeof(Guid) == 16U);
static_assert(alignof(Guid) == 8U);
static_assert(sizeof(TableHeader) == 24U);
static_assert(__builtin_offsetof(TableHeader, Crc32) == 16U);
static_assert(__builtin_offsetof(SimpleTextOutputProtocol, OutputString) == 8U);
static_assert(sizeof(AllocateType) == 4U);
static_assert(sizeof(MemoryType) == 4U);
static_assert(sizeof(LocateSearchType) == 4U);
static_assert(sizeof(BootServices) == 376U);
static_assert(__builtin_offsetof(BootServices, AllocatePagesFunction) == 40U);
static_assert(__builtin_offsetof(BootServices, FreePagesFunction) == 48U);
static_assert(__builtin_offsetof(BootServices, GetMemoryMapFunction) == 56U);
static_assert(__builtin_offsetof(BootServices, FreePoolFunction) == 72U);
static_assert(__builtin_offsetof(BootServices, HandleProtocolFunction) == 152U);
static_assert(__builtin_offsetof(BootServices, ExitBootServicesFunction) == 232U);
static_assert(__builtin_offsetof(BootServices, GetNextMonotonicCountFunction) == 240U);
static_assert(__builtin_offsetof(BootServices, StallFunction) == 248U);
static_assert(__builtin_offsetof(BootServices, SetWatchdogTimerFunction) == 256U);
static_assert(__builtin_offsetof(BootServices, LocateHandleBufferFunction) == 312U);
static_assert(__builtin_offsetof(BootServices, LocateProtocolFunction) == 320U);
static_assert(__builtin_offsetof(BootServices, CalculateCrc32Function) == 344U);
constexpr Foundation::Size MinimumConsumedBootServicesSize{__builtin_offsetof(BootServices, LocateProtocolFunction) +
                                                           sizeof(LocateProtocol)};
static_assert(MinimumConsumedBootServicesSize == 328U);
static_assert(sizeof(SystemTable) == 120U);
static_assert(__builtin_offsetof(SystemTable, ConsoleOut) == 64U);
static_assert(__builtin_offsetof(SystemTable, BootServicesTable) == 96U);
static_assert(__builtin_offsetof(SystemTable, ConfigurationTables) == 112U);
static_assert(sizeof(Time) == 16U);
static_assert(__builtin_offsetof(Time, Nanosecond) == 8U);
static_assert(__builtin_offsetof(RuntimeServices, GetTimeFunction) == 24U);
static_assert(sizeof(RuntimeServices) == 112U);
static_assert(sizeof(RngProtocol) == 16U);
static_assert(sizeof(ConfigurationTable) == 24U);
static_assert(alignof(ConfigurationTable) == 8U);
static_assert(sizeof(LoadedImageProtocol) == 96U);
static_assert(sizeof(GraphicsOutputModeInformation) == 36U);
static_assert(sizeof(GraphicsOutputProtocolMode) == 40U);
static_assert(sizeof(GraphicsOutputProtocol) == 32U);
static_assert(sizeof(EdidProtocol) == 16U);
static_assert(__builtin_offsetof(EdidProtocol, Edid) == 8U);
} // namespace Asteria::Boot::Uefi

#endif // ASTERIA_BOOT_UEFI_UEFI_TYPES_HPP_INCLUDED
