// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_PAGER_PAGER_PROGRAM_HPP_INCLUDED
#define ASTERIA_USER_PAGER_PAGER_PROGRAM_HPP_INCLUDED

#include "Abi/ComponentBootstrap.hpp"
#include "Abi/PagerBootstrap.hpp"
#include "Foundation/Types.hpp"
#include "User/Pager/ComponentImage.hpp"
#include "User/System/ObjectRequests.hpp"
#include "User/System/SystemCallClient.hpp"

namespace Asteria::User::Pager
{
using System::CapabilityHandle;
/// IA-64 test cycles retaining the same objects across acknowledged unmaps.
inline constexpr Foundation::UInt64 LedgerReuseSelfTestCycles{300U};

/// An entry of the start block's device table: a PCI function and the
/// DeviceRegion of its BAR, zero for none.
struct PagerDevice
{
    CapabilityHandle Function;
    CapabilityHandle Region;
};

/// The scanout: the framebuffer's DeviceRegion, its size in pixels, its pitch in pixels and its
/// pixel format (Boot::BootDisplayFormat*); a zero region names none.
struct PagerScanout
{
    CapabilityHandle Region;
    Foundation::UInt32 Width;
    Foundation::UInt32 Height;
    Foundation::UInt32 Stride;
    Foundation::UInt32 Format;
};

/// The decoded start block (Abi/PagerBootstrap.hpp).
struct PagerBootstrap
{
    CapabilityHandle Endpoint;
    CapabilityHandle WaitSet;
    CapabilityHandle Diagnostic;
    CapabilityHandle ClientSpace;
    CapabilityHandle LazyMemory;
    CapabilityHandle LazyFrame;
    Foundation::UInt64 LazyStart;
    Foundation::UInt64 LazyPages;
    /// A ResourceDomain with Manage that ObjectCreate charges, or zero.
    CapabilityHandle Management;
    /// The grant's Abi::PagerManagement* features.
    Foundation::UInt64 ManagementFeatures;
    /// With Abi::PagerManagementMapping: this pager's own AddressSpace with Map, and a window of it
    /// nothing maps; otherwise zero.
    CapabilityHandle OwnSpace;
    Foundation::UInt64 WindowStart;
    Foundation::UInt64 WindowPages;
    /// With Abi::PagerManagementDevices: the device table, indexed by Abi::PagerDevice*, of
    /// assignable PCI functions and the DeviceRegions of their BARs; zero entries name none.
    PagerDevice Devices[Abi::PagerBootstrapDevices];
    PagerScanout Scanout;
    CapabilityHandle I8042[4U]{};
    Foundation::UInt64 I8042Declared{0U};
    CapabilityHandle InventoryFrame{};
    Foundation::UInt64 InventoryPages{0U};
    Foundation::UInt64 ModuleBase{0U};
    Foundation::UInt64 ModuleBytes{0U};
    Foundation::UInt64 ClockTicksPerMicrosecond{0U};
    Foundation::UInt64 WatchdogSeconds{Abi::WatchdogDefaultSeconds};
    Foundation::UInt64 SessionMode{0U};
    Foundation::UInt64 LoginEnabled{0U};
};

/// Where the pager's self-test threads start: an entry and a stack in the pager image, which the
/// platform's image supplies, how long the first thread waits to be preempted, in ticks of the
/// clock it reads, the entry of a second thread that does nothing but read page zero, a fault this
/// pager does not back, and, where the platform supervises other exceptions, the entry of a third
/// that executes an invalid instruction (or zero). Where the platform builds components in created
/// domains, the kernel-mapped module table and the clock ticks its first incarnation spins (or null
/// and zero). Makes code the root service copied through its window visible to instruction fetch
/// before it maps the pages executable: bytes from address in the window.
using ComponentCodeFlush = void (*)(Foundation::UInt64 address, Foundation::UInt64 bytes) noexcept;

/// The pager thread's monotonic ISA counter, scaled by bootstrap word 67.
using PagerThreadClock = Foundation::UInt64 (*)() noexcept;

/// What the root service needs to know of the ISA to build a component: where its segments may lie
/// and how its entry is named; the stack's first page and page count, with the start-block page
/// right above, whose address is the stack top and the thread's argument; how copied code reaches
/// the instruction cache (null where stores and fetch are coherent); and how long an incarnation
/// may run without a fault or an exit before the watchdog kills it, nonzero; where an interpreter
/// finds the foreign executable it is given, read-only, or zero where the platform launches no
/// interpreter; and the window a component maps its own memory into, or zero pages where no
/// component creates in its own domain.
struct ComponentPlatform
{
    Loader::MachineRules Image;
    Foundation::UInt64 StackStart;
    Foundation::UInt64 StackPages;
    ComponentCodeFlush FlushCode;
    Foundation::UInt64 WatchdogMicroseconds;
    Foundation::UInt64 ForeignImageAddress;
    Foundation::UInt64 WindowStart;
    Foundation::UInt64 WindowPages;
    /// The broker's scanout mapping in this ISA's user region.
    Foundation::UInt64 ScanoutAddress{Abi::ComponentScanoutAddress};
    PagerThreadClock Clock{nullptr};
};

struct ScenarioContext;
enum class PagerServeError : Foundation::UInt8;

/// A scenario a platform's root service runs once its incarnations ended (the services side by side
/// for concurrent services, the loader or driver recovery), given the scenarios' context, or null for
/// none.
using PagerServiceDemo = PagerServeError (*)(ScenarioContext& context, Abi::SystemCallError& systemCall) noexcept;

struct PagerThreadLaunch
{
    Foundation::UInt64 Entry;
    Foundation::UInt64 StackTop;
    Foundation::UInt64 SpinLimit;
    Foundation::UInt64 FaultEntry;
    Foundation::UInt64 ExceptionEntry;
    /// A thread that counts in the word its argument names forever, which the pager kills; zero
    /// where the kernel cannot kill.
    Foundation::UInt64 SpinningEntry;
    /// A thread that waits on the WaitSet its PagerWaitingThreadControl names; zero where created
    /// threads cannot wait.
    Foundation::UInt64 WaitingEntry;
    /// The immutable module table (ComponentImage.hpp), decoded from the
    /// kernel's start block by RunPager; null and zero before that decode.
    const Foundation::UInt8* ComponentImages;
    Foundation::Size ComponentImagesBytes;
    Foundation::UInt64 ComponentSpinTicks;
    ComponentPlatform Component;
    /// Where no component exercises the domain types, the cycles of a whole domain created and
    /// destroyed that the self-test runs; zero for none.
    Foundation::UInt64 DomainCycles;
    PagerServiceDemo ServiceDemo;
    /// The loader's demonstration, after the services', or null.
    PagerServiceDemo LoaderDemo;
    /// The driver's recovery, after its task and before the echo, or null.
    PagerServiceDemo DriverDemo;
    /// The AHCI driver's demonstration, after the driver's recovery, or null.
    PagerServiceDemo BlockDemo;
    /// The filesystem service's demonstration, right before the block demonstration, or null.
    PagerServiceDemo FsDemo;
    /// The network demonstration, after the block demonstration, or null.
    PagerServiceDemo NetDemo;
    PagerServiceDemo PortsDemo{nullptr};
    /// Hardware-free input demonstration, last, beside the display broker.
    PagerServiceDemo InputDemo{nullptr};
};

/// What the pager shares with its self-test thread, whose argument is the
/// block's address. The pager counts its ObjectDestroy calls that found the
/// thread still running in Polls; on one processor the thread sees Polls
/// advance only if it was preempted and the pager ran.
struct PagerThreadControl
{
    /// Accessed only through AtomicUInt64Reference; the pager publishes each poll.
    alignas(8) Foundation::UInt64 Polls;
    CapabilityHandle Diagnostic;
    Foundation::UInt64 SpinLimit;
};

/// The self-test thread's exit codes and lines (TAP comments): preempted with
/// its registers intact, preempted with its registers changed, or never
/// preempted within its limit.
inline constexpr Foundation::UInt64 PagerThreadExitCode{0x7E5U};
inline constexpr Foundation::UInt64 PagerThreadStateLostExitCode{0x7E6U};
inline constexpr Foundation::UInt64 PagerThreadTimeoutExitCode{0x7E7U};
inline constexpr char PagerThreadDoneLine[] = "# thread: done\n";
inline constexpr char PagerThreadStateLostLine[] = "# thread: extended state lost\n";
inline constexpr char PagerThreadTimeoutLine[] = "# thread: never preempted\n";

/// What the pager shares with its waiting thread, whose argument is the block's address: the
/// WaitSet it waits on, how long, and what the wait returned (an Abi::SystemCallError).
struct PagerWaitingThreadControl
{
    CapabilityHandle WaitSet;
    Foundation::UInt64 TimeoutMicroseconds;
    /// Published with a release store just before the thread waits; read with acquire.
    alignas(8) Foundation::UInt64 Waiting;
    /// The returned error is published with release and observed with acquire.
    alignas(8) Foundation::UInt64 Result;
};

/// The waiting thread's exit codes: its wait timed out, or it returned
/// otherwise.
inline constexpr Foundation::UInt64 PagerWaitingThreadExitCode{0x7E8U};
inline constexpr Foundation::UInt64 PagerWaitingThreadWokenExitCode{0x7E9U};

/// Whether the registers the thread's entry loaded still hold its pattern.
using PagerThreadStateCheck = bool (*)() noexcept;

enum class PagerServeError : Foundation::UInt8
{
    None,
    WaitFailed,
    UnexpectedReadiness,
    ReceiveFailed,
    MalformedMessage,
    /// FaultResolve Terminate of a fault outside the lazy region failed.
    TerminateFailed,
    ResolveFailed,
    CloseFailed,
    /// The object-management self-test failed; the outcome's system-call error is the failed
    /// call's, or None for a wrong result.
    ManagementFailed,
    /// A timed wait passed with nothing ready; nothing was served.
    TimedOut,
};

struct PagerServeOutcome
{
    PagerServeError Error;
    Abi::SystemCallError SystemCall;
    Foundation::UInt64 Resolved;
};

/// The boot pager: it backs a client's lazy region with one Frame, mapping each faulting page from
/// the matching page of that Frame. Each fault arrives as a pager-protocol message whose one
/// attachment is the FaultToken handle. As the boot domains' supervisor it ends a thread whose
/// fault lies outside the region with FaultResolve Terminate and keeps serving. The object owns its
/// delivery and request buffers, so it lives in static storage of the pager image; it is not
/// thread-safe.
class PagerProgram final
{
public:
    /// The largest Endpoint delivery: 48 header bytes and a 504-byte frame.
    static constexpr Foundation::Size DeliveryCapacityBytes{552U};

