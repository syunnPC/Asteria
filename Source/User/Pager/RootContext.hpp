// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_PAGER_ROOT_CONTEXT_HPP_INCLUDED
#define ASTERIA_USER_PAGER_ROOT_CONTEXT_HPP_INCLUDED

#include "Abi/ComponentBootstrap.hpp"
#include "Abi/PagerBootstrap.hpp"
#include "Abi/SystemCallAbi.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Ipc/MessageCodec.hpp"
#include "User/Pager/ComponentRecord.hpp"
#include "User/Component/DisplayFrame.hpp"
#include "User/Pager/ComponentSupervisor.hpp"
#include "User/Pager/DeviceManagerRole.hpp"
#include "User/Pager/ObjectCalls.hpp"
#include "User/Pager/PagerProgram.hpp"
#include "User/System/SystemCallClient.hpp"

namespace Asteria::User::Pager
{
/// The root service's mirror of its lines on the system console: its send capability on the display
/// broker's console Endpoint while the broker runs and mirroring holds, zero otherwise; the
/// transaction of its next frame; and whether a line was dropped, which stopped the mirroring.
/// PartialLine views the terminal's edited line the console shows after its last ended line
/// (PartialBytes long, zero when the cursor starts a line); the terminal owns its bytes.
struct DisplayMirror
{
    System::CapabilityHandle Console;
    Foundation::UInt64 Transaction;
    bool Dropped;
    const ComponentRecord* Broker{nullptr};
    Foundation::UInt64 ReportedSlowestMicroseconds{0U};
    Foundation::UInt64 LastMetricsCounter{0U};
    const char* PartialLine{nullptr};
    Foundation::Size PartialBytes{0U};
};

/// Scenario access to service interfaces, images, Endpoints and shared buffers.
/// The context lives in static pager storage and is not thread-safe.
struct ScenarioContext
{
    const System::SystemCallClient* Client;
    const PagerBootstrap* Bootstrap{nullptr};
    void (*ConsoleObserver)(void*, const char*, Foundation::Size) noexcept {nullptr};
    void* ConsoleObserverContext{nullptr};
    /// The grant, charged with the root service's own objects, and the
    /// DiagnosticChannel the scenarios write their lines through.
    System::CapabilityHandle Management;
    System::CapabilityHandle Diagnostic;
    /// Where the lines also go to the screen; the root service's, so that a
    /// scenario holding the context read-only still mirrors.
    DisplayMirror* Display;
    /// The launch: its table of images and its component platform.
    const PagerThreadLaunch* Launch;
    ObjectCalls* Calls;
    ComponentSupervisor* Supervisor;
    const DeviceManagerRole* Devices;
    /// Counted inventory text, formatted once by the product inventory reader.
    const char* DeviceText{nullptr};
    Foundation::Size DeviceTextBytes{0U};
    /// The `component` image of the launch's table, and the `directory` and
    /// `client` images where the table has them.
    LoadedImage Component;
    LoadedImage Directory;
    LoadedImage ClientImage;
    /// The `usb` image (the USB host group driver), where the table has it.
    LoadedImage UsbHost;
    /// The separate broker image.
    LoadedImage DisplayImage;
    /// The Endpoint tasks report on, and the one the services the scenarios send to reply on, zero
    /// where components cannot wait: both unbound and polled, apart, so neither queue takes the
    /// other's messages.
    System::CapabilityHandle Report;
    System::CapabilityHandle Reply;
    /// The Directory's Endpoint while the Directory runs, which a client
    /// gets an Invoke copy of; zero otherwise.
    System::CapabilityHandle DirectoryEndpoint;
    /// The root service's own AddressSpace (Map) and the start of its
    /// window, where a scenario may map a page while no component is being
    /// built.
    System::CapabilityHandle OwnSpace;
    Foundation::UInt64 WindowStart;
    /// The driver the next client service is built on (the root service's
    /// send capability on a block or NIC driver's Endpoint and that
    /// instance's tag) and its flags, zero otherwise.
    System::CapabilityHandle DriverService;
    Foundation::UInt64 DriverGeneration;
    System::CapabilityHandle SessionInput{};
    Foundation::UInt64 SessionInputEpoch{0U};
    System::CapabilityHandle IdentityLimiter{};
    Foundation::UInt64 ServiceFlags;
    /// The tag the next component's domain gets.
    Foundation::UInt64 NextTag;
    /// Product init continues after optional input driver failures. Scenario
    /// links retain their own failure policy.
    bool OptionalInputDrivers{false};
    /// A message a scenario builds or parses, its wire frame, and the
    /// deliveries it receives.
    Kernel::Ipc::MessageEnvelope Message;
    /// Decoded display replies share this serialized pager-owned scratch.
    Component::DisplayMessage DecodedDisplayReply{};
    alignas(8) Foundation::UInt8 Frame[Kernel::Ipc::MessageCodec::MaximumWireSizeBytes];
    alignas(8) Foundation::UInt8 Delivery[PagerProgram::DeliveryCapacityBytes];
};

/// Prints one screen/debug line with the policy and startup/latest/slowest metrics.
/// Bounded, allocation-free; unknown timing is explicitly labelled.
[[gnu::noinline]] void WriteDisplayMetrics(const ScenarioContext& context, const Component::DisplayInfo& info) noexcept;

/// Fence on the existing reply Endpoint; prints pointer metadata only to the
/// DiagnosticChannel, so observations cannot damage the screen they describe.
/// Invoke Query replies include the cursor sequence; no endpoint is allocated.
[[nodiscard]] bool FencePointer(ScenarioContext& context, Foundation::UInt64& epoch, Foundation::UInt64& sequence,
                                Abi::SystemCallError& error) noexcept;
/// Installs an Invoke-only display route through the manager's input capability.
[[nodiscard]] bool SetPointerRoute(ScenarioContext& context, const ComponentRecord& input,
                                   Abi::SystemCallError& error) noexcept;

/// Starts a component of image and behavior (an interpreter given foreign)
/// in a free record under tag, linked to the context's report Endpoint and
/// Directory: the record, or null.
[[nodiscard, gnu::noinline]] ComponentRecord* StartComponent(ScenarioContext& context, const LoadedImage& image,
                                                             Foundation::UInt64 tag, Foundation::UInt64 behavior,
                                                             const ForeignImage& foreign,
                                                             Abi::SystemCallError& systemCall) noexcept;
[[nodiscard]] ComponentRecord* StartComponent(ScenarioContext& context, const LoadedImage& image,
                                              Foundation::UInt64 tag, Foundation::UInt64 behavior,
                                              Abi::SystemCallError& systemCall) noexcept;
/// As StartComponent, but the component's thread is left to ComponentSupervisor::Launch, so
/// requests sent to it meanwhile are queued before it runs.
[[nodiscard, gnu::noinline]] ComponentRecord* BuildComponent(ScenarioContext& context, const LoadedImage& image,
                                                             Foundation::UInt64 tag, Foundation::UInt64 behavior,
                                                             Abi::SystemCallError& systemCall) noexcept;
/// Steps until a message arrives on endpoint, into the context's delivery
/// buffer, while component runs: its size, or zero when none came.
[[nodiscard]] Foundation::UInt64 AwaitMessage(ScenarioContext& context, const ComponentRecord& component,
                                              System::CapabilityHandle endpoint,
                                              Abi::SystemCallError& systemCall) noexcept;
/// Writes line to the debug channel and, while the display broker runs, to the system console: a
/// send its queue refuses is retried after a pause that serves faults but reaps no exit, a bounded
/// number of times; a send still refused, or refused otherwise, stops the mirroring.
void WriteLine(const ScenarioContext& context, const char* line, Foundation::Size bytes) noexcept;
/// Writes terminal bytes to the display without the line-oriented diagnostic
/// channel. Uses the same bounded mirroring and fault-serving waits as WriteLine.
void WriteConsole(const ScenarioContext& context, const char* text, Foundation::Size bytes) noexcept;
/// WriteLine for a line that arrives while the terminal may hold a partial
/// edited line: the console erases that line, shows this one, then draws the
/// partial line again, so the screen keeps the debug channel's line order.
void WriteBetweenLines(const ScenarioContext& context, const char* line, Foundation::Size bytes) noexcept;
/// Writes which incarnation ended how, when not as its behavior asks.
void ReportUnexpectedEnd(const ScenarioContext& context, Foundation::UInt64 incarnation, Foundation::UInt64 exitCode,
                         bool crashed) noexcept;

} // Namespace Asteria::User::Pager

#endif // ASTERIA_USER_PAGER_ROOT_CONTEXT_HPP_INCLUDED
