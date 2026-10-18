// SPDX-License-Identifier: Apache-2.0

#include "User/Component/ComponentProgram.hpp"

#include "Abi/Clock.hpp"

#include "Abi/IpcCanonicalFrame.hpp"
#include "Abi/PagerProtocol.hpp"
#include "User/Component/AhciDriver.hpp"
#include "User/Component/InputBroker.hpp"
#include "User/Component/Ps2Driver.hpp"
#include "User/Component/XhciDriver.hpp"
#include "User/Component/DriverFrame.hpp"
#include "User/Component/DriverSetup.hpp"
#include "User/Component/EchoFrame.hpp"
#include "User/Component/FilesystemService.hpp"
#include "User/Component/NetworkService.hpp"
#include "User/Component/NicDriver.hpp"
#include "User/Component/ServiceCapabilities.hpp"
#include "User/System/AhciPort.hpp"
#include "User/System/DeviceAccess.hpp"
#include "User/System/E1000e.hpp"
#include "User/System/EduQuiesce.hpp"
#include "User/System/ObjectRequests.hpp"

namespace Asteria::User::Component
{
namespace
{
using System::CreateMemory;
using System::MapWindow;
using System::ReadRegister;

// The buffers a service receives into and builds its replies in: static, not on its stack
// (Source/User/System/AhciPort.hpp: components join the 512-byte stack budget), and inside one
// mapping of its space, its.bss, as the kernel's record-backed access requires. One incarnation
// runs one behaviour, so the echo and the driver service share the delivery buffer.
alignas(8) constinit Foundation::UInt8 g_delivery[EchoDeliveryHeaderBytes + EchoMaximumFrameBytes]{};
alignas(8) constinit Foundation::UInt8 g_reply[EchoMaximumFrameBytes]{};

// One echo request: answered on the reply capability it carries with the request itself, any other
// capability it carried moved back (WriteEchoReply). A request that is no echo frame with a reply
// Endpoint, or did not come through a capability sealed with EchoSealedSelector, is dropped
// unanswered: its reply capability cannot be trusted. False only when a Close failed other than on
// a stale handle.
[[nodiscard, gnu::noinline]] bool AnswerEcho(const System::SystemCallClient& client,
                                             const Foundation::UInt8* const delivery,
                                             const Foundation::Size deliveryBytes,
                                             Foundation::UInt8* const reply) noexcept
{
    const Foundation::Size replyBytes = WriteEchoReply(delivery, deliveryBytes, reply);
    if ((replyBytes == 0U) || (ReadEchoSealedSelector(delivery) != EchoSealedSelector))
    {
        return CloseAttachments(client, delivery, 0U);
    }
    const System::CapabilityHandle answer = ReadEchoHandle(delivery, 0U);
    // An unsent reply leaves the capabilities it would have moved back here.
    const bool sent =
        client
            .Send(answer, static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(reply)), replyBytes)
            .Error == Abi::SystemCallError::None;
    const bool returned = sent || CloseAttachments(client, delivery, 1U);
    return Closed(client, answer) && returned;
}

// A service: waits on its WaitSet for its Endpoint's readiness and answers every request until the
// Endpoint is empty, into and from the static buffers.
[[nodiscard]] Foundation::UInt64 Echo(const System::SystemCallClient& client,
                                      const Foundation::UInt64 (&startBlock)[Abi::ComponentBlockWords]) noexcept
{
    const System::CapabilityHandle service{startBlock[Abi::ComponentReceiveWord],
                                           startBlock[Abi::ComponentReceiveWord + 1U]};
    const System::CapabilityHandle waitSet{startBlock[Abi::ComponentWaitSetWord],
                                           startBlock[Abi::ComponentWaitSetWord + 1U]};
    for (;;)
    {
        const Abi::SystemCallResponse waited = client.Wait(waitSet, 0U);
        if ((waited.Error != Abi::SystemCallError::None) || (waited.Value0 != EchoWaitTag))
        {
            return Abi::ComponentEchoFailedExitCode;
        }
        for (;;)
        {
            const Abi::SystemCallResponse received =
                client.Receive(service, System::AddressOf(g_delivery), sizeof(g_delivery));
            if (received.Error == Abi::SystemCallError::WouldBlock)
            {
                break;
            }
            if ((received.Error != Abi::SystemCallError::None) ||
                !AnswerEcho(client, g_delivery, static_cast<Foundation::Size>(received.Value1), g_reply))
            {
                return Abi::ComponentEchoFailedExitCode;
            }
        }
    }
}
// A created supervisor receives on its own WaitSet and resolves the kernel-issued token from its
// own domain.
[[nodiscard, gnu::noinline]] Foundation::UInt64
FaultSupervisor(const System::SystemCallClient& client,
                const Foundation::UInt64 (&block)[Abi::ComponentBlockWords]) noexcept
{
    const System::CapabilityHandle account{block[Abi::ComponentAccountWord], block[Abi::ComponentAccountWord + 1U]};
    const System::CapabilityHandle report{block[Abi::ComponentReportWord], block[Abi::ComponentReportWord + 1U]};
    alignas(8) constinit static Foundation::UInt8 request[Abi::ObjectCreateProtectionDomainRequestSizeBytes]{};
    const auto created = client.CreateObject(account, Abi::ObjectTypeFaultChannel, System::AddressOf(request),
                                             Abi::ObjectCreateFaultChannelRequestSizeBytes);
    if (created.Error != Abi::SystemCallError::None)
    {
        return Abi::ComponentFaultSupervisorFailedExitCode | (1ULL << 32U);
    }
    const System::CapabilityHandle channel{created.Value0, created.Value1};
    const auto waited = client.CreateObject(account, Abi::ObjectTypeWaitSet, System::AddressOf(request),
                                            Abi::ObjectCreateWaitSetRequestSizeBytes);
    const System::CapabilityHandle waitSet{waited.Value0, waited.Value1};
    if (waited.Error != Abi::SystemCallError::None ||
        client.BindWaitSet(waitSet, channel, EchoWaitTag).Error != Abi::SystemCallError::None)
    {
        return Abi::ComponentFaultSupervisorFailedExitCode | (2ULL << 32U);
    }
    const EchoAttachment attachment{channel, EchoInspectRight | EchoManageRight, EchoCopy,
                                    static_cast<Foundation::UInt8>(Abi::ObjectTypeFaultChannel)};
    WriteEchoFrame(g_reply, &attachment, 1U);
    if (client.Send(report, System::AddressOf(g_reply), EchoReplyBytes).Error != Abi::SystemCallError::None)
    {
        return Abi::ComponentFaultSupervisorFailedExitCode | (3ULL << 32U);
    }
    const auto ready = client.Wait(waitSet, 0U);
    if (ready.Error != Abi::SystemCallError::None || ready.Value0 != EchoWaitTag)
    {
        return Abi::ComponentFaultSupervisorFailedExitCode | (4ULL << 32U);
    }
    const auto received = client.Receive(channel, System::AddressOf(g_delivery), sizeof(g_delivery));
    constexpr Foundation::Size payload{EchoDeliveryHeaderBytes + EchoHeaderBytes + EchoAttachmentBytes};
    constexpr Foundation::Size frame{EchoDeliveryHeaderBytes};
    if (received.Error != Abi::SystemCallError::None || received.Value1 != payload + Abi::PagerFaultPayloadBytes ||
        g_delivery[frame + 36U] != Abi::PagerProtocolMajor || g_delivery[frame + 38U] != Abi::PagerProtocolMinor ||
        g_delivery[frame + 81U] != 1U)
    {
        return Abi::ComponentFaultSupervisorFailedExitCode | (5ULL << 32U);
    }
    // Identity establishes the principal; its local creator tag is checked next.
    const auto identity = System::LoadWord(g_delivery + payload + Abi::PagerFaultIdentityOffset);
    if (identity == 0U ||
        System::LoadWord(g_delivery + payload + Abi::PagerFaultTagOffset) != block[Abi::ComponentIncarnationWord] + 1U)
    {
        return Abi::ComponentFaultSupervisorFailedExitCode | (6ULL << 32U);
    }
    const auto token = ReadEchoHandle(g_delivery, 0U);
    if (client.FaultTerminate(token).Error != Abi::SystemCallError::None)
    {
        return Abi::ComponentFaultSupervisorFailedExitCode | (7ULL << 32U);
    }
    return Abi::ComponentFaultSupervisorExitCode;
}

// The first word of each page an account incarnation maps into its window,
// plus the page's index.
constexpr Foundation::UInt64 WindowPattern{0x574F'4E50'4147'4500ULL};
// Sends the canonical asynchronous frame on the report Endpoint. The frame
// lies on the stack, inside one mapping of the component's space, as the
// Kernel's record-backed access requires.
[[nodiscard, gnu::noinline]] bool Report(const System::SystemCallClient& client,
                                         const Foundation::UInt64 (&startBlock)[Abi::ComponentBlockWords]) noexcept
{
    Foundation::UInt8 frame[sizeof(Abi::CanonicalAsynchronousFrame)]{};
    for (Foundation::Size index{0U}; index < sizeof(frame); ++index)
    {
        frame[index] = Abi::CanonicalAsynchronousFrame[index];
    }
    const Abi::SystemCallResponse sent = client.Send(
        System::CapabilityHandle{startBlock[Abi::ComponentReportWord], startBlock[Abi::ComponentReportWord + 1U]},
        System::AddressOf(frame), sizeof(frame));
    return sent.Error == Abi::SystemCallError::None;
}

// An account incarnation's exit code for the step that was not as its domain requires, with the
// error the kernel answered (Abi::ComponentAccountFailedExitCode).
[[nodiscard]] constexpr Foundation::UInt64 AccountFailure(const Foundation::UInt64 step,
                                                          const Abi::SystemCallError error) noexcept
{
    return Abi::ComponentAccountFailedExitCode | (step << 32U) | (static_cast<Foundation::UInt64>(error) << 40U);
}

// Creates an Endpoint, and a Frame of its window's pages with a MemoryObject over it, in its own
// domain, maps them into its window and writes and reads each page through the mapping; a page more
// than its domain's limit is refused (ResourceExhausted), and executable memory without Code is
// refused (RightsDenied). It destroys nothing: the drain that tears it down takes it all. Zero, or
// the failure's exit code.
[[nodiscard, gnu::noinline]] Foundation::UInt64
UseAccount(const System::SystemCallClient& client,
           const Foundation::UInt64 (&startBlock)[Abi::ComponentBlockWords]) noexcept
{
    const System::CapabilityHandle domain{startBlock[Abi::ComponentAccountWord],
                                          startBlock[Abi::ComponentAccountWord + 1U]};
    const System::CapabilityHandle space{startBlock[Abi::ComponentAccountSpaceWord],
                                         startBlock[Abi::ComponentAccountSpaceWord + 1U]};
    const Foundation::UInt64 start = startBlock[Abi::ComponentWindowStartWord];
    const Foundation::UInt64 pages = startBlock[Abi::ComponentWindowPagesWord];
    alignas(8) Foundation::UInt8 endpoint[Abi::ObjectCreateEndpointRequestSizeBytes]{};
    System::BuildDefaultEndpointRequest(endpoint);
    const Abi::SystemCallResponse queue =
        client.CreateObject(domain, Abi::ObjectTypeEndpoint, System::AddressOf(endpoint), sizeof(endpoint));
    if (queue.Error != Abi::SystemCallError::None)
    {
        return AccountFailure(Abi::ComponentAccountEndpointStep, queue.Error);
    }
    const Abi::SystemCallResponse frame = CreateMemory(client, domain, Abi::ObjectTypeFrame, pages);
    if (frame.Error != Abi::SystemCallError::None)
    {
        return AccountFailure(Abi::ComponentAccountFrameStep, frame.Error);
    }
    const Abi::SystemCallResponse memory = CreateMemory(client, domain, Abi::ObjectTypeMemoryObject, pages);
    if (memory.Error != Abi::SystemCallError::None)
    {
        return AccountFailure(Abi::ComponentAccountMemoryObjectStep, memory.Error);
    }
    const Abi::SystemCallError mapped = MapWindow(client, space, System::CapabilityHandle{memory.Value0, memory.Value1},
                                                  System::CapabilityHandle{frame.Value0, frame.Value1}, start, pages);
    if ((pages == 0U) || (mapped != Abi::SystemCallError::None))
    {
        return AccountFailure(Abi::ComponentAccountMapStep, mapped);
    }
    volatile Foundation::UInt64* const window =
        reinterpret_cast<volatile Foundation::UInt64*>(static_cast<Foundation::UIntPointer>(start));
    constexpr Foundation::UInt64 PageWords{4096U / sizeof(Foundation::UInt64)};
    for (Foundation::UInt64 page{0U}; page < pages; ++page)
    {
        window[page * PageWords] = WindowPattern + page;
    }
    bool held{true};
    for (Foundation::UInt64 page{0U}; page < pages; ++page)
    {
        held = held && (window[page * PageWords] == (WindowPattern + page));
    }
    if (!held)
    {
        return AccountFailure(Abi::ComponentAccountMemoryStep, Abi::SystemCallError::None);
    }
    const Abi::SystemCallResponse beyond = CreateMemory(client, domain, Abi::ObjectTypeFrame, 1U);
    if (beyond.Error != Abi::SystemCallError::ResourceExhausted)
    {
        return AccountFailure(Abi::ComponentAccountRefusalStep, beyond.Error);
    }
    alignas(8) Foundation::UInt8 executable[Abi::ObjectCreateMemoryObjectRequestSizeBytes]{};
    System::StoreWord(executable + Abi::ObjectCreatePagesOffset, 1U);
    executable[Abi::ObjectCreatePermissionsOffset] = 0x0DU; // Read, Execute, User.
    const Abi::SystemCallResponse code =
        client.CreateObject(domain, Abi::ObjectTypeMemoryObject, System::AddressOf(executable), sizeof(executable));
    if (code.Error != Abi::SystemCallError::RightsDenied)
    {
        return AccountFailure(Abi::ComponentAccountCodeRefusalStep, code.Error);
    }
    alignas(8) Foundation::UInt8 child[Abi::ObjectCreateResourceDomainExtendedRequestSizeBytes]{};
    System::StoreWord(child + Abi::ObjectCreateDomainFlagsOffset, Abi::ObjectCreateDomainFlagCode);
    const auto flagged =
        client.CreateObject(domain, Abi::ObjectTypeResourceDomain, System::AddressOf(child), sizeof(child));
    if (flagged.Error != Abi::SystemCallError::RightsDenied)
    {
        return AccountFailure(Abi::ComponentAccountCodeRefusalStep, flagged.Error);
    }
    alignas(8) Foundation::UInt8 waitSet[Abi::ObjectCreateWaitSetRequestSizeBytes]{};
    const auto limited =
        client.CreateObject(domain, Abi::ObjectTypeWaitSet, System::AddressOf(waitSet), sizeof(waitSet));
    return limited.Error == Abi::SystemCallError::ResourceExhausted
               ? 0U
               : AccountFailure(Abi::ComponentAccountRefusalStep, limited.Error);
}

// QEMU edu's registers, as 32-bit words of its BAR0: its identification, a liveness word that reads
// back the inverse of what was written, a factorial computed in the background while status bit 0
// is set, and the status.
constexpr Foundation::Size EduIdentificationWord{0x00U / 4U};
constexpr Foundation::Size EduLivenessWord{0x04U / 4U};
constexpr Foundation::Size EduFactorialWord{0x08U / 4U};
constexpr Foundation::Size EduStatusWord{0x20U / 4U};
constexpr Foundation::UInt32 EduIdentification{0x0100'00EDU};
constexpr Foundation::UInt32 EduComputing{0x01U};
constexpr Foundation::UInt32 LivenessProbe{0x5A5A'1234U};
constexpr Foundation::UInt32 FactorialInput{10U};
constexpr Foundation::UInt32 FactorialOutput{3'628'800U};

// Reads the device's identification and checks its liveness word, then
// computes a factorial, waiting at most the block's tick count for it.
[[nodiscard, gnu::noinline]] Foundation::UInt64 ExerciseDevice(volatile Foundation::UInt32* const registers,
                                                               const ComponentClock clock,
                                                               const Foundation::UInt64 ticks) noexcept
{
    if (ReadRegister(registers, EduIdentificationWord) != EduIdentification)
    {
        return DriverFailure(Abi::ComponentDriverIdentityStep, Abi::SystemCallError::None);
    }
    registers[EduLivenessWord] = LivenessProbe;
    if (ReadRegister(registers, EduLivenessWord) != static_cast<Foundation::UInt32>(~LivenessProbe))
    {
        return DriverFailure(Abi::ComponentDriverLivenessStep, Abi::SystemCallError::None);
    }
    registers[EduFactorialWord] = FactorialInput;
    const Foundation::UInt64 start = clock();
    while (((ReadRegister(registers, EduStatusWord) & EduComputing) != 0U) &&
           ((Abi::ElapsedClockTicks(clock(), start)) < ticks))
    {
    }
    return (((ReadRegister(registers, EduStatusWord) & EduComputing) == 0U) &&
            (ReadRegister(registers, EduFactorialWord) == FactorialOutput))
               ? 0U
               : DriverFailure(Abi::ComponentDriverFactorialStep, Abi::SystemCallError::None);
}

// The assignee may not make its device a bus master while no IommuDomain confines it, and may stop
// it being one, which writes the function's Command register. Zero, or the failure's exit code.
[[nodiscard, gnu::noinline]] Foundation::UInt64
ControlBusMaster(const System::SystemCallClient& client,
                 const Foundation::UInt64 (&startBlock)[Abi::ComponentBlockWords]) noexcept
{
    const System::CapabilityHandle function{startBlock[Abi::ComponentDeviceFunctionWord],
                                            startBlock[Abi::ComponentDeviceFunctionWord + 1U]};
    const Abi::SystemCallResponse enabled = client.ControlDevice(function, Abi::DeviceEnableBusMaster);
    if (enabled.Error != Abi::SystemCallError::InvalidConfiguration)
    {
        return DriverFailure(Abi::ComponentDriverBusMasterStep, enabled.Error);
    }
    const Abi::SystemCallResponse disabled = client.ControlDevice(function, Abi::DeviceDisableBusMaster);
    return (disabled.Error == Abi::SystemCallError::None)
               ? 0U
               : DriverFailure(Abi::ComponentDriverDisableStep, disabled.Error);
}

// QEMU edu's DMA engine: the source, destination and count at 0x80, 0x88 and 0x90 and the command
// at 0x98, whose bit 0 runs a transfer, bit 1 sends it from the device's buffer, at device address
// 0x40000, to memory, and bit 2 raises status bit 8 when it ends. A transfer completes 100 ms
// later.
constexpr Foundation::Size EduDmaSourceWord{0x80U / 4U};
constexpr Foundation::Size EduDmaDestinationWord{0x88U / 4U};
constexpr Foundation::Size EduDmaCountWord{0x90U / 4U};
constexpr Foundation::Size EduDmaCommandWord{0x98U / 4U};
constexpr Foundation::UInt32 EduDmaRun{1U};
constexpr Foundation::UInt32 EduDmaToMemory{2U};
constexpr Foundation::UInt32 EduDmaInterrupt{4U};
constexpr Foundation::UInt32 EduBuffer{0x4'0000U};
constexpr Foundation::UInt32 DmaBytes{256U};
constexpr Foundation::Size DmaWords{DmaBytes / sizeof(Foundation::UInt64)};
// edu's interrupt: its status at 0x24, which a write to 0x60
// Sets bits of, raising the interrupt, and a write to 0x64 clears.
constexpr Foundation::Size EduInterruptStatusWord{0x24U / 4U};
constexpr Foundation::Size EduInterruptRaiseWord{0x60U / 4U};
constexpr Foundation::Size EduInterruptAcknowledgeWord{0x64U / 4U};
constexpr Foundation::UInt32 EduRaisedStatus{0x0000'0001U};
constexpr Foundation::UInt32 EduDmaStatus{0x0000'0100U};
// The tag the driver binds its Interrupt under, and how long it waits for
// one: a transfer's 100 ms and the kernel's wake latency of up to a tick.
constexpr Foundation::UInt64 DriverInterruptTag{0x4544'5549U};
constexpr Foundation::UInt64 InterruptTimeoutMicroseconds{500'000U};
// The driver's two pages as the device sees them, below edu's 28-bit reach
constexpr Foundation::UInt64 SourceDeviceAddress{0x0010'0000U};
constexpr Foundation::UInt64 TargetDeviceAddress{0x0020'0000U};
constexpr Foundation::UInt64 FirstPattern{0x4449'5245'4354'0000ULL};
constexpr Foundation::UInt64 SecondPattern{0x5354'5241'5900'0000ULL};
constexpr Foundation::UInt64 PageBytes{4096U};

// Maps one page of frame for edu at address. The demonstration needs the
// device addresses it asks for, which only a translating IOMMU gives (edu is
// never trusted: its stray write must fault); any other answer is refused
// as InvalidConfiguration.
[[nodiscard, gnu::noinline]] Abi::SystemCallError MapForEdu(const System::SystemCallClient& client,
                                                            const System::CapabilityHandle iommu,
                                                            const System::CapabilityHandle frame,
                                                            const Foundation::UInt64 address,
                                                            const Foundation::UInt8 permissions) noexcept
{
    const DeviceMapping mapped = MapForDevice(client, iommu, frame, 1U, address, permissions);
    return ((mapped.Error == Abi::SystemCallError::None) && (mapped.Address != address))
               ? Abi::SystemCallError::InvalidConfiguration
               : mapped.Error;
}

// The driver's view of its device: its registers, and the WaitSet its
// Interrupt is bound to.
struct DeviceAccess
{
    volatile Foundation::UInt32* Registers;
    System::CapabilityHandle WaitSet;
    System::CapabilityHandle Interrupt;
};

// Waits for the device's interrupt, clears the status it raised and rearms
// the Interrupt. Whether the wait returned the Interrupt's tag and the
// device had raised status: the driver reads the device's status after
// each wake, since an edge that arrived while it was masked is delivered
// at the next arm.
[[nodiscard, gnu::noinline]] bool AwaitInterrupt(const System::SystemCallClient& client, const DeviceAccess& device,
                                                 const Foundation::UInt32 status) noexcept
{
    const Abi::SystemCallResponse waited = client.Wait(device.WaitSet, InterruptTimeoutMicroseconds);
    if ((waited.Error != Abi::SystemCallError::None) || (waited.Value0 != DriverInterruptTag))
    {
        return false;
    }
    const Foundation::UInt32 raised = ReadRegister(device.Registers, EduInterruptStatusWord);
    device.Registers[EduInterruptAcknowledgeWord] = raised;
    return ((raised & status) == status) &&
           (client.ControlInterrupt(device.Interrupt, Abi::InterruptRearm).Error == Abi::SystemCallError::None);
}

// Starts one transfer of DmaBytes that raises the interrupt when it ends.
void StartDma(volatile Foundation::UInt32* const registers, const Foundation::UInt64 source,
              const Foundation::UInt64 destination, const bool toMemory,
              const Foundation::UInt32 bytes = DmaBytes) noexcept
{
    System::OrderDmaStorage();
    registers[EduDmaSourceWord] = static_cast<Foundation::UInt32>(source);
    registers[EduDmaDestinationWord] = static_cast<Foundation::UInt32>(destination);
    registers[EduDmaCountWord] = bytes;
    registers[EduDmaCommandWord] = EduDmaRun | EduDmaInterrupt | (toMemory ? EduDmaToMemory : 0U);
}

// Runs one transfer and waits for the interrupt that ends it.
[[nodiscard, gnu::noinline]] bool EduDma(const System::SystemCallClient& client, const DeviceAccess& device,
                                         const Foundation::UInt64 source, const Foundation::UInt64 destination,
                                         const bool toMemory, const Foundation::UInt32 bytes = DmaBytes) noexcept
{
    StartDma(device.Registers, source, destination, toMemory, bytes);
    const bool completed = AwaitInterrupt(client, device, EduDmaStatus) &&
                           ((ReadRegister(device.Registers, EduDmaCommandWord) & EduDmaRun) == 0U);
    System::OrderDmaStorage();
    return completed;
}

void Fill(volatile Foundation::UInt64* const page, const Foundation::UInt64 pattern) noexcept
{
    for (Foundation::Size index{0U}; index < DmaWords; ++index)
    {
        page[index] = pattern + index;
    }
}

[[nodiscard]] bool Holds(const volatile Foundation::UInt64* const page, const Foundation::UInt64 pattern) noexcept
{
    bool held{true};
    for (Foundation::Size index{0U}; index < DmaWords; ++index)
    {
        held = held && (page[index] == (pattern + index));
    }
    return held;
}

// The faults the unit recorded against iommu's functions, or ~0 if the
// query failed.
[[nodiscard, gnu::noinline]] Foundation::UInt64 CountFaults(const System::SystemCallClient& client,
                                                            const System::CapabilityHandle iommu) noexcept
{
    const Abi::SystemCallResponse faults = client.ControlIommu(iommu, Abi::IommuFaults, 0U, 0U);
    return (faults.Error == Abi::SystemCallError::None) ? (faults.Value0 & 0xFFFF'FFFFU) : ~Foundation::UInt64{0U};
}

// Exactly `count` faults, the last a mapping fault at the target. QEMU's
// Amd-iommu leaves the event's address zero (Architecture/AmdVi.md), so zero is accepted here; the
// runner checks the exact address from the kernel's evidence for VT-d and virtio-iommu.
[[nodiscard, gnu::noinline]] bool MappingFault(const System::SystemCallClient& client,
                                               const System::CapabilityHandle iommu,
                                               const Foundation::UInt64 count) noexcept
{
    const auto faults = client.ControlIommu(iommu, Abi::IommuFaults, 0U, 0U);
    const Foundation::UInt64 reason = (faults.Value0 >> 48U) & 0xFFU;
    return (faults.Error == Abi::SystemCallError::None) && ((faults.Value0 & 0xFFFF'FFFFU) == count) &&
           ((reason == 5U) || (reason == 2U)) && ((faults.Value1 == TargetDeviceAddress) || (faults.Value1 == 0U));
}

// The DMA steps after the device is attached and its pages mapped: a copy
// through the device lands in the target page; once the target is unmapped
// from the device, the same copy faults and leaves the page as it was; mapped
// again, it lands. Zero, or the failure's exit code.
[[nodiscard, gnu::noinline]] Foundation::UInt64
CopyThroughDevice(const System::SystemCallClient& client, const System::CapabilityHandle iommu,
                  const System::CapabilityHandle target, const DeviceAccess& device,
                  volatile Foundation::UInt64* const sourcePage,
                  const volatile Foundation::UInt64* const targetPage) noexcept
{
    Fill(sourcePage, FirstPattern);
    if (!EduDma(client, device, SourceDeviceAddress, EduBuffer, false) ||
        !EduDma(client, device, EduBuffer, TargetDeviceAddress, true) || !Holds(targetPage, FirstPattern))
    {
        return DriverFailure(Abi::ComponentDriverDmaStep, Abi::SystemCallError::None);
    }
    const Foundation::UInt64 before = CountFaults(client, iommu);
    const Abi::SystemCallError unmapped = client.ControlIommu(iommu, Abi::IommuUnmap, TargetDeviceAddress, 0U).Error;
    if (unmapped != Abi::SystemCallError::None)
    {
        return DriverFailure(Abi::ComponentDriverUnmapStep, unmapped);
    }
    // The stray write: the device still names the target's old address. One
    // byte, so the write is one bus transaction and exactly one fault record.
    Fill(sourcePage, SecondPattern);
    if (!EduDma(client, device, SourceDeviceAddress, EduBuffer, false) ||
        !EduDma(client, device, EduBuffer, TargetDeviceAddress, true, 1U) || !Holds(targetPage, FirstPattern) ||
        !MappingFault(client, iommu, before + 1U))
    {
        return DriverFailure(Abi::ComponentDriverStrayStep, Abi::SystemCallError::None);
    }
    const Abi::SystemCallError remapped =
        MapForEdu(client, iommu, target, TargetDeviceAddress, Abi::IommuMapDeviceWrite);
    if ((remapped != Abi::SystemCallError::None) || !EduDma(client, device, EduBuffer, TargetDeviceAddress, true) ||
        !Holds(targetPage, SecondPattern))
    {
        return DriverFailure(Abi::ComponentDriverRemapStep, remapped);
    }
    return 0U;
}

// With its device mastering the bus, which its messages need, the driver creates a WaitSet and an
// Interrupt for the device's MSI in its own domain, binds it and arms it. Without an IOMMU the
// Interrupt takes the device's INTx line instead. The drain ends both. Zero, or the
// failure's exit code.
[[nodiscard, gnu::noinline]] Foundation::UInt64 ArmInterrupt(const System::SystemCallClient& client,
                                                             const System::CapabilityHandle domain,
                                                             const System::CapabilityHandle function,
                                                             DeviceAccess& device) noexcept
{
    alignas(8) Foundation::UInt8 request[Abi::ObjectCreateInterruptRequestSizeBytes]{};
    const Abi::SystemCallResponse waitSet = client.CreateObject(
        domain, Abi::ObjectTypeWaitSet, System::AddressOf(request), Abi::ObjectCreateWaitSetRequestSizeBytes);
    System::StoreHandle(request + Abi::ObjectCreateInterruptFunctionOffset, function);
    const Abi::SystemCallResponse interrupt =
        (waitSet.Error == Abi::SystemCallError::None)
            ? client.CreateObject(domain, Abi::ObjectTypeInterrupt, System::AddressOf(request), sizeof(request))
            : waitSet;
    if (interrupt.Error != Abi::SystemCallError::None)
    {
        return DriverFailure(Abi::ComponentDriverInterruptStep, interrupt.Error);
    }
    device.WaitSet = System::CapabilityHandle{waitSet.Value0, waitSet.Value1};
    device.Interrupt = System::CapabilityHandle{interrupt.Value0, interrupt.Value1};
    Abi::SystemCallError connected = client.BindWaitSet(device.WaitSet, device.Interrupt, DriverInterruptTag).Error;
    connected = (connected == Abi::SystemCallError::None)
                    ? client.ControlInterrupt(device.Interrupt, Abi::InterruptRearm).Error
                    : connected;
    return (connected == Abi::SystemCallError::None) ? 0U : DriverFailure(Abi::ComponentDriverInterruptStep, connected);
}

// Arms the device's interrupt and raises it, which must wake the driver.
// Zero, or the failure's exit code.
[[nodiscard, gnu::noinline]] Foundation::UInt64 ConnectInterrupt(const System::SystemCallClient& client,
                                                                 const System::CapabilityHandle domain,
                                                                 const System::CapabilityHandle function,
                                                                 DeviceAccess& device) noexcept
{
    const Foundation::UInt64 armed = ArmInterrupt(client, domain, function, device);
    if (armed != 0U)
    {
        return armed;
    }
    device.Registers[EduInterruptRaiseWord] = EduRaisedStatus;
    return AwaitInterrupt(client, device, EduRaisedStatus)
               ? 0U
               : DriverFailure(Abi::ComponentDriverRaiseStep, Abi::SystemCallError::None);
}

// The driver's DMA arrangement: an IommuDomain of its own domain with the device attached, and two
// pages of its own the device sees, the source readable and the target writable.
struct DmaSetup
{
    System::CapabilityHandle Iommu;
    System::CapabilityHandle Target;
    volatile Foundation::UInt64* SourcePage;
    const volatile Foundation::UInt64* TargetPage;
};

// Creates the IommuDomain, attaches the device, maps the two pages after the
// registers' in the window and for the device, and lets the device master
// the bus. Zero, or the failure's exit code.
[[nodiscard, gnu::noinline]] Foundation::UInt64
PrepareDma(const System::SystemCallClient& client, const Foundation::UInt64 (&startBlock)[Abi::ComponentBlockWords],
           DmaSetup& setup) noexcept
{
    const System::CapabilityHandle domain{startBlock[Abi::ComponentAccountWord],
                                          startBlock[Abi::ComponentAccountWord + 1U]};
    const System::CapabilityHandle space{startBlock[Abi::ComponentAccountSpaceWord],
                                         startBlock[Abi::ComponentAccountSpaceWord + 1U]};
    const System::CapabilityHandle function{startBlock[Abi::ComponentDeviceFunctionWord],
                                            startBlock[Abi::ComponentDeviceFunctionWord + 1U]};
    const Foundation::UInt64 start = startBlock[Abi::ComponentWindowStartWord];
    const Foundation::UInt64 confined = ConfineFunction(client, startBlock, setup.Iommu);
    if (confined != 0U)
    {
        return confined;
    }
    System::CapabilityHandle source{};
    Abi::SystemCallError mapped = MapDmaPage(client, domain, space, start + PageBytes, source);
    mapped = (mapped == Abi::SystemCallError::None)
                 ? MapDmaPage(client, domain, space, start + (2U * PageBytes), setup.Target)
                 : mapped;
    if (mapped != Abi::SystemCallError::None)
    {
        return DriverFailure(Abi::ComponentDriverPagesStep, mapped);
    }
    Abi::SystemCallError exposed = MapForEdu(client, setup.Iommu, source, SourceDeviceAddress, Abi::IommuMapDeviceRead);
    exposed = (exposed == Abi::SystemCallError::None)
                  ? MapForEdu(client, setup.Iommu, setup.Target, TargetDeviceAddress, Abi::IommuMapDeviceWrite)
                  : exposed;
    if (exposed != Abi::SystemCallError::None)
    {
        return DriverFailure(Abi::ComponentDriverDeviceMapStep, exposed);
    }
    const Abi::SystemCallError mastering = client.ControlDevice(function, Abi::DeviceEnableBusMaster).Error;
    if (mastering != Abi::SystemCallError::None)
    {
        return DriverFailure(Abi::ComponentDriverMasterStep, mastering);
    }
    setup.SourcePage =
        reinterpret_cast<volatile Foundation::UInt64*>(static_cast<Foundation::UIntPointer>(start + PageBytes));
    setup.TargetPage = reinterpret_cast<const volatile Foundation::UInt64*>(
        static_cast<Foundation::UIntPointer>(start + (2U * PageBytes)));
    return 0U;
}

// The driver confines its device in an IommuDomain of its own domain, maps two pages of its own for
// it, lets it master the bus, connects its interrupt, and checks the confinement with a stray
// write, waiting for each transfer's interrupt; then it stops the device and attempts detachment.
// AHCI block I/O: a translation-only domain keeps its requester until the owner's drain confirms it
// quiet. The drain takes the rest. Without an IOMMU it connects its device's INTx line and waits
// for one interrupt, then stops with DriverWithoutIommu.
[[nodiscard, gnu::noinline]] Foundation::UInt64
ConfineDevice(const System::SystemCallClient& client, const Foundation::UInt64 (&startBlock)[Abi::ComponentBlockWords],
              volatile Foundation::UInt32* const registers) noexcept
{
    const System::CapabilityHandle domain{startBlock[Abi::ComponentAccountWord],
                                          startBlock[Abi::ComponentAccountWord + 1U]};
    const System::CapabilityHandle function{startBlock[Abi::ComponentDeviceFunctionWord],
                                            startBlock[Abi::ComponentDeviceFunctionWord + 1U]};
    DmaSetup setup{.Iommu = {}, .Target = {}, .SourcePage = nullptr, .TargetPage = nullptr};
    DeviceAccess device{.Registers = registers, .WaitSet = {}, .Interrupt = {}};
    Foundation::UInt64 failure = PrepareDma(client, startBlock, setup);
    if (failure == DriverWithoutIommu)
    {
        const Foundation::UInt64 connected = ConnectInterrupt(client, domain, function, device);
        return (connected != 0U) ? connected : DriverWithoutIommu;
    }
    failure = (failure != 0U) ? failure : ConnectInterrupt(client, domain, function, device);
    failure = (failure != 0U)
                  ? failure
                  : CopyThroughDevice(client, setup.Iommu, setup.Target, device, setup.SourcePage, setup.TargetPage);
    if (failure != 0U)
    {
        return failure;
    }
    const Abi::SystemCallError stopped = client.ControlDevice(function, Abi::DeviceDisableBusMaster).Error;
    if (stopped != Abi::SystemCallError::None)
    {
        return DriverFailure(Abi::ComponentDriverReleaseStep, stopped);
    }
    const Abi::SystemCallError detached =
        client.ControlIommu(setup.Iommu, Abi::IommuDetach, function.Selector, function.Authenticator).Error;
    return ((detached == Abi::SystemCallError::None) || (detached == Abi::SystemCallError::WouldBlock))
               ? 0U
               : DriverFailure(Abi::ComponentDriverReleaseStep, detached);
}

// Once its device cannot master the bus, the driver maps its registers and drives it through them,
// polling, then confines its DMA. Zero, or the failure's exit code.
[[nodiscard, gnu::noinline]] Foundation::UInt64
DriveDevice(const System::SystemCallClient& client, const Foundation::UInt64 (&startBlock)[Abi::ComponentBlockWords],
            const ComponentClock clock) noexcept
{
    Foundation::UInt64 failure = ControlBusMaster(client, startBlock);
    failure = (failure != 0U) ? failure : MapRegisters(client, startBlock);
    failure = (failure != 0U) ? failure
                              : ExerciseDevice(RegistersOf(startBlock), clock, startBlock[Abi::ComponentSpinTicksWord]);
    return (failure != 0U) ? failure : ConfineDevice(client, startBlock, RegistersOf(startBlock));
}

// An instance that follows a dead one finds the device as the drain left it, bus mastering and
// messages off, but perhaps still running a transfer or a factorial the dead one started: nothing
// resets edu (function-level reset is deferred). Before the device may master the bus again, under
// which that transfer would use the new instance's mappings, the instance waits for it to go quiet
// (System::IsEduQuiet), reading it once every poll (an MMIO read takes the emulator's global lock,
// which edu's timer and factorial thread need too) for at most 64 spin periods, longer than a
// transfer; and it clears the interrupt status that work raised before it enables its interrupt.
// Returns the status it found, or ~0 if the device did not go quiet.
[[nodiscard, gnu::noinline]] Foundation::UInt64 QuiesceDevice(volatile Foundation::UInt32* const registers,
                                                              const ComponentClock clock,
                                                              const Foundation::UInt64 period) noexcept
{
    const Foundation::UInt64 poll = period >> 6U;
    const Foundation::UInt64 start = clock();
    Foundation::UInt64 polled = start;
    bool running = !System::IsEduQuiet(registers);
    for (Foundation::UInt64 now = start; running && ((Abi::ElapsedClockTicks(now, start)) < (period << 6U));
         now = clock())
    {
        if ((Abi::ElapsedClockTicks(now, polled)) >= poll)
        {
            running = !System::IsEduQuiet(registers);
            polled = now;
        }
    }
    if (!System::IsEduQuiet(registers))
    {
        return ~Foundation::UInt64{0U};
    }
    return System::AcknowledgeEdu(registers);
}

// What the driver service serves requests with: its device, its DMA pages,
// its generation (its tag), the interrupt status it found as it started,
// and how it crashes when a request injects that fault.
struct DriverService
{
    DeviceAccess Device;
    DmaSetup Dma;
    Foundation::UInt64 Generation;
    Foundation::UInt64 Found;
    ComponentCrash Crash;
    Foundation::UInt64 CrashKind;
};

// Sends the reply of kind to message's request on its reply capability.
[[nodiscard, gnu::noinline]] bool ReplyTo(const System::SystemCallClient& client, const DriverMessage& message,
                                          const DriverService& service, const Foundation::UInt64 kind) noexcept
{
    alignas(8) Foundation::UInt8 frame[DriverReplyBytes]{};
    const Foundation::UInt64 words[DriverPayloadWords]{message.Words[0U], message.Words[1U], kind, service.Found};
    const Foundation::Size bytes = WriteDriverFrame(frame, message.Transaction, words, nullptr);
    return client.Send(message.Reply, System::AddressOf(frame), bytes).Error == Abi::SystemCallError::None;
}

// The factorials a service computes on request, by argument; edu keeps
// a factorial in 32 bits.
struct Factorial
{
    Foundation::UInt64 Input;
    Foundation::UInt32 Output;
};

constexpr Factorial Factorials[]{{.Input = 10U, .Output = 3'628'800U}, {.Input = 12U, .Output = 479'001'600U}};

// A factorial request (on a device that may not master the bus: the instance asks edu for the
// factorial of the argument and for its interrupt when done, replies Started, waits for that
// interrupt, checks the result and replies Done; a request that injects the fault crashes the
// instance once the factorial runs. It stops asking for the interrupt afterwards, so no request is
// left to a Device Manager's quiesce (System::IsEduQuiet). An argument it has no result for is
// refused. Zero, or the exit code of a device that no longer behaves.
[[nodiscard, gnu::noinline]] Foundation::UInt64 ServeFactorial(const System::SystemCallClient& client,
                                                               const DriverMessage& message,
                                                               const DriverService& service) noexcept
{
    const Factorial* known{nullptr};
    for (const Factorial& candidate : Factorials)
    {
        known = (candidate.Input == message.Words[1U]) ? &candidate : known;
    }
    if (known == nullptr)
    {
        static_cast<void>(ReplyTo(client, message, service, DriverFailed));
        return 0U;
    }
    volatile Foundation::UInt32* const registers = service.Device.Registers;
    registers[EduStatusWord] = System::EduFactorialInterruptEnable;
    registers[EduFactorialWord] = static_cast<Foundation::UInt32>(known->Input);
    if ((message.Words[2U] & DriverCrashAfterStart) != 0U)
    {
        service.Crash(service.CrashKind);
        return DriverFailure(Abi::ComponentDriverDeviceStep, Abi::SystemCallError::None);
    }
    static_cast<void>(ReplyTo(client, message, service, DriverStarted));
    const bool computed = AwaitInterrupt(client, service.Device, System::EduFactorialRaised) &&
                          (ReadRegister(registers, EduFactorialWord) == known->Output);
    registers[EduStatusWord] = 0U;
    static_cast<void>(ReplyTo(client, message, service, computed ? DriverDone : DriverFailed));
    return computed ? 0U : DriverFailure(Abi::ComponentDriverDeviceStep, Abi::SystemCallError::None);
}

// One request. A request for another generation was sent to an
// instance that died, whose device was reset since: its effects are unknown,
// so the instance answers DriverRestarted and executes nothing, and the
// client decides. Otherwise it copies the pattern through the device, into
// the device's buffer and back into its target page, replying Started once
// the write-back runs and Done once it landed; a request that injects the
// fault crashes the instance as the write-back starts. A reply that cannot be
// sent is dropped, but a started transfer is still waited for. Zero, or the
// exit code of a device that no longer behaves (a transfer that does not end
// or does not land): its state is unknown and only its Device Manager can
// recover it.
[[nodiscard, gnu::noinline]] Foundation::UInt64
Serve(const System::SystemCallClient& client, const DriverMessage& message, const DriverService& service) noexcept
{
    if (message.Words[0U] != service.Generation)
    {
        static_cast<void>(ReplyTo(client, message, service, DriverRestarted));
        return 0U;
    }
    if (message.Method == DriverFactorialMethod)
    {
        return ServeFactorial(client, message, service);
    }
    // A copy needs DMA, which a device without an IOMMU may not do.
    if (service.Dma.SourcePage == nullptr)
    {
        static_cast<void>(ReplyTo(client, message, service, DriverFailed));
        return 0U;
    }
    Fill(service.Dma.SourcePage, message.Words[1U]);
    if (!EduDma(client, service.Device, SourceDeviceAddress, EduBuffer, false))
    {
        static_cast<void>(ReplyTo(client, message, service, DriverFailed));
        return DriverFailure(Abi::ComponentDriverDeviceStep, Abi::SystemCallError::None);
    }
    StartDma(service.Device.Registers, EduBuffer, TargetDeviceAddress, true);
    if ((message.Words[2U] & DriverCrashAfterStart) != 0U)
    {
        service.Crash(service.CrashKind);
        return DriverFailure(Abi::ComponentDriverDeviceStep, Abi::SystemCallError::None);
    }
    static_cast<void>(ReplyTo(client, message, service, DriverStarted));
    const bool landed = AwaitInterrupt(client, service.Device, EduDmaStatus) &&
                        ((ReadRegister(service.Device.Registers, EduDmaCommandWord) & EduDmaRun) == 0U) &&
                        Holds(service.Dma.TargetPage, message.Words[1U]);
    static_cast<void>(ReplyTo(client, message, service, landed ? DriverDone : DriverFailed));
    return landed ? 0U : DriverFailure(Abi::ComponentDriverDeviceStep, Abi::SystemCallError::None);
}

// Waits on the service's WaitSet and serves each request its Endpoint holds,
// until it is killed. A request that is neither a copy nor a factorial
// through the sealed capability with a reply Endpoint is dropped, every
// capability it carried closed. Returns only a fatal failure's exit code.
[[nodiscard, gnu::noinline]] Foundation::UInt64
ServeRequests(const System::SystemCallClient& client, const Foundation::UInt64 (&startBlock)[Abi::ComponentBlockWords],
              const DriverService& service) noexcept
{
    const System::CapabilityHandle endpoint{startBlock[Abi::ComponentReceiveWord],
                                            startBlock[Abi::ComponentReceiveWord + 1U]};
    const System::CapabilityHandle waitSet{startBlock[Abi::ComponentWaitSetWord],
                                           startBlock[Abi::ComponentWaitSetWord + 1U]};
    for (;;)
    {
        const Abi::SystemCallResponse waited = client.Wait(waitSet, 0U);
        if ((waited.Error != Abi::SystemCallError::None) || (waited.Value0 != DriverServiceWaitTag))
        {
            return DriverFailure(Abi::ComponentDriverServeStep, waited.Error);
        }
        for (;;)
        {
            const Abi::SystemCallResponse received =
                client.Receive(endpoint, System::AddressOf(g_delivery), sizeof(g_delivery));
            if (received.Error == Abi::SystemCallError::WouldBlock)
            {
                break;
            }
            if (received.Error != Abi::SystemCallError::None)
            {
                return DriverFailure(Abi::ComponentDriverServeStep, received.Error);
            }
            DriverMessage message{};
            const bool request = ReadDriverFrame(g_delivery, static_cast<Foundation::Size>(received.Value1), message) &&
                                 ((message.Method == DriverCopyMethod) || (message.Method == DriverFactorialMethod)) &&
                                 (ReadEchoSealedSelector(g_delivery) == DriverSealedSelector);
            const Foundation::UInt64 failure = request ? Serve(client, message, service) : 0U;
            if (failure != 0U)
            {
                return failure;
            }
            if (!CloseAttachments(client, g_delivery, 0U))
            {
                return DriverFailure(Abi::ComponentDriverServeStep, Abi::SystemCallError::None);
            }
        }
    }
}

// The driver as a service its Device Manager restarts. It maps its registers, quiesces whatever a
// dead instance left running, confines its DMA, connects its interrupt and serves copies and
// factorials until it is killed; without an IOMMU it serves factorials only. Returns only a
// failure's exit code.
[[nodiscard, gnu::noinline]] Foundation::UInt64
ServeDevice(const System::SystemCallClient& client, const Foundation::UInt64 (&startBlock)[Abi::ComponentBlockWords],
            const ComponentClock clock, const ComponentCrash crash) noexcept
{
    const System::CapabilityHandle domain{startBlock[Abi::ComponentAccountWord],
                                          startBlock[Abi::ComponentAccountWord + 1U]};
    const System::CapabilityHandle function{startBlock[Abi::ComponentDeviceFunctionWord],
                                            startBlock[Abi::ComponentDeviceFunctionWord + 1U]};
    const Foundation::UInt64 mapped = MapRegisters(client, startBlock);
    if (mapped != 0U)
    {
        return mapped;
    }
    DriverService service{.Device = {.Registers = RegistersOf(startBlock), .WaitSet = {}, .Interrupt = {}},
                          .Dma = {.Iommu = {}, .Target = {}, .SourcePage = nullptr, .TargetPage = nullptr},
                          .Generation = startBlock[Abi::ComponentIncarnationWord],
                          .Found = 0U,
                          .Crash = crash,
                          .CrashKind = startBlock[Abi::ComponentCrashKindWord]};
    service.Found = QuiesceDevice(service.Device.Registers, clock, startBlock[Abi::ComponentSpinTicksWord]);
    if ((service.Found == ~Foundation::UInt64{0U}) ||
        (ReadRegister(service.Device.Registers, EduIdentificationWord) != EduIdentification))
    {
        return DriverFailure(Abi::ComponentDriverQuiesceStep, Abi::SystemCallError::None);
    }
    // Without an IOMMU the service copies nothing and computes factorials
    // through its registers and its INTx line.
    Foundation::UInt64 failure = PrepareDma(client, startBlock, service.Dma);
    if (failure == DriverWithoutIommu)
    {
        failure = ArmInterrupt(client, domain, function, service.Device);
        return (failure != 0U) ? failure : ServeRequests(client, startBlock, service);
    }
    failure = (failure != 0U) ? failure : ConnectInterrupt(client, domain, function, service.Device);
    return (failure != 0U) ? failure : ServeRequests(client, startBlock, service);
}

// Compiled floating point and integer division (IA-64 divides in f6-f15) for the block's tick
// count, longer than a time slice, so the thread is preempted mid-computation. The step comes from
// the block, which the compiler cannot fold; halves add exactly, and each remainder is computed two
// ways. The clock is read every 16 steps, so the computation ends soon after its ticks even where
// emulated divisions are slow.
[[gnu::noinline]] bool Compute(const ComponentClock clock,
                               const Foundation::UInt64 (&startBlock)[Abi::ComponentBlockWords]) noexcept
{
    const double step = 1.0 / static_cast<double>(2U + (startBlock[Abi::ComponentIncarnationWord] >> 60U));
    double sum{0.0};
    Foundation::UInt64 additions{0U};
    Foundation::UInt64 mismatches{0U};
    const Foundation::UInt64 start = clock();
    while ((Abi::ElapsedClockTicks(clock(), start)) < startBlock[Abi::ComponentSpinTicksWord])
    {
        for (Foundation::UInt64 index{0U}; index < 16U; ++index)
        {
            sum += step;
            const Foundation::UInt64 dividend = additions + (index << 3U);
            const Foundation::UInt64 divisor = index + 3U + (additions & 7U);
            mismatches += ((dividend - ((dividend / divisor) * divisor)) != (dividend % divisor)) ? 1U : 0U;
        }
        additions += 16U;
    }
    return (mismatches == 0U) && (additions != 0U) && (sum == (static_cast<double>(additions) * step));
}
} // Namespace

Foundation::UInt64 RunComponent(const System::SystemCallTransport transport,
                                const Foundation::UInt64 (&startBlock)[Abi::ComponentBlockWords],
                                const ComponentClock clock, const ComponentCrash crash) noexcept
{
    if (startBlock[Abi::ComponentStartMagicWord] != Abi::ComponentStartMagic)
    {
        return Abi::ComponentBadStartExitCode;
    }
    const Foundation::UInt64 behavior = startBlock[Abi::ComponentBehaviorWord];
    if (behavior == Abi::ComponentBehaviorCrash)
    {
        // CPU-bound for longer than a time slice, so the root service runs
        // meanwhile only through preemption.
        const Foundation::UInt64 start = clock();
        while ((Abi::ElapsedClockTicks(clock(), start)) < startBlock[Abi::ComponentSpinTicksWord])
        {
        }
        crash(startBlock[Abi::ComponentCrashKindWord]);
        return Abi::ComponentCrashReturnedExitCode;
    }
    if (behavior == Abi::ComponentBehaviorHang)
    {
        // Never reports, faults or exits: only its supervisor's watchdog ends it.
        for (;;)
        {
            static_cast<void>(clock());
        }
    }
    if (behavior == Abi::ComponentBehaviorEcho)
    {
        const System::SystemCallClient client{transport};
        return Echo(client, startBlock);
    }
    if (behavior == Abi::ComponentBehaviorDriver)
    {
        const System::SystemCallClient client{transport};
        const Foundation::UInt64 failure = DriveDevice(client, startBlock, clock);
        if ((failure != 0U) && (failure != DriverWithoutIommu))
        {
            return failure;
        }
        const Foundation::UInt64 driven =
            (failure == 0U) ? Abi::ComponentDriverExitCode : Abi::ComponentDriverRegistersExitCode;
        return Report(client, startBlock) ? driven : Abi::ComponentReportFailedExitCode;
    }
    if (behavior == Abi::ComponentBehaviorDriverService)
    {
        const System::SystemCallClient client{transport};
        return ServeDevice(client, startBlock, clock, crash);
    }
    if (behavior == Abi::ComponentBehaviorBlockDriver)
    {
        const System::SystemCallClient client{transport};
        const AhciDriverPlatform platform{.Clock = clock, .Crash = crash, .WriteRegister = System::WriteAhciRegister};
        return ServeAhciDevice(client, startBlock, platform);
    }
    if (behavior == Abi::ComponentBehaviorNicDriver)
    {
        const System::SystemCallClient client{transport};
        const NicDriverPlatform platform{.Clock = clock,
                                         .Crash = crash,
                                         .ReadRegister = System::ReadE1000eRegister,
                                         .WriteRegister = System::WriteE1000eRegister};
        return ServeNicDevice(client, startBlock, platform);
    }
    if (behavior == Abi::ComponentBehaviorXhci)
    {
        const System::SystemCallClient client{transport};
        return ServeXhciDevice(client, startBlock, clock);
    }
    if (behavior == Abi::ComponentBehaviorPs2)
    {
        const System::SystemCallClient client{transport};
        return ServePs2(client, startBlock, clock);
    }
    if (behavior == Abi::ComponentBehaviorInput)
    {
        const System::SystemCallClient client{transport};
        return ServeInput(client, startBlock);
    }
    if (behavior == Abi::ComponentBehaviorInputClient)
    {
        const System::SystemCallClient client{transport};
        return ServeInputClient(client, startBlock);
    }
    if (behavior == Abi::ComponentBehaviorFilesystem)
    {
        const System::SystemCallClient client{transport};
        const FilesystemPlatform platform{.Crash = crash, .Window = startBlock[Abi::ComponentWindowStartWord]};
        return ServeFilesystem(client, startBlock, platform);
    }
    if (behavior == Abi::ComponentBehaviorNetwork)
    {
        const System::SystemCallClient client{transport};
        const NetworkPlatform platform{.Window = startBlock[Abi::ComponentWindowStartWord]};
        return ServeNetwork(client, startBlock, platform);
    }
    if (behavior == Abi::ComponentBehaviorFaultSupervisor)
    {
        const System::SystemCallClient client{transport};
        return FaultSupervisor(client, startBlock);
    }
    if (behavior == Abi::ComponentBehaviorAccount)
    {
        const System::SystemCallClient client{transport};
        const Foundation::UInt64 failure = UseAccount(client, startBlock);
        if (failure != 0U)
        {
            return failure;
        }
        return Report(client, startBlock) ? Abi::ComponentAccountExitCode : Abi::ComponentReportFailedExitCode;
    }
    if (behavior != Abi::ComponentBehaviorReport)
    {
        return Abi::ComponentBadStartExitCode;
    }
    if (!Compute(clock, startBlock))
    {
        return Abi::ComponentArithmeticFailedExitCode;
    }
    const System::SystemCallClient client{transport};
    return Report(client, startBlock) ? Abi::ComponentExitCode : Abi::ComponentReportFailedExitCode;
}
} // Namespace Asteria::User::Component
