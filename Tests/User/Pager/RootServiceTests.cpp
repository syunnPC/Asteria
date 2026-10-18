// SPDX-License-Identifier: Apache-2.0
#include <cstring>
#include <memory>

#include "Abi/ComponentBootstrap.hpp"
#include "Abi/IpcCanonicalFrame.hpp"
#include "Abi/PagerBootstrap.hpp"
#include "User/Pager/PagerWords.hpp"
#include "User/Component/DisplayFrame.hpp"
#include "User/Loader/ExecutableFormats.hpp"
#include "Guest/Pager/TestRootService.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Foundation::Size;
using Foundation::UInt64;
using Foundation::UInt8;
using User::Pager::ComponentPlatform;
using User::Pager::PagerBootstrap;
using User::Pager::PagerProgram;
using User::Pager::PagerServeError;
using User::Pager::PagerServeOutcome;
using User::Pager::PagerThreadLaunch;
using RootService = User::Pager::TestRootService;
using User::System::SystemCallClient;
using User::System::SystemCallTransport;

constexpr UInt64 PageBytes{4096U};
constexpr UInt64 StackStart{0x5000'1000U};
constexpr UInt64 StackPages{2U};
constexpr UInt64 Entry{0x4000'0010U};
constexpr UInt64 PagerEndpoint{0x11U};
// Where an image's.bss lies (BssImage).
constexpr UInt64 BssStart{0x4000'4000U};
constexpr UInt64 WatchdogMicroseconds{2'000'000U};
// The events the kernel logs: a created or destroyed object's type.
constexpr UInt64 Created{0x100U};
constexpr UInt64 Destroyed{0x200U};
// The window a component of the account test maps its own memory into.
constexpr UInt64 WindowStart{0x7000'0000U};
// The selector of the grant's handle (Bootstrap).
constexpr UInt64 GrantSelector{0x99U};
constexpr UInt64 DomainFeatures{Abi::PagerManagementDomains | Abi::PagerManagementMapping};

void Store(UInt8* const bytes, const UInt64 value, const Size width) noexcept
{
    for (Size index{0U}; index < width; ++index)
    {
        bytes[index] = static_cast<UInt8>(value >> (index * 8U));
    }
}

[[nodiscard]] UInt64 LoadWord(const UInt8* const bytes) noexcept
{
    UInt64 value{0U};
    for (Size index{0U}; index < 8U; ++index)
    {
        value |= static_cast<UInt64>(bytes[index]) << (8U * index);
    }
    return value;
}

// A linked component: text (R+X, two pages) at 0x40000000 from file offset
// 0x1000, read-only data at 0x40002000 from 0x3000.
struct Image
{
    UInt8 Bytes[0x3100U]{};

    Image() noexcept
    {
        Bytes[0U] = 0x7FU;
        Bytes[1U] = 'E';
        Bytes[2U] = 'L';
        Bytes[3U] = 'F';
        Bytes[4U] = 2U;
        Bytes[5U] = 1U;
        Bytes[6U] = 1U;
        Store(Bytes + 16U, 2U, 2U);
        Store(Bytes + 18U, 62U, 2U);
        Store(Bytes + 20U, 1U, 4U);
        Store(Bytes + 24U, Entry, 8U);
        Store(Bytes + 32U, 64U, 8U);
        Store(Bytes + 54U, 56U, 2U);
        Store(Bytes + 56U, 2U, 2U);
        Segment(0U, 5U, 0x1000U, 0x4000'0000U, 0x1100U);
        Segment(1U, 4U, 0x3000U, 0x4000'2000U, 0x58U);
        Bytes[0x1000U] = 0xC3U;
        Bytes[0x3000U] = 0x5AU;
    }

    void Segment(const Size index, const UInt64 flags, const UInt64 offset, const UInt64 address,
                 const UInt64 bytes) noexcept
    {
        UInt8* const header = Bytes + 64U + (index * 56U);
        Store(header, 1U, 4U);
        Store(header + 4U, flags, 4U);
        Store(header + 8U, offset, 8U);
        Store(header + 16U, address, 8U);
        Store(header + 32U, bytes, 8U);
        Store(header + 40U, bytes, 8U);
    }
};

// An image with a third segment of two zero-filled pages, placed past the
// file's end as a linker may place it.
struct BssImage : Image
{
    BssImage() noexcept
    {
        Store(Bytes + 56U, 3U, 2U);
        Segment(2U, 6U, 0x9'0000U, BssStart, 0U);
        Store(Bytes + 64U + (2U * 56U) + 40U, 0x2000U, 8U);
    }
};

// The display module is separate even in this loader model. No scanout is
// configured here, so only the component image is instantiated.
struct ImageTable
{
    alignas(8) UInt8 Bytes[80U + 2U * sizeof(Image::Bytes)]{};
    explicit ImageTable(const Image& image) noexcept
    {
        Store(Bytes, 2U, 8U);
        constexpr const char* names[]{"component", "display"};
        for (Size i{0U}; i < 2U; ++i)
        {
            for (Size j{0U}; names[i][j] != '\0'; ++j)
            {
                Bytes[16U + i * 32U + j] = static_cast<UInt8>(names[i][j]);
            }
            const Size offset = 80U + i * sizeof(image.Bytes);
            Store(Bytes + 32U + i * 32U, offset, 8U);
            Store(Bytes + 40U + i * 32U, sizeof(image.Bytes), 8U);
            std::memcpy(Bytes + offset, image.Bytes, sizeof(image.Bytes));
        }
    }
};

// The pager's own window the root service stages each segment through.
alignas(4096) UInt8 g_window[User::Pager::Amd64ComponentSegmentPages * PageBytes]{};
Size g_flushes{0U};
UInt64 g_flushedBytes{0U};

void Flush(const UInt64 address, const UInt64 bytes) noexcept
{
    g_flushes += (address == reinterpret_cast<Foundation::UIntPointer>(g_window)) ? 1U : 0U;
    g_flushedBytes = bytes;
}

// What an incarnation's thread does once started: fault (the crash), run
// until the watchdog kills it (the hang), run into a fault as the watchdog
// fires (the hang the kill leaves to its fault), exit after its report, or
// get killed although it should crash.
enum class Script
{
    Crash,
    Hang,
    HangBlocked,
    Report,
    CrashKilled,
    // Creates in its own domain and reports.
    Account,
    // Drives the assignable device and reports.
    Driver,
    // The same through its registers only, on a kernel without an IOMMU
    DriverRegisters,
};

/// A kernel for the root service: the nth ObjectCreate answers handle
/// 0x1000 + n (an incarnation's twelve objects follow the report Endpoint's),
/// logs each create and destroy, and runs the current thread by its script.
class ComponentKernel final
{
public:
    static void Transport(void* const context, const Abi::SystemCallRequest* const request,
                          Abi::SystemCallResponse* const response) noexcept
    {
        static_cast<ComponentKernel*>(context)->Handle(*request, *response);
    }

    void Handle(const Abi::SystemCallRequest& request, Abi::SystemCallResponse& response) noexcept
    {
        response = Abi::SystemCallResponse{.Error = Abi::SystemCallError::None, .Value0 = 0U, .Value1 = 0U};
        switch (static_cast<Abi::SystemCallNumber>(request.Number))
        {
        case Abi::SystemCallNumber::ObjectCreate:
            if ((FailCreateType != 0U) && (request.Arguments[2U] == FailCreateType))
            {
                response.Error = Abi::SystemCallError::ResourceExhausted;
                return;
            }
            ++Handles;
            response.Value0 = 0x1000U + Handles;
            response.Value1 = 0xA000U + request.Arguments[2U];
            Log(Created + request.Arguments[2U]);
            Charge(request, response.Value0);
            if (request.Arguments[2U] == Abi::ObjectTypeThread)
            {
                std::memcpy(LastThread, reinterpret_cast<const UInt8*>(request.Arguments[3U]), sizeof(LastThread));
                Thread = response.Value0;
                ExitCode = 0U;
                FaultDelivered = false;
                ++Threads;
            }
            if (request.Arguments[2U] == Abi::ObjectTypeEndpoint)
            {
                Report = response.Value0;
            }
            return;
        case Abi::SystemCallNumber::ObjectDestroy:
            if ((request.Arguments[2U] == Abi::ObjectTypeMemoryObject) && (DestroyMemoryFailures != 0U))
            {
                --DestroyMemoryFailures;
                response.Error = Abi::SystemCallError::WouldBlock;
                return;
            }
            // A failed driver start block is private and pins its account
            // while the pager's staging window still maps it.
            if ((request.Arguments[2U] == Abi::ObjectTypeResourceDomain) && (AssignedFunction != 0U) &&
                (WindowMappedPages != 0U))
            {
                ++PinnedDrains;
                response.Error = Abi::SystemCallError::WouldBlock;
                return;
            }
            if ((request.Arguments[2U] == Abi::ObjectTypeResourceDomain) && (DrainBusy != 0U))
            {
                --DrainBusy;
                ++DrainPolls;
                response.Error = Abi::SystemCallError::WouldBlock;
                return;
            }
            if ((request.Arguments[2U] == Abi::ObjectTypeThread) && (ExitCode == 0U))
            {
                ++BusyPolls;
                response.Error = Abi::SystemCallError::WouldBlock;
                return;
            }
            response.Value0 = (request.Arguments[2U] == Abi::ObjectTypeThread) ? ExitCode : 0U;
            for (const UInt64 handle : SharedHandles)
            {
                SharedDestroyed += ((handle != 0U) && (request.Arguments[0U] == handle)) ? 1U : 0U;
            }
            Log(Destroyed + request.Arguments[2U]);
            return;
        case Abi::SystemCallNumber::WaitSetWait:
            ++Waits;
            TimedWaits += (request.Arguments[2U] == WatchdogMicroseconds) ? 1U : 0U;
            Wait(response);
            return;
        case Abi::SystemCallNumber::ThreadTerminate:
            ++Kills;
            KilledThread = request.Arguments[0U];
            response.Error = KillError;
            if (Current() == Script::HangBlocked)
            {
                PendingFault = true;
                response.Error = Abi::SystemCallError::WouldBlock;
            }
            else if (KillError == Abi::SystemCallError::None)
            {
                ExitCode = Abi::KilledExitCode;
            }
            return;
        case Abi::SystemCallNumber::IpcReceive:
            Receive(request, response);
            return;
        case Abi::SystemCallNumber::FaultResolve:
            ++Terminates;
            ExitCode = Abi::FaultTerminatedExitCode;
            response.Value0 = Abi::FaultTerminatedExitCode;
            return;
        case Abi::SystemCallNumber::CapabilityDerive:
            MapOnlyDerives = MapOnlyDerives && (request.Arguments[2U] == Abi::CapabilityRightMap) &&
                             (request.Arguments[3U] == ~UInt64{0U}) && (request.Arguments[4U] == 0U);
            response.Value0 = request.Arguments[0U] + 0x10000U;
            response.Value1 = request.Arguments[1U];
            if (Derives < 4U)
            {
                DerivedHandles[Derives] = response.Value0;
            }
            ++Derives;
            return;
        case Abi::SystemCallNumber::CapabilityGrant:
            ++Grants;
            response.Value0 = 0x5000U + Grants;
            response.Value1 = 0x6000U;
            GrantTarget = request.Arguments[2U];
            return;
        case Abi::SystemCallNumber::DebugWrite:
            if (std::memcmp(reinterpret_cast<const char*>(request.Arguments[2U]), "# shared image:", 15U) == 0)
            {
                ++SharedLines;
                return;
            }
            std::memset(Lines[LineCount % 4U], 0, sizeof(Lines[0U]));
            std::memcpy(Lines[LineCount % 4U], reinterpret_cast<const char*>(request.Arguments[2U]),
                        request.Arguments[3U] < sizeof(Lines[0U]) ? request.Arguments[3U] : sizeof(Lines[0U]) - 1U);
            ++LineCount;
            return;
        case Abi::SystemCallNumber::MemoryMap:
        {
            ++Maps;
            const auto* const block = reinterpret_cast<const UInt8*>(request.Arguments[3U]);
            const UInt64 address = LoadWord(block + Abi::FaultResolveVirtualStartOffset);
            if ((address == 0x4000'0000U) || (address == 0x4000'2000U))
            {
                const Size slot = (address == 0x4000'0000U) ? 0U : 2U;
                const UInt64 frame = LoadWord(block + Abi::FaultResolveFrameOffset);
                const UInt64 memory = LoadWord(block + Abi::FaultResolveMemoryObjectOffset);
                if (SharedHandles[slot] == 0U)
                {
                    SharedHandles[slot] = frame;
                    SharedHandles[slot + 1U] = memory;
                }
                SharedStable = SharedStable && (SharedHandles[slot] == frame) && (SharedHandles[slot + 1U] == memory) &&
                               (block[Abi::FaultResolvePermissionsOffset] == ((slot == 0U) ? 0x0DU : 0x09U));
                bool frameDerived{false};
                bool memoryDerived{false};
                for (const UInt64 handle : DerivedHandles)
                {
                    frameDerived = frameDerived || (frame == handle);
                    memoryDerived = memoryDerived || (memory == handle);
                }
                SharedStable = SharedStable && frameDerived && memoryDerived && MapOnlyDerives;
                ++SharedMaps;
            }
            if (address == reinterpret_cast<Foundation::UIntPointer>(g_window))
            {
                WindowOverlaps += (WindowMappedPages != 0U) ? 1U : 0U;
                WindowMappedPages = LoadWord(block + Abi::FaultResolvePageCountOffset);
            }
            if (address == Abi::ComponentForeignImageAddress)
            {
                ForeignCopied = (g_window[0U] == 0x7FU) && (g_window[1U] == 'E') && (g_window[0x1000U] == 0xC3U) &&
                                (block[Abi::FaultResolvePermissionsOffset] == 0x09U);
            }
            if (address == BssStart)
            {
                ++BssMaps;
                BssPermissions = block[Abi::FaultResolvePermissionsOffset];
            }
            return;
        }
        case Abi::SystemCallNumber::MemoryUnmap:
            ++Unmaps;
            response.Error = UnmapError;
            if ((UnmapFailures != 0U) || (Unmaps == FailUnmapAt))
            {
                UnmapFailures -= (UnmapFailures != 0U) ? 1U : 0U;
                response.Error = Abi::SystemCallError::WouldBlock;
            }
            if ((response.Error == Abi::SystemCallError::None) &&
                (request.Arguments[2U] == reinterpret_cast<Foundation::UIntPointer>(g_window)))
            {
                WindowMappedPages = 0U;
            }
            return;
        case Abi::SystemCallNumber::CapabilityClose:
            for (const UInt64 handle : SharedHandles)
            {
                SharedClosed += ((handle != 0U) && (request.Arguments[0U] == handle)) ? 1U : 0U;
            }
            ++Closes;
            return;
        case Abi::SystemCallNumber::DeviceControl:
            ++DeviceControls;
            DeviceOperation = request.Arguments[2U];
            if (DeviceOperation == Abi::DeviceQuiesced)
            {
                // The Device Manager's confirmation after the drain.
                QuiescedSequence = request.Arguments[3U];
                return;
            }
            AssignedFunction = request.Arguments[0U];
            AssigneeAuthenticator = request.Arguments[4U];
            response.Value0 = AssignmentSequence;
            return;
        default:
            ++Other;
            return;
        }
    }

    [[nodiscard]] Script Current() const noexcept
    {
        return Scripts[(Threads - 1U) % 5U];
    }

    // Which ResourceDomain an ObjectCreate charges: the grant, or the last
    // ResourceDomain created (a component's own).
    void Charge(const Abi::SystemCallRequest& request, const UInt64 created) noexcept
    {
        if (request.Arguments[2U] == Abi::ObjectTypeResourceDomain)
        {
            Account = created;
            std::memcpy(AccountRequest, reinterpret_cast<const UInt8*>(request.Arguments[3U]), sizeof(AccountRequest));
        }
        if (request.Arguments[2U] == Abi::ObjectTypeMemoryObject)
        {
            const auto* const memory = reinterpret_cast<const UInt8*>(request.Arguments[3U]);
            if ((memory[Abi::ObjectCreatePermissionsOffset] & 4U) != 0U)
            {
                ++ExecutableObjects;
                SealedAfterUnmap = SealedAfterUnmap && (Unmaps != 0U) && (WindowMappedPages == 0U) &&
                                   (request.Arguments[0U] == GrantSelector) &&
                                   (memory[Abi::ObjectCreatePermissionsOffset] == 0x0DU);
            }
        }
        GrantCharges += (request.Arguments[0U] == GrantSelector) ? 1U : 0U;
        AccountCharges += ((Account != 0U) && (request.Arguments[0U] == Account)) ? 1U : 0U;
    }

    void Wait(Abi::SystemCallResponse& response) noexcept
    {
        response.Value0 = Abi::PagerReadinessTag;
        response.Value1 = 1U;
        switch (Current())
        {
        case Script::Crash:
            PendingFault = !FaultDelivered;
            return;
        case Script::Report:
            ExitCode = Abi::ComponentExitCode;
            return;
        case Script::Account:
            ExitCode = Abi::ComponentAccountExitCode;
            return;
        case Script::Driver:
            ExitCode = Abi::ComponentDriverExitCode;
            return;
        case Script::DriverRegisters:
            ExitCode = Abi::ComponentDriverRegistersExitCode;
            return;
        case Script::Hang:
        case Script::CrashKilled:
            response = Abi::SystemCallResponse{.Error = Abi::SystemCallError::TimedOut, .Value0 = 0U, .Value1 = 0U};
            return;
        case Script::HangBlocked:
            if (!PendingFault)
            {
                response = Abi::SystemCallResponse{.Error = Abi::SystemCallError::TimedOut, .Value0 = 0U, .Value1 = 0U};
            }
            return;
        }
    }

    void Receive(const Abi::SystemCallRequest& request, Abi::SystemCallResponse& response) noexcept
    {
        auto* const delivery = reinterpret_cast<UInt8*>(request.Arguments[2U]);
        if (request.Arguments[0U] == Report)
        {
            ++Reports;
            std::memset(delivery, 0, 48U);
            std::memcpy(delivery + 48U, Abi::CanonicalAsynchronousFrame, sizeof(Abi::CanonicalAsynchronousFrame));
            response.Value0 = 1U;
            response.Value1 = 48U + sizeof(Abi::CanonicalAsynchronousFrame);
            return;
        }
        if ((request.Arguments[0U] != PagerEndpoint) || !PendingFault)
        {
            response.Error = Abi::SystemCallError::WouldBlock;
            return;
        }
        // A fault of the incarnation's domain, under its tag.
        PendingFault = false;
        FaultDelivered = true;
        std::memset(delivery, 0, 216U);
        std::memcpy(delivery + 48U, "AIPC", 4U);
        delivery[48U + 12U] = 48U;
        delivery[48U + 36U] = 1U;
        delivery[48U + 38U] = 2U;
        delivery[48U + 81U] = 1U;
        Store(delivery + 136U, 0x0F0FU, 8U);
        Store(delivery + 144U, 0x7A7AU, 8U);
        Store(delivery + 168U, 0x4000'0000U, 8U);
        Store(delivery + 200U, Threads, 8U);
        Store(delivery + 208U, 0xA000U + Threads, 8U);
        response.Value0 = 1U;
        response.Value1 = 216U;
    }

    void Log(const UInt64 event) noexcept
    {
        if (EventCount < (sizeof(Events) / sizeof(Events[0U])))
        {
            Events[EventCount] = event;
        }
        ++EventCount;
    }

    Script Scripts[5U]{Script::Crash, Script::Hang, Script::Report, Script::Account, Script::Driver};
    /// The ObjectCreate type refused with ResourceExhausted; zero for none.
    UInt64 FailCreateType{0U};
    Size DrainBusy{0U};
    Size DrainPolls{0U};
    Size PinnedDrains{0U};
    UInt64 Account{0U};
    UInt8 AccountRequest[Abi::ObjectCreateResourceDomainRequestSizeBytes]{};
    Size GrantCharges{0U};
    Size AccountCharges{0U};
    Size Unmaps{0U};
    Size UnmapFailures{0U};
    Size FailUnmapAt{0U};
    Size DestroyMemoryFailures{0U};
    UInt64 WindowMappedPages{0U};
    Size WindowOverlaps{0U};
    Size Derives{0U};
    UInt64 DerivedHandles[4U]{};
    bool MapOnlyDerives{true};
    Size ExecutableObjects{0U};
    bool SealedAfterUnmap{true};
    Abi::SystemCallError UnmapError{Abi::SystemCallError::None};
    Size Closes{0U};
    Size DeviceControls{0U};
    /// DeviceAssign's sequence, and the one DeviceQuiesced named.
    UInt64 AssignmentSequence{0x5E0U};
    UInt64 QuiescedSequence{0U};
    UInt64 AssignedFunction{0U};
    UInt64 DeviceOperation{0U};
    UInt64 AssigneeAuthenticator{0U};
    UInt64 SharedHandles[4U]{};
    Size SharedMaps{0U};
    Size SharedLines{0U};
    Size SharedDestroyed{0U};
    Size SharedClosed{0U};
    bool SharedStable{true};
    bool ForeignCopied{false};
    Size Maps{0U};
    Size BssMaps{0U};
    UInt8 BssPermissions{0U};
    Abi::SystemCallError KillError{Abi::SystemCallError::None};
    UInt64 Handles{0U};
    UInt64 Report{0U};
    UInt64 Thread{0U};
    UInt64 Threads{0U};
    UInt64 ExitCode{0U};
    bool PendingFault{false};
    bool FaultDelivered{false};
    UInt8 LastThread[Abi::ObjectCreateThreadRequestSizeBytes]{};
    Size BusyPolls{0U};
    Size Waits{0U};
    Size TimedWaits{0U};
    Size Kills{0U};
    UInt64 KilledThread{0U};
    Size Terminates{0U};
    Size Reports{0U};
    Size Grants{0U};
    UInt64 GrantTarget{0U};
    Size Other{0U};
    char Lines[4U][96U]{};
    Size LineCount{0U};
    UInt64 Events[96U]{};
    Size EventCount{0U};
};

[[nodiscard]] PagerBootstrap Bootstrap(const UInt64 features) noexcept
{
    return PagerBootstrap{
        .Endpoint = {PagerEndpoint, 0x1111U},
        .WaitSet = {0x22U, 0x2222U},
        .Diagnostic = {0x33U, 0x3333U},
        .ClientSpace = {0x44U, 0x4444U},
        .LazyMemory = {0x55U, 0x5555U},
        .LazyFrame = {0x66U, 0x6666U},
        .LazyStart = 0x7000'0000U,
        .LazyPages = 2U,
        .Management = {GrantSelector, 0x9999U},
        .ManagementFeatures = features,
        .OwnSpace = {0x88U, 0x8888U},
        .WindowStart = reinterpret_cast<Foundation::UIntPointer>(g_window),
        .WindowPages = User::Pager::Amd64ComponentSegmentPages,
        .Devices = {{.Function = {0xD1U, 0xD2U}, .Region = {0xD3U, 0xD4U}}, {.Function = {}, .Region = {}}},
        .Scanout = {},
    };
}

[[nodiscard]] PagerThreadLaunch Launch(const PagerBootstrap& bootstrap, const UInt64 windowPages) noexcept
{
    return PagerThreadLaunch{
        .Entry = 0U,
        .StackTop = 0U,
        .SpinLimit = 0U,
        .FaultEntry = 0U,
        .ExceptionEntry = 0U,
        .SpinningEntry = 0U,
        .WaitingEntry = 0U,
        .ComponentImages = User::Pager::ModuleTableBytes(bootstrap.ModuleBase),
        .ComponentImagesBytes = bootstrap.ModuleBytes,
        .ComponentSpinTicks = 77U,
        .Component = ComponentPlatform{.Image = User::Pager::MakeComponentRules(
                                           User::Loader::ExecutableMachine::Amd64, 0x1000U, 0x0000'8000'0000'0000ULL,
                                           false, User::Pager::Amd64ComponentSegmentPages),
                                       .StackStart = StackStart,
                                       .StackPages = StackPages,
                                       .FlushCode = Flush,
                                       .WatchdogMicroseconds = WatchdogMicroseconds,
                                       .ForeignImageAddress = Abi::ComponentForeignImageAddress,
                                       .WindowStart = (windowPages != 0U) ? WindowStart : 0U,
                                       .WindowPages = windowPages},
        .DomainCycles = 0U,
        .ServiceDemo = nullptr,
        .LoaderDemo = nullptr,
        .DriverDemo = nullptr,
        .BlockDemo = nullptr,
        .FsDemo = nullptr,
        .NetDemo = nullptr};
}

// Run with a fake immutable module slice whose lifetime includes every rerun.
[[nodiscard]] PagerServeOutcome RunTable(RootService& root, ComponentKernel& kernel, const ImageTable& table,
                                         const UInt64 features = DomainFeatures, const UInt64 windowPages = 0U) noexcept
{
    const SystemCallClient client{SystemCallTransport{&ComponentKernel::Transport, &kernel}};
    PagerBootstrap bootstrap = Bootstrap(features);
    bootstrap.ModuleBase = User::Pager::AddressOf(table.Bytes);
    bootstrap.ModuleBytes = sizeof(table.Bytes);
    const PagerThreadLaunch launch = Launch(bootstrap, windowPages);
    PagerProgram pager{client, bootstrap, &launch};
    return root.Run(client, bootstrap, pager, launch);
}

// A single run in a fresh pager/kernel fixture.
[[nodiscard]] PagerServeOutcome Run(RootService& root, ComponentKernel& kernel, const Image& image,
                                    const UInt64 features = DomainFeatures, const UInt64 windowPages = 0U) noexcept
{
    const auto table = std::make_unique<ImageTable>(image);
    return RunTable(root, kernel, *table, features, windowPages);
}

// The three incarnations each run in a domain of their own record,
// end as their behavior asks, and are torn down before the next.
void TestIncarnations(TestContext& runner) noexcept
{
    const auto image = std::make_unique<Image>();
    const auto root = std::make_unique<RootService>();
    const auto kernel = std::make_unique<ComponentKernel>();
    g_flushes = 0U;
    const PagerServeOutcome outcome = Run(*root, *kernel, *image);
    const UInt64* const events = kernel->Events;
    const UInt8* const block = g_window + (StackPages * PageBytes);
    ASTERIA_EXPECT(runner, "the root service builds, ends and tears down each incarnation in order",
                   outcome.Error == PagerServeError::None, kernel->Other == 0U, kernel->EventCount == 46U,
                   events[0U] == (Created + Abi::ObjectTypeEndpoint), kernel->SharedMaps == 6U, kernel->SharedStable,
                   kernel->ExecutableObjects == 1U, kernel->SealedAfterUnmap, kernel->SharedDestroyed == 0U,
                   kernel->SharedClosed == 0U, events[45U] == (Destroyed + Abi::ObjectTypeEndpoint),
                   kernel->Threads == 3U, kernel->Grants == 3U, g_flushes == 1U, g_flushedBytes == (2U * PageBytes));
    ASTERIA_EXPECT(runner,
                   "the crash is terminated, the hang killed after its watchdog wait, the report received, done",
                   kernel->LineCount == 5U, std::strcmp(kernel->Lines[1U], User::Pager::ComponentCrashedLine) == 0,
                   std::strcmp(kernel->Lines[2U], User::Pager::ComponentHungLine) == 0,
                   std::strcmp(kernel->Lines[3U], User::Pager::ComponentReportedLine) == 0,
                   std::strcmp(kernel->Lines[0U], User::Pager::RootServiceDoneLine) == 0, kernel->Terminates == 1U,
                   kernel->Kills == 1U, kernel->KilledThread == (0x1000U + 19U), kernel->Reports == 1U,
                   kernel->Waits == 3U, kernel->TimedWaits == kernel->Waits);
    ASTERIA_EXPECT(
        runner, "the last thread starts at the entry with the start block as stack and argument",
        LoadWord(kernel->LastThread + Abi::ObjectCreateThreadEntryOffset) == Entry,
        LoadWord(kernel->LastThread + Abi::ObjectCreateThreadStackOffset) == (StackStart + (StackPages * PageBytes)),
        LoadWord(kernel->LastThread + Abi::ObjectCreateThreadArgumentOffset) == (StackStart + (StackPages * PageBytes)),
        LoadWord(kernel->LastThread + Abi::ObjectCreateThreadDomainOffset) == (0x1000U + 22U),
        LoadWord(block + (8U * Abi::ComponentStartMagicWord)) == Abi::ComponentStartMagic,
        LoadWord(block + (8U * Abi::ComponentIncarnationWord)) == 3U,
        LoadWord(block + (8U * Abi::ComponentReportWord)) == 0x5003U,
        LoadWord(block + (8U * Abi::ComponentSpinTicksWord)) == 77U,
        LoadWord(block + (8U * Abi::ComponentBehaviorWord)) == Abi::ComponentBehaviorReport,
        kernel->GrantTarget == (0x1000U + 21U));

    // A fresh pager and kernel reproduce the complete incarnation sequence.
    const auto again = std::make_unique<ComponentKernel>();
    const PagerServeOutcome rerun = Run(*std::make_unique<RootService>(), *again, *image);
    ASTERIA_EXPECT(runner, "a fresh pager reproduces the incarnation sequence", rerun.Error == PagerServeError::None,
                   again->LineCount == 5U, again->EventCount == 46U);
}

// The same pager and kernel retain shared objects across completed runs.
// Only the transport's script cursor is rewound; capabilities stay valid.
void TestSameInstanceRerun(TestContext& runner) noexcept
{
    const auto image = std::make_unique<Image>();
    const auto table = std::make_unique<ImageTable>(*image);
    const auto root = std::make_unique<RootService>();
    const auto kernel = std::make_unique<ComponentKernel>();
    g_flushes = 0U;
    const PagerServeOutcome first =
        RunTable(*root, *kernel, *table, DomainFeatures | Abi::PagerManagementResourceDomains);
    const Size created = kernel->Handles;
    kernel->Threads = 0U;
    const PagerServeOutcome second =
        RunTable(*root, *kernel, *table, DomainFeatures | Abi::PagerManagementResourceDomains);
    ASTERIA_EXPECT(runner, "a finished run frees incarnation records and the same instance reuses sealed code",
                   first.Error == PagerServeError::None, second.Error == PagerServeError::None, kernel->Threads == 3U,
                   kernel->LineCount == 10U, kernel->Handles > created, kernel->SharedMaps == 12U, kernel->SharedStable,
                   kernel->Derives == 4U, kernel->ExecutableObjects == 1U, g_flushes == 1U,
                   kernel->WindowMappedPages == 0U, kernel->WindowOverlaps == 0U, kernel->SharedDestroyed == 0U,
                   kernel->SharedClosed == 0U);
}

// A.bss segment is a Frame and a read-write MemoryObject mapped at once, without staging, and torn
// down like the rest.
void TestBssSegment(TestContext& runner) noexcept
{
    const auto image = std::make_unique<BssImage>();
    const auto root = std::make_unique<RootService>();
    const auto kernel = std::make_unique<ComponentKernel>();
    const PagerServeOutcome outcome = Run(*root, *kernel, *image);
    ASTERIA_EXPECT(runner, "each incarnation's .bss is mapped read-write from fresh memory, never staged",
                   outcome.Error == PagerServeError::None, kernel->LineCount == 5U,
                   kernel->EventCount == (46U + (3U * 4U)), kernel->BssMaps == 3U, kernel->BssPermissions == 0x0BU,
                   kernel->Maps == (2U + (3U * 5U)));
}

// A hang the watchdog fires on while it is blocked on a fault: the kill is
// left to the fault, which the next wait delivers and terminates.
void TestHangEndedByItsFault(TestContext& runner) noexcept
{
    const auto image = std::make_unique<Image>();
    const auto root = std::make_unique<RootService>();
    const auto kernel = std::make_unique<ComponentKernel>();
    kernel->Scripts[1U] = Script::HangBlocked;
    const PagerServeOutcome outcome = Run(*root, *kernel, *image);
    ASTERIA_EXPECT(runner, "a hang whose kill finds it faulting ends by the fault's termination",
                   outcome.Error == PagerServeError::None, kernel->LineCount == 5U,
                   std::strcmp(kernel->Lines[2U], User::Pager::ComponentHungLine) == 0, kernel->Kills == 1U,
                   kernel->Terminates == 2U, kernel->Waits == 4U, kernel->TimedWaits == 4U, kernel->EventCount == 46U);
}

// Where the platform offers ResourceDomains, private objects charge the child and shared image
// objects charge the grant. The drain, retried on WouldBlock, releases private mappings while the
// shared handles survive for the next incarnation.
void TestComponentDomains(TestContext& runner) noexcept
{
    const auto image = std::make_unique<Image>();
    const auto root = std::make_unique<RootService>();
    const auto kernel = std::make_unique<ComponentKernel>();
    kernel->DrainBusy = 2U;
    const PagerServeOutcome outcome = Run(*root, *kernel, *image, DomainFeatures | Abi::PagerManagementResourceDomains);
    constexpr Size Events{37U};
    const bool retained = (kernel->SharedMaps == 6U) && kernel->SharedStable && (kernel->SharedDestroyed == 0U) &&
                          (kernel->SharedClosed == 0U);
    const UInt8* const request = kernel->AccountRequest;
    // Only the stack (two pages) and start block charge this child.
    constexpr UInt64 Pages{3U};
    ASTERIA_EXPECT(runner, "each incarnation drains its private domain and retains shared image objects",
                   outcome.Error == PagerServeError::None, kernel->Other == 0U, retained, kernel->EventCount == Events,
                   kernel->LineCount == 5U, kernel->GrantCharges == 10U, kernel->AccountCharges == (3U * 6U),
                   kernel->DrainPolls == 2U, LoadWord(request + Abi::ObjectCreateDomainCommittedPagesOffset) == Pages,
                   LoadWord(request + Abi::ObjectCreateDomainResidentPagesOffset) == Pages,
                   LoadWord(request + Abi::ObjectCreateDomainMetadataOffset) == 0U,
                   LoadWord(request + Abi::ObjectCreateDomainHandlesOffset) == 0U,
                   LoadWord(request + Abi::ObjectCreateDomainQueueBytesOffset) == 0U,
                   LoadWord(request + Abi::ObjectCreateDomainThreadsOffset) == 1U,
                   LoadWord(request + Abi::ObjectCreateDomainChildrenOffset) == 0U,
                   LoadWord(kernel->LastThread + Abi::ObjectCreateThreadDomainOffset) == (0x1000U + 25U));
    // Only the staging mappings are unmapped; the drain takes the rest with
    // the AddressSpace. Each incarnation closes its two replaced staging
    // MemoryObjects, its thread, its domain, spaces and ProtectionDomain and
    // its three mappings' objects; then the report Endpoint. The pager
    // closes the crash's fault besides.
    ASTERIA_EXPECT(runner, "the drain leaves the mappings to the kernel and closes every handle",
                   kernel->Unmaps == (2U + 3U), kernel->Closes == (2U + 4U + (3U * 7U) + 1U + 1U));

    const auto again = std::make_unique<ComponentKernel>();
    const PagerServeOutcome rerun =
        Run(*std::make_unique<RootService>(), *again, *image, DomainFeatures | Abi::PagerManagementResourceDomains);
    ASTERIA_EXPECT(runner, "a fresh pager reproduces the domain-drain sequence", rerun.Error == PagerServeError::None,
                   again->LineCount == 5U, again->EventCount == Events);
}

// With a window, a fourth incarnation gets copies of its own domain (Manage and Inspect) and space
// (Map) and the window, which its domain's pages cover, besides its image's, and one Endpoint's
// queue it creates itself.
void TestAccountIncarnation(TestContext& runner) noexcept
{
    const auto image = std::make_unique<Image>();
    const auto root = std::make_unique<RootService>();
    const auto kernel = std::make_unique<ComponentKernel>();
    const PagerServeOutcome outcome =
        Run(*root, *kernel, *image, DomainFeatures | Abi::PagerManagementResourceDomains, 2U);
    const UInt8* const block = g_window + (StackPages * PageBytes);
    const UInt8* const request = kernel->AccountRequest;
    ASTERIA_EXPECT(
        runner, "a fourth incarnation creates in its own domain within its window's pages",
        outcome.Error == PagerServeError::None, kernel->Other == 0U, kernel->Threads == 4U, kernel->LineCount == 6U,
        std::strcmp(kernel->Lines[0U], User::Pager::ComponentAccountLine) == 0, kernel->Grants == (4U + 2U),
        kernel->Reports == 2U, LoadWord(block + (8U * Abi::ComponentBehaviorWord)) == Abi::ComponentBehaviorAccount,
        LoadWord(block + (8U * Abi::ComponentIncarnationWord)) == 4U,
        LoadWord(block + (8U * Abi::ComponentAccountWord)) == 0x5005U,
        LoadWord(block + (8U * Abi::ComponentAccountSpaceWord)) == 0x5006U,
        LoadWord(block + (8U * Abi::ComponentWindowStartWord)) == WindowStart,
        LoadWord(block + (8U * Abi::ComponentWindowPagesWord)) == 2U,
        LoadWord(request + Abi::ObjectCreateDomainCommittedPagesOffset) == (3U + 2U),
        LoadWord(request + Abi::ObjectCreateDomainQueueBytesOffset) == User::System::DefaultEndpointQueuedBytes);

    // Without ResourceDomains there is no fourth incarnation, window or not.
    const auto plain = std::make_unique<ComponentKernel>();
    const PagerServeOutcome unaccounted = Run(*std::make_unique<RootService>(), *plain, *image, DomainFeatures, 2U);
    ASTERIA_EXPECT(runner, "without ResourceDomains no incarnation creates in its own domain",
                   unaccounted.Error == PagerServeError::None, plain->Threads == 3U, plain->LineCount == 5U);
}

// Where the platform grants an assignable device, a fifth incarnation, a driver, gets its own
// domain's and space's copies and the window with a page more for the registers, its domain
// covering the window's pages for its DMA, and copies of the device's region and function, which is
// assigned to its ProtectionDomain.
void TestDriverIncarnation(TestContext& runner) noexcept
{
    const auto image = std::make_unique<Image>();
    const auto root = std::make_unique<RootService>();
    const auto kernel = std::make_unique<ComponentKernel>();
    const PagerServeOutcome outcome = Run(
        *root, *kernel, *image, DomainFeatures | Abi::PagerManagementResourceDomains | Abi::PagerManagementDevices, 2U);
    const UInt8* const block = g_window + (StackPages * PageBytes);
    ASTERIA_EXPECT(runner, "a fifth incarnation drives the device with its own copies of it",
                   outcome.Error == PagerServeError::None, kernel->Other == 0U, kernel->Threads == 5U,
                   kernel->LineCount == 7U, std::strcmp(kernel->Lines[1U], User::Pager::ComponentDriverLine) == 0,
                   kernel->Grants == (5U + 2U + 4U), kernel->Reports == 3U,
                   LoadWord(block + (8U * Abi::ComponentBehaviorWord)) == Abi::ComponentBehaviorDriver,
                   LoadWord(block + (8U * Abi::ComponentIncarnationWord)) == 5U,
                   LoadWord(block + (8U * Abi::ComponentDeviceRegionWord)) == (0x5000U + 10U),
                   LoadWord(block + (8U * Abi::ComponentDeviceFunctionWord)) == (0x5000U + 11U),
                   LoadWord(block + (8U * Abi::ComponentWindowPagesWord)) == 3U,
                   LoadWord(kernel->AccountRequest + Abi::ObjectCreateDomainCommittedPagesOffset) == 5U,
                   kernel->DeviceControls == 2U, kernel->AssignedFunction == 0xD1U,
                   kernel->DeviceOperation == Abi::DeviceQuiesced, kernel->QuiescedSequence == 0x5E0U,
                   kernel->AssigneeAuthenticator == (0xA000U + Abi::ObjectTypeProtectionDomain));

    // Without an IOMMU the driver drives the device through its registers
    // only, and says so.
    const auto registers = std::make_unique<ComponentKernel>();
    registers->Scripts[4U] = Script::DriverRegisters;
    const PagerServeOutcome unconfined =
        Run(*std::make_unique<RootService>(), *registers, *image,
            DomainFeatures | Abi::PagerManagementResourceDomains | Abi::PagerManagementDevices, 2U);
    ASTERIA_EXPECT(runner, "a driver without an IOMMU reports through its registers only",
                   unconfined.Error == PagerServeError::None, registers->LineCount == 7U,
                   std::strcmp(registers->Lines[1U], User::Pager::ComponentDriverRegistersLine) == 0,
                   registers->Reports == 3U, registers->DeviceOperation == Abi::DeviceQuiesced);

    // Without the device feature there is no driver.
    const auto plain = std::make_unique<ComponentKernel>();
    const PagerServeOutcome unequipped =
        Run(*std::make_unique<RootService>(), *plain, *image, DomainFeatures | Abi::PagerManagementResourceDomains, 2U);
    ASTERIA_EXPECT(runner, "without an assignable device no driver runs", unequipped.Error == PagerServeError::None,
                   plain->Threads == 4U, plain->DeviceControls == 0U);
}

// An incarnation that ends otherwise than its behavior asks fails its
// scenario: the root service abandons its components, torn down, says so and
// runs no scenario after. A kill refused fails the abandonment too, and the domain
// stays standing.
void TestUnexpectedEnds(TestContext& runner) noexcept
{
    const auto image = std::make_unique<Image>();
    const auto killed = std::make_unique<ComponentKernel>();
    killed->Scripts[0U] = Script::CrashKilled;
    const auto killedRoot = std::make_unique<RootService>();
    const PagerServeOutcome unexpected = Run(*killedRoot, *killed, *image);
    constexpr char Diagnostic[] = "# component: incarnation 1 ended with 0x8000000000000003, crashed 0\n";
    const bool tornDown = (killed->SharedDestroyed == 0U) && (killed->SharedClosed == 0U);

    const auto refused = std::make_unique<ComponentKernel>();
    refused->Scripts[0U] = Script::Hang;
    refused->KillError = Abi::SystemCallError::InvalidArguments;
    const auto refusedRoot = std::make_unique<RootService>();
    const PagerServeOutcome unkilled = Run(*refusedRoot, *refused, *image);
    ASTERIA_EXPECT(runner, "a crash the watchdog killed fails its scenario, whose domain is torn down",
                   unexpected.Error == PagerServeError::ManagementFailed, killed->LineCount == 3U,
                   std::strcmp(killed->Lines[1U], Diagnostic) == 0,
                   std::strcmp(killed->Lines[2U], "# root service: crash failed, components torn down\n") == 0,
                   killed->EventCount == 21U, tornDown, killed->Threads == 1U);
    ASTERIA_EXPECT(runner, "a kill refused fails the abandonment too, and the domain stands",
                   unkilled.Error == PagerServeError::ManagementFailed,
                   unkilled.SystemCall == Abi::SystemCallError::InvalidArguments, refused->LineCount == 2U,
                   std::strcmp(refused->Lines[1U], "# root service: crash failed, components left standing\n") == 0,
                   refused->EventCount == 15U, refused->Kills == 2U);
}

// A build that fails part way is still the supervisor's (ComponentState
// Building): abandonment drains its ResourceDomain and preserves shared
// image objects under the grant.
void TestFailedBuild(TestContext& runner) noexcept
{
    const auto image = std::make_unique<Image>();
    const auto root = std::make_unique<RootService>();
    const auto kernel = std::make_unique<ComponentKernel>();
    kernel->FailCreateType = Abi::ObjectTypeThread;
    const PagerServeOutcome failed = Run(*root, *kernel, *image, DomainFeatures | Abi::PagerManagementResourceDomains);
    const UInt64 last = kernel->Events[(kernel->EventCount - 1U) % 96U];
    ASTERIA_EXPECT(runner, "a failed build's domain is drained and its scenario reported",
                   failed.Error == PagerServeError::ManagementFailed,
                   failed.SystemCall == Abi::SystemCallError::ResourceExhausted, kernel->Threads == 0U,
                   last == (Destroyed + Abi::ObjectTypeResourceDomain), kernel->SharedDestroyed == 0U,
                   kernel->SharedClosed == 0U, kernel->LineCount == 2U,
                   std::strcmp(kernel->Lines[1U], "# root service: crash failed, components torn down\n") == 0);

    const auto healthy = std::make_unique<ComponentKernel>();
    const PagerServeOutcome rerun =
        Run(*std::make_unique<RootService>(), *healthy, *image, DomainFeatures | Abi::PagerManagementResourceDomains);
    ASTERIA_EXPECT(runner, "a fresh pager completes after a separately injected build failure",
                   rerun.Error == PagerServeError::None, healthy->LineCount == 5U);
}

// Failure of the synchronous staging unmap must prevent sealing or
// executable publication. A persistent failure leaves the window range
// recorded and blocks abandonment before a private drain or device reuse.
void TestFailedStaging(TestContext& runner) noexcept
{
    const auto image = std::make_unique<Image>();
    const auto root = std::make_unique<RootService>();
    const auto kernel = std::make_unique<ComponentKernel>();
    kernel->UnmapError = Abi::SystemCallError::WouldBlock;
    const PagerServeOutcome failed = Run(*root, *kernel, *image, DomainFeatures | Abi::PagerManagementResourceDomains);
    ASTERIA_EXPECT(runner, "an incomplete staging unmap cannot publish executable memory",
                   failed.Error == PagerServeError::ManagementFailed,
                   failed.SystemCall == Abi::SystemCallError::WouldBlock, kernel->ExecutableObjects == 0U,
                   kernel->SharedMaps == 0U, kernel->Threads == 0U, kernel->WindowMappedPages != 0U,
                   kernel->Unmaps == 2U,
                   std::strcmp(kernel->Lines[1U], "# root service: crash failed, components left standing\n") == 0);
}

// Unmap/destroy failures poison the image without leaving an unnoticed
// window mapping. Abandon retries a transient unmap before any reuse.
void TestStagingRecovery(TestContext& runner) noexcept
{
    for (const bool unmap : {false, true})
    {
        const auto image = std::make_unique<Image>();
        const auto table = std::make_unique<ImageTable>(*image);
        const auto root = std::make_unique<RootService>();
        const auto kernel = std::make_unique<ComponentKernel>();
        kernel->UnmapFailures = unmap ? 1U : 0U;
        kernel->DestroyMemoryFailures = unmap ? 0U : 1U;
        const PagerServeOutcome failed =
            RunTable(*root, *kernel, *table, DomainFeatures | Abi::PagerManagementResourceDomains);
        const PagerServeOutcome poisoned =
            RunTable(*root, *kernel, *table, DomainFeatures | Abi::PagerManagementResourceDomains);
        ASTERIA_EXPECT(runner, "failed staging cleans the window and a poisoned image has a distinct error",
                       failed.Error == PagerServeError::ManagementFailed,
                       failed.SystemCall == Abi::SystemCallError::WouldBlock,
                       poisoned.Error == PagerServeError::ManagementFailed,
                       poisoned.SystemCall == Abi::SystemCallError::InvalidConfiguration,
                       kernel->WindowMappedPages == 0U, kernel->WindowOverlaps == 0U, kernel->ExecutableObjects == 0U,
                       kernel->SharedMaps == 0U, kernel->Threads == 0U);
    }
}

// The driver's start-block unmap fails after device assignment. Its drain
// must retry that unmap before RecoverDevice maps the register page.
void TestDeviceRecoveryWindow(TestContext& runner) noexcept
{
    const auto image = std::make_unique<Image>();
    const auto root = std::make_unique<RootService>();
    const auto kernel = std::make_unique<ComponentKernel>();
    kernel->FailUnmapAt = 7U; // Two image stages, then five start blocks
    const PagerServeOutcome failed = Run(
        *root, *kernel, *image, DomainFeatures | Abi::PagerManagementResourceDomains | Abi::PagerManagementDevices, 2U);
    ASTERIA_EXPECT(runner, "device recovery cleans the failed staging range before mapping its registers",
                   failed.Error == PagerServeError::ManagementFailed,
                   failed.SystemCall == Abi::SystemCallError::WouldBlock, kernel->Threads == 4U,
                   kernel->DeviceControls == 2U, kernel->DeviceOperation == Abi::DeviceQuiesced,
                   kernel->PinnedDrains == 0U, kernel->WindowMappedPages == 0U, kernel->WindowOverlaps == 0U,
                   kernel->SharedDestroyed == 0U, kernel->SharedClosed == 0U);
}

// Foreign guest bytes remain private to the interpreter even when the
// interpreter's native code is shared. Check the bytes at publication.
void TestForeignStaging(TestContext& runner) noexcept
{
    const auto image = std::make_unique<Image>();
    const auto table = std::make_unique<ImageTable>(*image);
    const auto kernel = std::make_unique<ComponentKernel>();
    const SystemCallClient client{SystemCallTransport{&ComponentKernel::Transport, kernel.get()}};
    PagerBootstrap bootstrap = Bootstrap(DomainFeatures | Abi::PagerManagementResourceDomains);
    bootstrap.ModuleBase = User::Pager::AddressOf(table->Bytes);
    bootstrap.ModuleBytes = sizeof(table->Bytes);
    const PagerThreadLaunch launch = Launch(bootstrap, 0U);
    User::Pager::ObjectCalls calls{};
    calls.Configure(client);
    User::Pager::DeviceManagerRole devices{};
    devices.Configure(client, bootstrap.Devices);
    const auto loader = std::make_unique<User::Pager::ComponentLoader>();
    loader->Configure(calls, bootstrap, launch, devices);
    const auto loaded = std::make_unique<User::Pager::LoadedImage>();
    loaded->Bytes = image->Bytes;
    loaded->Size = sizeof(image->Bytes);
    const auto described =
        User::Loader::DescribeExecutable(loaded->Bytes, loaded->Size, launch.Component.Image, loaded->Layout);
    const auto component = std::make_unique<User::Pager::ComponentRecord>();
    component->Mode = User::Pager::ComponentMode::Task;
    component->State = User::Pager::ComponentState::Building;
    const User::Pager::ComponentBuild build{
        .Image = loaded.get(),
        .Tag = 1U,
        .Behavior = Abi::ComponentBehaviorInterpret,
        .Foreign = {.Bytes = image->Bytes, .Size = sizeof(image->Bytes), .Identity = loaded->Layout.Identity},
        .Report = {0x77U, 0x78U},
        .Directory = {},
        .DriverService = {},
        .DriverGeneration = 0U,
        .ServiceFlags = 0U,
        .Deferred = true};
    Abi::SystemCallError systemCall{Abi::SystemCallError::None};
    const bool built = loader->Load(*component, build, systemCall);
    ASTERIA_EXPECT(runner, "an interpreter receives staged read-only guest bytes and shared native code",
                   described == User::Loader::LoaderError::None, built, systemCall == Abi::SystemCallError::None,
                   kernel->ForeignCopied, component->MappingCount == 4U, component->Mappings[0U].Shared,
                   !component->Mappings[2U].Shared, kernel->ExecutableObjects == 1U);
}

/// The kernel a mirrored line meets: the debug channel takes every write,
/// the broker's Endpoint every send until SendError says otherwise.
struct MirrorKernel
{
    static void Transport(void* const context, const Abi::SystemCallRequest* const request,
                          Abi::SystemCallResponse* const response) noexcept
    {
        auto& kernel = *static_cast<MirrorKernel*>(context);
        *response = Abi::SystemCallResponse{.Error = Abi::SystemCallError::None, .Value0 = 0U, .Value1 = 0U};
        if (request->Number == static_cast<UInt64>(Abi::SystemCallNumber::DebugWrite))
        {
            kernel.Debugged += request->Arguments[3U];
            return;
        }
        if (request->Number != static_cast<UInt64>(Abi::SystemCallNumber::IpcSend))
        {
            response->Error = Abi::SystemCallError::UnknownSystemCall;
            return;
        }
        response->Error = kernel.SendError;
        if (kernel.Sends < 4U)
        {
            std::memcpy(kernel.Frames[kernel.Sends] + 48U,
                        reinterpret_cast<const UInt8*>(static_cast<Foundation::UIntPointer>(request->Arguments[2U])),
                        static_cast<Size>(request->Arguments[3U]));
            kernel.Sizes[kernel.Sends] = static_cast<Size>(request->Arguments[3U]) + 48U;
            kernel.Targets[kernel.Sends] = request->Arguments[0U];
        }
        ++kernel.Sends;
    }

    Abi::SystemCallError SendError{Abi::SystemCallError::None};
    UInt64 Debugged{0U};
    Size Sends{0U};
    UInt8 Frames[4U][48U + User::Component::DisplayMaximumFrameBytes]{};
    Size Sizes[4U]{};
    UInt64 Targets[4U]{};
};

// While the broker runs a line goes to the debug channel and to the console,
// split at a frame's payload; a send refused other than for a full queue
// stops the mirroring, never the line.
void TestMirroredLines(TestContext& runner) noexcept
{
    const auto kernel = std::make_unique<MirrorKernel>();
    const SystemCallClient client{SystemCallTransport{&MirrorKernel::Transport, kernel.get()}};
    const auto context = std::make_unique<User::Pager::ScenarioContext>();
    User::Pager::DisplayMirror mirror{.Console = {0x77U, 0x78U}, .Transaction = 0U, .Dropped = false};
    context->Client = &client;
    context->Display = &mirror;
    char line[250]{};
    for (Size index{0U}; index < sizeof(line); ++index)
    {
        line[index] = static_cast<char>('a' + (index % 26U));
    }
    line[sizeof(line) - 1U] = '\n';
    User::Pager::WriteLine(*context, line, sizeof(line));
    User::Component::DisplayMessage first{};
    User::Component::DisplayMessage second{};
    const bool read = User::Component::ReadDisplayFrame(kernel->Frames[0U], kernel->Sizes[0U], first) &&
                      User::Component::ReadDisplayFrame(kernel->Frames[1U], kernel->Sizes[1U], second);
    ASTERIA_EXPECT(runner, "a line longer than a frame reaches the console in two, in order, and the channel whole",
                   kernel->Debugged == sizeof(line), kernel->Sends == 2U, read, kernel->Targets[0U] == 0x77U,
                   first.Method == User::Component::DisplayConsoleWriteMethod, first.Transaction == 1U,
                   first.Count == User::Component::DisplayMaximumWriteBytes, second.Transaction == 2U,
                   second.Count == (sizeof(line) - User::Component::DisplayMaximumWriteBytes),
                   std::memcmp(first.Text, line, first.Count) == 0,
                   std::memcmp(second.Text, line + first.Count, second.Count) == 0, mirror.Console.Selector == 0x77U,
                   !mirror.Dropped);

    kernel->SendError = Abi::SystemCallError::InvalidHandle;
    User::Pager::WriteLine(*context, line, 10U);
    User::Pager::WriteLine(*context, line, 10U);
    ASTERIA_EXPECT(runner, "a send the broker's Endpoint refuses stops the mirroring, not the lines",
                   kernel->Sends == 3U, kernel->Debugged == (sizeof(line) + 20U), mirror.Console.Selector == 0U,
                   mirror.Dropped);
}
// The filesystem and block demonstrations write to and may format the AHCI
// controller's disk: they run only where the start block says it holds no
// disk but the emulator's, and are skipped otherwise.
void TestDiskDemonstrations(TestContext& runner) noexcept
{
    using User::Pager::DiskDemonstrations;
    using User::Pager::SelectDiskDemonstrations;
    constexpr UInt64 granted{DomainFeatures | Abi::PagerManagementResourceDomains | Abi::PagerManagementDevices};
    ASTERIA_EXPECT(
        runner, "the disk demonstrations run only on a disk the boot found to be the emulator's",
        SelectDiskDemonstrations(granted | Abi::PagerManagementDiskWritable, true) == DiskDemonstrations::Run,
        SelectDiskDemonstrations(granted, true) == DiskDemonstrations::Skipped,
        SelectDiskDemonstrations(granted | Abi::PagerManagementDiskWritable, false) == DiskDemonstrations::None,
        SelectDiskDemonstrations(granted, false) == DiskDemonstrations::None);
}
} // Namespace
} // Namespace Asteria::Test

int main()
{
    TestContext runner{"RootServiceTests"};
    Asteria::Test::TestIncarnations(runner);
    Asteria::Test::TestHangEndedByItsFault(runner);
    Asteria::Test::TestBssSegment(runner);
    Asteria::Test::TestUnexpectedEnds(runner);
    Asteria::Test::TestFailedBuild(runner);
    Asteria::Test::TestFailedStaging(runner);
    Asteria::Test::TestStagingRecovery(runner);
    Asteria::Test::TestDeviceRecoveryWindow(runner);
    Asteria::Test::TestSameInstanceRerun(runner);
    Asteria::Test::TestForeignStaging(runner);
    Asteria::Test::TestComponentDomains(runner);
    Asteria::Test::TestAccountIncarnation(runner);
    Asteria::Test::TestDriverIncarnation(runner);
    Asteria::Test::TestMirroredLines(runner);
    Asteria::Test::TestDiskDemonstrations(runner);
    return runner.Finish();
}
