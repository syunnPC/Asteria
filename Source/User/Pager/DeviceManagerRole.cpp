// SPDX-License-Identifier: Apache-2.0

#include "User/Pager/DeviceManagerRole.hpp"

#include "Abi/ComponentBootstrap.hpp"
#include "User/Input/Ps2Controller.hpp"
#include "User/System/AhciQuiesce.hpp"
#include "User/System/DeviceAccess.hpp"
#include "User/System/EduQuiesce.hpp"
#include "User/System/NicQuiesce.hpp"

namespace Asteria::User::Pager
{
namespace
{
// A driver's copies of its device: the region with Inspect, Map, Read and Write; the function with
// Inspect and Invoke. DeviceRegion and PciFunction are ObjectTypes 15 and 21.
constexpr Foundation::UInt16 DeviceRegionRights{0x001DU};
constexpr Foundation::UInt8 DeviceRegionType{15U};
constexpr Foundation::UInt16 DeviceFunctionRights{0x0003U};
constexpr Foundation::UInt8 DeviceFunctionType{21U};
struct Ps2QuietIo
{
    const System::SystemCallClient* Client;
    const System::CapabilityHandle* Ports;
    Abi::SystemCallError Error;
};
[[nodiscard]] bool ReadPs2(void* context, bool status, Foundation::UInt8& value) noexcept
{
    auto& io = *static_cast<Ps2QuietIo*>(context);
    const auto result = io.Client->PortIo(io.Ports[status ? 1U : 0U], 0U, 1U, 0U);
    io.Error = result.Error;
    value = static_cast<Foundation::UInt8>(result.Value0);
    return result.Error == Abi::SystemCallError::None;
}
[[nodiscard]] bool WritePs2(void* context, bool command, Foundation::UInt8 value) noexcept
{
    auto& io = *static_cast<Ps2QuietIo*>(context);
    io.Error = io.Client->PortIo(io.Ports[command ? 1U : 0U], 0U, 1U, 1U, value).Error;
    return io.Error == Abi::SystemCallError::None;
}
[[nodiscard]] bool DiscardPs2(void*, Foundation::UInt8, Foundation::UInt8) noexcept
{
    return true;
}
[[nodiscard]] constexpr bool IsXhci(const Foundation::Size device) noexcept
{
    return (device >= Abi::PagerDeviceXhci) && (device < Abi::PagerDeviceUsbHost);
}
[[nodiscard]] constexpr bool IsUsbHost(const Foundation::Size device) noexcept
{
    return (device >= Abi::PagerDeviceUsbHost) && (device < Abi::PagerBootstrapDevices);
}
} // Namespace

void DeviceManagerRole::UsbHostAccess::Configure(const System::SystemCallClient& client, const PagerDevice& device,
                                                 volatile Foundation::UInt32* const registers) noexcept
{
    m_client = &client;
    m_device = device;
    m_registers = registers;
}

Foundation::UInt32 DeviceManagerRole::UsbHostAccess::Read32(const Foundation::UInt32 offset) noexcept
{
    return (m_registers == nullptr) ? 0xFFFF'FFFFU : System::ReadRegister(m_registers, offset / 4U);
}

void DeviceManagerRole::UsbHostAccess::Write32(const Foundation::UInt32 offset, const Foundation::UInt32 value) noexcept
{
    if (m_registers != nullptr)
    {
        m_registers[offset / 4U] = value;
    }
}

// A refused port access reads all ones, which every recipe takes for an
// absent or unreachable controller.
Foundation::UInt16 DeviceManagerRole::UsbHostAccess::Read16(const Foundation::UInt32 offset) noexcept
{
    const Abi::SystemCallResponse read = m_client->PortIo(m_device.Region, offset, 2U, 0U);
    return (read.Error == Abi::SystemCallError::None) ? static_cast<Foundation::UInt16>(read.Value0) : 0xFFFFU;
}

void DeviceManagerRole::UsbHostAccess::Write16(const Foundation::UInt32 offset, const Foundation::UInt16 value) noexcept
{
    static_cast<void>(m_client->PortIo(m_device.Region, offset, 2U, 1U, value));
}

bool DeviceManagerRole::UsbHostAccess::ReadConfiguration(const Foundation::UInt32 offset,
                                                         Foundation::UInt32& value) noexcept
{
    const Abi::SystemCallResponse read =
        m_client->ControlDevice(m_device.Function, Abi::DeviceReadConfiguration, System::CapabilityHandle{offset, 0U});
    value = (read.Error == Abi::SystemCallError::None) ? static_cast<Foundation::UInt32>(read.Value0) : 0xFFFF'FFFFU;
    return read.Error == Abi::SystemCallError::None;
}

void DeviceManagerRole::Configure(const System::SystemCallClient& client,
                                  const PagerDevice (&devices)[Abi::PagerBootstrapDevices]) noexcept
{
    m_client = &client;
    for (Foundation::Size device{0U}; device < Abi::PagerBootstrapDevices; ++device)
    {
        m_devices[device] = Device{.Handles = devices[device], .Sequence = 0U, .Pending = false};
    }
}

bool DeviceManagerRole::QuiescePs2(const System::CapabilityHandle (&ports)[4U],
                                   Abi::SystemCallError& systemCall) const noexcept
{
    if (m_client == nullptr)
    {
        systemCall = Abi::SystemCallError::InvalidConfiguration;
        return false;
    }
    Ps2QuietIo adapter{m_client, ports, Abi::SystemCallError::None};
    Input::Ps2Controller controller{Input::Ps2Transport{
        .Context = &adapter, .Read = ReadPs2, .Write = WritePs2, .Observe = DiscardPs2, .Clock = nullptr}};
    const bool quiet = controller.Quiesce();
    systemCall = quiet ? Abi::SystemCallError::None
                       : (adapter.Error == Abi::SystemCallError::None ? Abi::SystemCallError::InvalidConfiguration
                                                                      : adapter.Error);
    return quiet;
}

bool DeviceManagerRole::IsPresent(const Foundation::Size device) const noexcept
{
    return (m_client != nullptr) && (device < Abi::PagerBootstrapDevices) &&
           (m_devices[device].Handles.Function.Selector != 0U) && (m_devices[device].Handles.Region.Selector != 0U);
}

bool DeviceManagerRole::Grant(const Foundation::Size device, const System::CapabilityHandle capabilities,
                              const System::CapabilityHandle domain, volatile Foundation::UInt64* const block,
                              Abi::SystemCallError& systemCall, const Foundation::Size regionWord) noexcept
{
    if (!IsPresent(device))
    {
        systemCall = Abi::SystemCallError::InvalidConfiguration;
        return false;
    }
    Device& granted = m_devices[device];
    const Abi::SystemCallResponse region = m_client->GrantCapability(
        granted.Handles.Region, capabilities, DeviceRegionRights, DeviceRegionType, Abi::CapabilityGrantCopy);
    const Abi::SystemCallResponse function = m_client->GrantCapability(
        granted.Handles.Function, capabilities, DeviceFunctionRights, DeviceFunctionType, Abi::CapabilityGrantCopy);
    if (!Check(region, systemCall) || !Check(function, systemCall))
    {
        return false;
    }
    const Abi::SystemCallResponse assigned =
        m_client->ControlDevice(granted.Handles.Function, Abi::DeviceAssign, domain);
    if (!Check(assigned, systemCall))
    {
        return false;
    }
    granted.Sequence = assigned.Value0;
    granted.Pending = true;
    block[regionWord] = region.Value0;
    block[regionWord + 1U] = region.Value1;
    block[regionWord + 2U] = function.Value0;
    block[regionWord + 3U] = function.Value1;
    return true;
}

bool DeviceManagerRole::BeginQuiesce(const Foundation::Size device, const System::CapabilityHandle authority,
                                     const System::CapabilityHandle space, const Foundation::UInt64 start,
                                     Abi::SystemCallError& systemCall) noexcept
{
    if (!IsPresent(device) || (m_devices[device].Sequence == 0U) || (m_frame.Selector != 0U))
    {
        systemCall = Abi::SystemCallError::InvalidConfiguration;
        return false;
    }
    m_quiescing = device;
    m_last = DeviceQuiesce{.Running = false,
                           .Outcome = DeviceQuiesceOutcome::Invalid,
                           .Kind = System::UsbHostKind::None,
                           .Reserved = 0U,
                           .Status = 0U,
                           .Polls = 0U};
    m_results[device] = m_last;
    if (IsUsbHost(device))
    {
        return BeginUsbHost(device, authority, space, start, systemCall);
    }
    Abi::SystemCallResponse frame{};
    m_pages = IsXhci(device) ? Abi::XhciRegisterWindowPages : 1U;
    for (; m_pages != 0U; m_pages >>= 1U)
    {
        frame = System::CreateDeviceFrame(*m_client, authority, m_devices[device].Handles.Region, m_pages);
        if (frame.Error != Abi::SystemCallError::InvalidArguments)
        {
            break;
        }
    }
    if (!Check(frame, systemCall))
    {
        return false;
    }
    m_frame = System::CapabilityHandle{frame.Value0, frame.Value1};
    const Abi::SystemCallResponse memory = System::CreateMemory(*m_client, authority, Abi::ObjectTypeMemoryObject,
                                                                m_pages, Abi::ObjectCreateCacheUncached);
    if (!Check(memory, systemCall))
    {
        static_cast<void>(Unmap(systemCall));
        return false;
    }
    m_memory = System::CapabilityHandle{memory.Value0, memory.Value1};
    const Abi::SystemCallError mapped = System::MapWindow(*m_client, space, m_memory, m_frame, start, m_pages);
    if (mapped != Abi::SystemCallError::None)
    {
        static_cast<void>(Unmap(systemCall));
        systemCall = mapped;
        return false;
    }
    m_space = space;
    m_start = start;
    volatile Foundation::UInt32* const registers = Registers();
    m_last.Outcome = DeviceQuiesceOutcome::TimedOut;
    if (IsXhci(device))
    {
        m_last.Running = System::BeginXhciQuiesce(registers, m_xhci, static_cast<Foundation::UInt32>(m_pages * 4096U));
        return true;
    }
    if (device == Abi::PagerDeviceNic)
    {
        m_last.Running = System::BeginNicQuiesce(registers);
        return true;
    }
    if (device != Abi::PagerDeviceAhci)
    {
        m_last.Running = !System::IsEduQuiet(registers);
        return true;
    }
    // One page of the ABAR holds the registers of ports 0 to 29; a controller
    // with others is never confirmed quiet.
    if ((System::ReadRegister(registers, System::AhciPiWord) >> 30U) != 0U)
    {
        static_cast<void>(Unmap(systemCall));
        systemCall = Abi::SystemCallError::InvalidConfiguration;
        return false;
    }
    System::BeginAhciQuiesce(registers, m_ahci);
    m_last.Running = m_ahci.Issued;
    return true;
}

// A USB host table function: its kind from its class dword, an EHCI's or
// OHCI's registers at their offset within the BAR's page (BAR 0's low bits;
// the kernel published the whole page, UsbHostPublication.hpp), a UHCI's
// ports through PortIo on its region, which needs no mapping.
bool DeviceManagerRole::BeginUsbHost(const Foundation::Size device, const System::CapabilityHandle authority,
                                     const System::CapabilityHandle space, const Foundation::UInt64 start,
                                     Abi::SystemCallError& systemCall) noexcept
{
    const PagerDevice& handles = m_devices[device].Handles;
    m_usbAccess.Configure(*m_client, handles, nullptr);
    Foundation::UInt32 identity{0U};
    Foundation::UInt32 bar{0U};
    if (!m_usbAccess.ReadConfiguration(0x08U, identity) || !m_usbAccess.ReadConfiguration(0x10U, bar))
    {
        m_results[device] = m_last;
        systemCall = Abi::SystemCallError::InvalidConfiguration;
        return false;
    }
    const System::UsbHostKind kind = System::UsbHostKindOf(identity);
    m_last.Kind = kind;
    Foundation::UInt32 bytes{0U};
    if ((kind == System::UsbHostKind::Ehci) || (kind == System::UsbHostKind::Ohci))
    {
        m_pages = 1U;
        const Abi::SystemCallResponse frame = System::CreateDeviceFrame(*m_client, authority, handles.Region, 1U);
        if (!Check(frame, systemCall))
        {
            m_results[device] = m_last;
            return false;
        }
        m_frame = System::CapabilityHandle{frame.Value0, frame.Value1};
        const Abi::SystemCallResponse memory =
            System::CreateMemory(*m_client, authority, Abi::ObjectTypeMemoryObject, 1U, Abi::ObjectCreateCacheUncached);
        if (!Check(memory, systemCall))
        {
            static_cast<void>(Unmap(systemCall));
            m_results[device] = m_last;
            return false;
        }
        m_memory = System::CapabilityHandle{memory.Value0, memory.Value1};
        const Abi::SystemCallError mapped = System::MapWindow(*m_client, space, m_memory, m_frame, start, 1U);
        if (mapped != Abi::SystemCallError::None)
        {
            static_cast<void>(Unmap(systemCall));
            systemCall = mapped;
            m_results[device] = m_last;
            return false;
        }
        m_space = space;
        m_start = start;
        const Foundation::UInt32 within = (bar & ~Foundation::UInt32{0xFU}) & 0xFFFU;
        bytes = 4096U - within;
        m_usbAccess.Configure(*m_client, handles, Registers() + within / 4U);
    }
    m_last.Running = System::BeginUsbHostQuiesce(m_usbAccess, kind, bytes, m_usbHost);
    if ((m_usbHost.Phase == System::UsbHostQuietPhase::Invalid) ||
        (m_usbHost.Phase == System::UsbHostQuietPhase::FirmwareOwned))
    {
        // Nothing was written: the quarantine stays, and no poll can help.
        m_last.Outcome = (m_usbHost.Phase == System::UsbHostQuietPhase::Invalid) ? DeviceQuiesceOutcome::Invalid
                                                                                 : DeviceQuiesceOutcome::FirmwareOwned;
        m_results[device] = m_last;
        static_cast<void>(Unmap(systemCall));
        systemCall = Abi::SystemCallError::InvalidConfiguration;
        return false;
    }
    m_last.Outcome = DeviceQuiesceOutcome::TimedOut;
    return true;
}

DeviceQuiescePoll DeviceManagerRole::PollQuiesce() noexcept
{
    ++m_last.Polls;
    volatile Foundation::UInt32* const registers = Registers();
    bool quiet{false};
    if (IsUsbHost(m_quiescing))
    {
        quiet = System::PollUsbHostQuiesce(m_usbAccess, m_usbHost);
        m_last.Outcome = (m_usbHost.Phase == System::UsbHostQuietPhase::FirmwareOwned)
                             ? DeviceQuiesceOutcome::FirmwareOwned
                             : m_last.Outcome;
    }
    else if (IsXhci(m_quiescing))
    {
        quiet = System::PollXhciQuiesce(registers, m_xhci);
    }
    else if (m_quiescing == Abi::PagerDeviceAhci)
    {
        quiet = System::PollAhciQuiesce(registers, m_ahci) == System::AhciQuiesceProgress::Quiet;
    }
    else
    {
        quiet = (m_quiescing == Abi::PagerDeviceNic) ? System::IsNicQuiet(registers) : System::IsEduQuiet(registers);
    }
    return quiet ? DeviceQuiescePoll::Quiet : DeviceQuiescePoll::Running;
}

bool DeviceManagerRole::FinishQuiesce(Abi::SystemCallError& systemCall) noexcept
{
    volatile Foundation::UInt32* const registers = Registers();
    if (IsUsbHost(m_quiescing))
    {
        if (!System::PollUsbHostQuiesce(m_usbAccess, m_usbHost))
        {
            m_results[m_quiescing] = m_last;
            systemCall = Abi::SystemCallError::InvalidConfiguration;
            return false;
        }
        m_last.Status = System::AcknowledgeUsbHost(m_usbAccess, m_usbHost);
    }
    else if (IsXhci(m_quiescing))
    {
        if (!System::PollXhciQuiesce(registers, m_xhci))
        {
            systemCall = Abi::SystemCallError::InvalidConfiguration;
            return false;
        }
        m_last.Status = System::ReadRegister(registers, m_xhci.Operational + 1U);
    }
    else if (m_quiescing == Abi::PagerDeviceAhci)
    {
        m_last.Status = System::AcknowledgeAhci(registers, m_ahci);
    }
    else
    {
        m_last.Status = (m_quiescing == Abi::PagerDeviceNic) ? System::AcknowledgeNic(registers)
                                                             : System::AcknowledgeEdu(registers);
    }
    // DeviceQuiesced names the drained assignment's sequence in argument 3.
    const Device& quiesced = m_devices[m_quiescing];
    const Abi::SystemCallResponse confirmed = m_client->ControlDevice(quiesced.Handles.Function, Abi::DeviceQuiesced,
                                                                      System::CapabilityHandle{quiesced.Sequence, 0U});
    const bool unmapped = Unmap(systemCall);
    const bool done = Check(confirmed, systemCall) && unmapped;
    m_devices[m_quiescing].Pending = m_devices[m_quiescing].Pending && (confirmed.Error != Abi::SystemCallError::None);
    m_last.Outcome = done ? DeviceQuiesceOutcome::Quiet : DeviceQuiesceOutcome::Invalid;
    m_results[m_quiescing] = m_last;
    return done;
}

void DeviceManagerRole::AbandonQuiesce() noexcept
{
    Abi::SystemCallError ignored{Abi::SystemCallError::None};
    static_cast<void>(Unmap(ignored));
    m_results[m_quiescing] = m_last;
}

const DeviceQuiesce& DeviceManagerRole::GetLastQuiesce() const noexcept
{
    return m_last;
}

const DeviceQuiesce& DeviceManagerRole::GetQuiesce(const Foundation::Size device) const noexcept
{
    return m_results[(device < Abi::PagerBootstrapDevices) ? device : 0U];
}

bool DeviceManagerRole::IsPending(const Foundation::Size device) const noexcept
{
    return IsPresent(device) && m_devices[device].Pending;
}

volatile Foundation::UInt32* DeviceManagerRole::Registers() const noexcept
{
    return reinterpret_cast<volatile Foundation::UInt32*>(static_cast<Foundation::UIntPointer>(m_start));
}

bool DeviceManagerRole::Unmap(Abi::SystemCallError& systemCall) noexcept
{
    bool done{true};
    if (m_space.Selector != 0U)
    {
        done = Check(m_client->UnmapMemory(m_space, m_start, m_pages), systemCall);
        m_space = {};
    }
    if (m_memory.Selector != 0U)
    {
        done = Check(m_client->DestroyObject(m_memory, Abi::ObjectTypeMemoryObject), systemCall) && done;
        m_memory = {};
    }
    if (m_frame.Selector != 0U)
    {
        done = Check(m_client->DestroyObject(m_frame, Abi::ObjectTypeFrame), systemCall) && done;
        m_frame = {};
    }
    return done;
}

bool DeviceManagerRole::Check(const Abi::SystemCallResponse& response, Abi::SystemCallError& systemCall) noexcept
{
    if (response.Error != Abi::SystemCallError::None)
    {
        systemCall = response.Error;
        return false;
    }
    return true;
}
} // Namespace Asteria::User::Pager