    /// ThreadLaunch, or null where the platform has no self-test thread.
    constexpr PagerProgram(const System::SystemCallClient& client, const PagerBootstrap& bootstrap,
                           const PagerThreadLaunch* const threadLaunch = nullptr) noexcept
        : m_client{client}, m_bootstrap{bootstrap}, m_threadLaunch{threadLaunch}
    {
    }

    /// Waits once for the readiness Notification, then receives and resolves
    /// faults until the Endpoint is empty. Stops at the first failure. A
    /// nonzero timeoutMicroseconds bounds the wait: TimedOut once it passed.
    [[nodiscard]] PagerServeOutcome ServeOnce(Foundation::UInt64 timeoutMicroseconds = 0U) noexcept;

    /// With a management grant, before serving: creates an Endpoint charged to the grant, loops one
    /// message through it, destroys it, checks that it no longer resolves and closes its handle,
    /// then creates and destroys a second one. With the grant's frame pool it then creates a Frame
    /// and a MemoryObject, destroys both, checks that they no longer resolve and closes their
    /// handles. With MemoryMap it then creates another pair, maps it at the start of its window,
    /// checks that the pages arrive zeroed, writes each page's Abi::MappedPagePattern and index and
    /// reads them back, unmaps the pair with MemoryUnmap and destroys it. With threads
    /// and a launch it then creates a thread in its own domain and polls ObjectDestroy until the
    /// thread exited with PagerThreadExitCode, counting the polls the thread waits for; then a
    /// second thread that faults at once, and serves faults between its ObjectDestroy calls until
    /// the thread it terminated is reclaimed with Abi::FaultTerminatedExitCode. Without a grant it
    /// does nothing. Resolved counts the objects created.
    [[nodiscard]] PagerServeOutcome TestManagement() noexcept;

    /// Gives the self-test its thread launch, or null for none.
    constexpr void SetThreadLaunch(const PagerThreadLaunch* const threadLaunch) noexcept
    {
        m_threadLaunch = threadLaunch;
    }

    /// The faults this pager resolved with Terminate.
    [[nodiscard]] constexpr Foundation::UInt64 GetTerminated() const noexcept
    {
        return m_terminated;
    }

    /// The faults of domains this pager created (a nonzero supervision tag) it terminated as
    /// crashes.
    [[nodiscard]] constexpr Foundation::UInt64 GetComponentCrashes() const noexcept
    {
        return m_componentCrashes;
    }

private:
    [[nodiscard]] PagerServeError ResolveDelivery(Foundation::Size sizeBytes,
                                                  Abi::SystemCallError& systemCall) noexcept;
    [[nodiscard]] PagerServeError Terminate(CapabilityHandle token, Abi::SystemCallError& systemCall) noexcept;
    [[nodiscard]] PagerServeError Close(CapabilityHandle token, Abi::SystemCallError& systemCall) noexcept;
    [[nodiscard]] PagerServeError TestEndpoint(bool loopback, Abi::SystemCallError& systemCall) noexcept;
    [[nodiscard]] PagerServeError TestMemory(Abi::SystemCallError& systemCall) noexcept;
    [[nodiscard]] PagerServeError TestMapping(Abi::SystemCallError& systemCall) noexcept;
    [[nodiscard]] PagerServeError TestThread(Abi::SystemCallError& systemCall) noexcept;
    [[nodiscard]] PagerServeError TestFaultingThread(Foundation::UInt64 entry,
                                                     Abi::SystemCallError& systemCall) noexcept;
    [[nodiscard]] PagerServeError TestKilledThread(Abi::SystemCallError& systemCall) noexcept;
    [[nodiscard]] PagerServeError TestWaitingThread(Abi::SystemCallError& systemCall) noexcept;
    /// A thread waiting on waitSet without a timeout, killed while it waits.
    [[nodiscard]] PagerServeError TestKilledWaiter(CapabilityHandle waitSet, Abi::SystemCallError& systemCall) noexcept;
    /// One domain: an AddressSpace, a CapabilitySpace and a ProtectionDomain
    /// tagged tag, a pair mapped into the space and unmapped, then all of it
    /// destroyed.
    [[nodiscard]] PagerServeError TestDomain(Foundation::UInt64 tag, Abi::SystemCallError& systemCall) noexcept;
    /// Creates an object of type from the first bytes of m_request.
    [[nodiscard]] PagerServeError CreateFromRequest(Foundation::UInt64 type, Foundation::Size bytes,
                                                    CapabilityHandle& handle,
                                                    Abi::SystemCallError& systemCall) noexcept;
    /// Maps the self-test's pages of memory over frame at the window's start
    /// in space with MemoryMap.
    [[nodiscard]] PagerServeError MapWindow(CapabilityHandle space, CapabilityHandle frame, CapabilityHandle memory,
                                            Abi::SystemCallError& systemCall) noexcept;
    /// Creates a thread at entry on the launch's stack with argument.
    [[nodiscard]] PagerServeError CreateThread(Foundation::UInt64 entry, Foundation::UInt64 argument,
                                               CapabilityHandle& thread, Abi::SystemCallError& systemCall) noexcept;
    /// Requires that thread's ObjectDestroy returned exitCode, that the handle
    /// no longer resolves, and closes it.
    [[nodiscard]] PagerServeError ReclaimThread(CapabilityHandle thread, const Abi::SystemCallResponse& destroyed,
                                                Foundation::UInt64 exitCode, Abi::SystemCallError& systemCall) noexcept;
    /// Creates a Frame and a MemoryObject of the self-test's size.
    [[nodiscard]] PagerServeError CreatePair(CapabilityHandle& frame, CapabilityHandle& memory,
                                             Abi::SystemCallError& systemCall) noexcept;
    /// Creates an object of type from m_memoryRequest into handle.
    [[nodiscard]] PagerServeError CreateMemory(Foundation::UInt64 type, CapabilityHandle& handle,
                                               Abi::SystemCallError& systemCall) noexcept;
    /// Destroys handle's object of type, requires that it no longer resolves
    /// and closes the handle.
    [[nodiscard]] PagerServeError DestroyAndClose(Foundation::UInt64 type, CapabilityHandle handle,
                                                  Abi::SystemCallError& systemCall) noexcept;
    [[nodiscard]] PagerServeError LoopBack(CapabilityHandle endpoint, Abi::SystemCallError& systemCall) noexcept;

    const System::SystemCallClient& m_client;
    const PagerBootstrap& m_bootstrap;
    const PagerThreadLaunch* m_threadLaunch;
    PagerThreadControl m_threadControl{};
    // The assembly thread increments atomically with release; the pager reads with acquire.
    alignas(8) Foundation::UInt64 m_spins{0U};
    PagerWaitingThreadControl m_waiting{};
    alignas(8) Foundation::UInt8 m_threadRequest[Abi::ObjectCreateThreadRequestSizeBytes]{};
    alignas(8) Foundation::UInt8 m_delivery[DeliveryCapacityBytes]{};
    alignas(8) Foundation::UInt8 m_request[Abi::FaultResolveRequestSizeBytes]{};
    alignas(8) Foundation::UInt8 m_endpointRequest[Abi::ObjectCreateEndpointRequestSizeBytes]{};
    alignas(8) Foundation::UInt8 m_memoryRequest[Abi::ObjectCreateMemoryObjectRequestSizeBytes]{};
    Foundation::UInt64 m_terminated{0U};
    Foundation::UInt64 m_componentCrashes{0U};
};

using System::BuildDefaultEndpointRequest;
using System::DefaultEndpointQueuedBytes;

/// Decodes the start block at words; returns false for a malformed block.
[[nodiscard]] bool DecodePagerBootstrap(const Foundation::UInt64 (&words)[Abi::PagerBootstrapWords],
                                        PagerBootstrap& output) noexcept;

/// Serves faults forever. A failure is reported once as a TAP bail-out line through the
/// DiagnosticChannel; the pager then stops serving. threadLaunch is the platform's self-test
/// thread, or null. The root service a platform's pager runs after its self-test, or null where the
/// pager builds no components. Its failure is reported once as a bail-out line of its own, and the
/// pager goes on serving faults.
using PagerRootService = PagerServeOutcome (*)(const System::SystemCallClient& client, const PagerBootstrap& bootstrap,
                                               PagerProgram& pager, const PagerThreadLaunch& launch) noexcept;

void RunPager(System::SystemCallTransport transport, const Foundation::UInt64 (&words)[Abi::PagerBootstrapWords],
              const PagerThreadLaunch* threadLaunch = nullptr, PagerRootService rootService = nullptr) noexcept;

/// The self-test thread's body, after its entry loaded its registers: waits,
/// bounded by control.SpinLimit ticks of clock, until the pager's Polls
/// advanced past what it read at its start, checks its registers with
/// stateIntact, writes its line and returns the code it exits with.
[[nodiscard]] Foundation::UInt64 RunPagerThread(System::SystemCallTransport transport, PagerThreadControl& control,
                                                PagerThreadStateCheck stateIntact, PagerThreadClock clock) noexcept;

/// The waiting thread's body: waits once on control's WaitSet for its
/// timeout, records the result and returns the code it exits with.
[[nodiscard]] Foundation::UInt64 RunWaitingThread(System::SystemCallTransport transport,
                                                  PagerWaitingThreadControl& control) noexcept;
} // Namespace Asteria::User::Pager

#endif // ASTERIA_USER_PAGER_PAGER_PROGRAM_HPP_INCLUDED
