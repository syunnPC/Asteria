// SPDX-License-Identifier: Apache-2.0

#include "User/Usb/Xhci.hpp"

namespace Asteria::User::Usb
{
namespace
{
constexpr UInt32 PortChanges{0x00FE0000U};
constexpr UInt32 PortWritable{0x0E00C200U}; // PP, PIC and wake enables; never PED/PR/LWS/RW1C.
constexpr UInt64 OperationTimeoutMs{1000U};
// USB 2.0 sections 7.1.7.3 (TATTDB) and 9.1.2: after an attach, wait 100 ms
// for the insertion to complete and device power to settle before reset.
constexpr UInt64 AttachDebounceMs{100U};
[[nodiscard]] bool RegisterRange(const UInt32 offset, const UInt32 bytes, const UInt32 barBytes) noexcept
{
    return (offset <= barBytes) && (bytes <= (barBytes - offset));
}
[[nodiscard]] bool RingMemory(const DmaMemory memory, const bool address64) noexcept
{
    return ValidMemory(memory, 256U, 64U, address64) && ((memory.Bytes % 16U) == 0U) &&
           ((memory.Address & 0xFFFFU) + memory.Bytes <= 65536U);
}
[[nodiscard]] UInt32 SpeedId(const Input::UsbSpeed speed) noexcept
{
    switch (speed)
    {
    case Input::UsbSpeed::Full:
        return 1U;
    case Input::UsbSpeed::Low:
        return 2U;
    case Input::UsbSpeed::High:
        return 3U;
    case Input::UsbSpeed::Super:
        return 4U;
    }
    return 0U;
}
[[nodiscard]] bool PacketSize(const Input::UsbSpeed speed, const UInt16 size) noexcept
{
    switch (speed)
    {
    case Input::UsbSpeed::Low:
        return size == 8U;
    case Input::UsbSpeed::Full:
        return (size == 8U) || (size == 16U) || (size == 32U) || (size == 64U);
    case Input::UsbSpeed::High:
        return size == 64U;
    case Input::UsbSpeed::Super:
        return size == 512U;
    }
    return false;
}
[[nodiscard]] DmaMemory Region(const ControllerMemory& memory, const Size index) noexcept
{
    switch (index)
    {
    case 0U:
        return memory.Dcbaa;
    case 1U:
        return memory.CommandRing;
    case 2U:
        return memory.EventRing;
    case 3U:
        return memory.Erst;
    default:
        break;
    }
    Size remaining = index - 4U;
    for (Size i{0U}; i < memory.PortCount; ++i)
    {
        const PortMemory& port = memory.Ports[i];
        if (remaining < 4U)
        {
            switch (remaining)
            {
            case 0U:
                return port.InputContext;
            case 1U:
                return port.DeviceContext;
            case 2U:
                return port.ControlRing;
            case 3U:
                return port.Buffer;
            default:
                break;
            }
        }
        remaining -= 4U;
        if (remaining < port.PipeCount * 2U)
        {
            const EndpointMemory& pipe = port.Pipes[remaining / 2U];
            return (remaining & 1U) == 0U ? pipe.Ring : pipe.Buffer;
        }
        remaining -= port.PipeCount * 2U;
    }
    return remaining == 0U ? memory.ScratchpadArray : memory.Scratchpads[remaining - 1U];
}
[[nodiscard]] bool Overlap(const DmaMemory left, const DmaMemory right) noexcept
{
    // DMA memory-owner boundary: integer CPU addresses only test mapping overlap;
    // no pointer is formed or dereferenced here. ValidMemory bounds device sums.
    const Foundation::UIntPointer leftCpu = reinterpret_cast<Foundation::UIntPointer>(left.Cpu);
    const Foundation::UIntPointer rightCpu = reinterpret_cast<Foundation::UIntPointer>(right.Cpu);
    const bool cpu = leftCpu <= rightCpu ? ((rightCpu - leftCpu) < left.Bytes) : ((leftCpu - rightCpu) < right.Bytes);
    return cpu || ((left.Address < right.Address + right.Bytes) && (right.Address < left.Address + left.Bytes));
}
} // Namespace

bool XhciController::Handoff(const UInt32 barBytes, const UInt32 pollLimit) noexcept
{
    if ((m_state != ControllerState::Uninitialized) || (barBytes < 32U) || (pollLimit == 0U) || (pollLimit > 100000U))
    {
        return false;
    }
    UInt32 offset = (m_mmio.Read32(16U) >> 16U) * 4U;
    bool legacy{false};
    for (Size count{0U}; (count < 64U) && (offset != 0U); ++count)
    {
        if ((offset < 32U) || !RegisterRange(offset, 4U, barBytes))
        {
            return false;
        }
        const UInt32 header = m_mmio.Read32(offset);
        if (header == 0xFFFF'FFFFU)
        {
            return false;
        }
        const UInt32 next = ((header >> 8U) & 0xFFU) * 4U;
        if ((header & 0xFFU) == 1U)
        {
            if (legacy || !RegisterRange(offset, 8U, barBytes) || ((next != 0U) && (next < 8U)))
            {
                return false;
            }
            legacy = true;
            // USBLEGSUP's two semaphores are independently writable. The MMIO
            // Accessor writes only the OS byte, never a sampled BIOS semaphore.
            m_mmio.WriteOsOwned(offset);
            if (!Wait(offset, 0x01010000U, 0x01000000U, pollLimit))
            {
                return false;
            }
            // Preserve reserved fields, clear SMI enables, acknowledge only
            // the three RW1C sources (ownership/PCI command/BAR changes).
            const UInt32 control = m_mmio.Read32(offset + 4U);
            if (control == 0xFFFF'FFFFU)
            {
                return false;
            }
            m_mmio.Write32(offset + 4U, (control & ~0xE000E011U) | 0xE0000000U);
            if ((m_mmio.Read32(offset + 4U) & 0xE011U) != 0U)
            {
                return false;
            }
        }
        if (next == 0U)
        {
            m_handoffBar = barBytes;
            return true;
        }
        if (next > barBytes - offset)
        {
            return false;
        }
        offset += next;
    }
    if (offset == 0U)
    {
        m_handoffBar = barBytes;
        return true;
    }
    return false;
}
bool XhciController::QuiesceFirmware(const UInt32 barBytes, const UInt32 pollLimit) noexcept
{
    if ((m_state != ControllerState::Uninitialized) || (m_handoffBar != barBytes) || (pollLimit == 0U) ||
        (pollLimit > 100000U))
    {
        return false;
    }
    const UInt32 cap = m_mmio.Read32(0U);
    const UInt32 operational = cap & 255U;
    if ((cap == 0xFFFF'FFFFU) || (operational < 32U) || ((operational & 3U) != 0U) ||
        !RegisterRange(operational, 8U, barBytes) || !Wait(operational + 4U, 0x800U, 0U, pollLimit))
    {
        return false;
    }
    const UInt32 command = m_mmio.Read32(operational);
    if (command == 0xFFFF'FFFFU)
    {
        return false;
    }
    m_mmio.Write32(operational, command & ~5U);
    if (!Wait(operational + 4U, 1U, 1U, pollLimit))
    {
        return false;
    }
    // XHCI 1.2 section 5.4.1: HCRST only while halted. Reset is required
    // before a trusted driver enables mastering.
    m_mmio.Write32(operational, 2U);
    return Wait(operational, 2U, 0U, pollLimit) && Wait(operational + 4U, 0x800U, 0U, pollLimit);
}
Size XhciController::DeviceIndex(const UInt8 number) const noexcept
{
    if ((number == 0U) || (number > m_portCount) || (m_roots[number - 1U].Device == 0U))
    {
        return MaximumDevices;
    }
    return m_roots[number - 1U].Device - 1U;
}
XhciController::Port& XhciController::Root(const UInt8 number) noexcept
{
    // Internal borrow: callers have an assigned root (DeviceIndex != sentinel).
    // The one-based index is written only from the validated device pool.
    return m_ports[m_roots[number - 1U].Device - 1U];
}
bool XhciController::ReleasePort(const UInt8 number) noexcept
{
    if (DeviceIndex(number) == MaximumDevices)
    {
        return false;
    }
    Port& port = Root(number);
    if (port.Connected || port.Detaching || (port.Slot != 0U))
    {
        return false;
    }
    for (Size index{0U}; index <= port.Memory.PipeCount; ++index)
    {
        const Port& pipe = Pipe(number, index);
        if (pipe.Busy || pipe.Ready)
        {
            return false;
        }
    }
    // Rings remain allocated and pinned; Disable Slot completed before reuse.
    port.RootNumber = 0U;
    port.PoweredOff = false;
    port.Change = false;
    m_roots[number - 1U].Device = 0U;
    return true;
}
XhciController::Port& XhciController::Pipe(const UInt8 number, const Size index) noexcept
{
    return index == 0U ? Root(number) : m_endpointPipes[DeviceIndex(number)][index - 1U];
}
XhciController::Port* XhciController::FindPipe(const UInt8 number, const UInt8 address) noexcept
{
    if ((number == 0U) || (number > m_portCount) || (DeviceIndex(number) == MaximumDevices))
    {
        return nullptr;
    }
    if (address == 0U)
    {
        return &Root(number);
    }
    for (Size i{0U}; i < Root(number).Memory.PipeCount; ++i)
    {
        Port& pipe = m_endpointPipes[DeviceIndex(number)][i];
        if (((pipe.Endpoint != 0U) || pipe.Ready) && (pipe.Descriptor.Address == address))
        {
            return &pipe;
        }
    }
    return nullptr;
}
XhciRing& XhciController::TransferRing(Port& port) noexcept
{
    if (port.ActiveEndpoint == 1U)
    {
        return port.Control;
    }
    if (port.RecoveryEndpoint != 0U)
    {
        // ClearHalt is owned by EP0; dequeue belongs to the target endpoint.
        for (Size i{0U}; i < m_slotCount; ++i)
        {
            if (m_ports[i].Slot == port.Slot)
            {
                for (Size j{0U}; j < m_ports[i].Memory.PipeCount; ++j)
                {
                    Port& pipe = m_endpointPipes[i][j];
                    if (pipe.Endpoint == port.ActiveEndpoint)
                    {
                        return pipe.Interrupt;
                    }
                }
            }
        }
    }
    return port.Interrupt;
}
void XhciController::RetirePipes(const UInt8 number, const Input::TransferStatus status) noexcept
{
    if (DeviceIndex(number) == MaximumDevices)
    {
        return;
    }
    for (Size i{0U}; i <= Root(number).Memory.PipeCount; ++i)
    {
        Port& pipe = Pipe(number, i);
        if (pipe.Busy)
        {
            Finish(pipe, status);
        }
    }
}
bool XhciController::ClearHalt(const UInt8 number, const UInt8 address, const UInt64 token) noexcept
{
    Port* pipe = FindPipe(number, address);
    if ((pipe == nullptr) || (pipe->Index == 0U) || !pipe->Configured || pipe->Busy || pipe->Ready)
    {
        return false;
    }
    Port& root = Root(number);
    Input::UsbRequest request{.Operation = Input::UsbOperation::Control,
                              .Token = token,
                              .Speed = root.Speed,
                              .Address = root.Request.Address,
                              .PacketSize0 = root.Request.PacketSize0,
                              .Setup = {.Bytes = {2U, 1U, 0U, 0U, address, 0U, 0U, 0U}},
                              .Endpoint = {},
                              .Data = nullptr,
                              .Length = 0U,
                              .Route = {},
                              .TimeoutMilliseconds = 1000U};
    if (!Submit(number, request))
    {
        return false;
    }
    root.RecoveryEndpoint = pipe->Endpoint;
    return true;
}
bool XhciController::Wait(const UInt32 offset, const UInt32 mask, const UInt32 value, const UInt32 limit) noexcept
{
    for (UInt32 i{0U}; i < limit; ++i)
    {
        const UInt32 read = m_mmio.Read32(offset);
        if (read == 0xFFFF'FFFFU)
        {
            return false;
        }
        if ((read & mask) == value)
        {
            return true;
        }
        m_mmio.PausePoll();
    }
    return false;
}
bool XhciController::Protocols(const UInt32 first, const UInt32 barBytes) noexcept
{
    UInt32 offset = first;
    for (Size count{0U}; (count < 64U) && (offset != 0U); ++count)
    {
        if (!RegisterRange(offset, 4U, barBytes))
        {
            return false;
        }
        const UInt32 header = m_mmio.Read32(offset);
        if ((header & 0xFFU) == 2U)
        {
            if (!RegisterRange(offset, 16U, barBytes) || (m_mmio.Read32(offset + 4U) != 0x20425355U))
            {
                return false;
            }
            const UInt32 ports = m_mmio.Read32(offset + 8U);
            const UInt32 start = ports & 0xFFU;
            const UInt32 length = (ports >> 8U) & 0xFFU;
            const UInt32 major = header >> 24U;
            const UInt32 speedCount = ports >> 28U;
            if (!RegisterRange(offset, 16U + speedCount * 4U, barBytes) || ((major != 2U) && (major != 3U)) ||
                (start == 0U) || (length == 0U) || (start > m_portCount) || (length > (m_portCount - start + 1U)))
            {
                return false;
            }
            // Protocol Slot Type occupies a reserved dword in the 0.96 profile.
            const UInt32 slotType = m_version >= 0x100U ? (m_mmio.Read32(offset + 12U) & 0x1FU) : 0U;
            for (UInt32 i{start}; i < start + length; ++i)
            {
                RootPort& root = m_roots[i - 1U];
                if (root.Protocol)
                {
                    return false;
                }
                root.Protocol = true;
                root.SuperPort = major == 3U;
                root.SlotType = static_cast<UInt8>(slotType);
                if (speedCount != 0U)
                {
                    for (Size id{0U}; id < 16U; ++id)
                    {
                        root.Speeds[id] = 0U;
                    }
                    for (UInt32 psiIndex{0U}; psiIndex < speedCount; ++psiIndex)
                    {
                        const UInt32 psi = m_mmio.Read32(offset + 16U + psiIndex * 4U);
                        const UInt32 id = psi & 15U;
                        const UInt32 exponent = (psi >> 4U) & 3U;
                        UInt64 rate = psi >> 16U;
                        for (UInt32 scale{0U}; scale < exponent; ++scale)
                        {
                            rate *= 1000U;
                        }
                        const UInt32 type = (psi >> 6U) & 3U;
                        if ((id == 0U) || (type == 1U) || (psi == 0xFFFF'FFFFU))
                        {
                            return false;
                        }
                        UInt8 decoded{0U};
                        if ((major == 3U) && (rate >= 5'000'000'000ULL) && (((psi >> 14U) & 3U) <= 1U))
                        {
                            decoded = static_cast<UInt8>(Input::UsbSpeed::Super) + 1U;
                        }
                        else if ((major == 2U) && (type == 0U) && ((psi & 0xC100U) == 0U))
                        {
                            switch (rate)
                            {
                            case 1'500'000U:
                                decoded = static_cast<UInt8>(Input::UsbSpeed::Low) + 1U;
                                break;
                            case 12'000'000U:
                                decoded = static_cast<UInt8>(Input::UsbSpeed::Full) + 1U;
                                break;
                            case 480'000'000U:
                                decoded = static_cast<UInt8>(Input::UsbSpeed::High) + 1U;
                                break;
                            default:
                                return false;
                            }
                        }
                        if (decoded == 0U)
                        {
                            return false;
                        }
                        if (type == 3U)
                        {
                            if ((psiIndex == 0U) || (root.Speeds[id] != decoded))
                            {
                                return false;
                            }
                            const UInt32 previous = m_mmio.Read32(offset + 12U + psiIndex * 4U);
                            if (((previous & 15U) != id) || (((previous >> 6U) & 3U) != 2U))
                            {
                                return false;
                            }
                        }
                        else
                        {
                            if (root.Speeds[id] != 0U)
                            {
                                return false;
                            }
                            if (type == 2U)
                            {
                                if (psiIndex + 1U >= speedCount)
                                {
                                    return false;
                                }
                                const UInt32 nextPsi = m_mmio.Read32(offset + 20U + psiIndex * 4U);
                                if (((nextPsi & 15U) != id) || (((nextPsi >> 6U) & 3U) != 3U))
                                {
                                    return false;
                                }
                            }
                            root.Speeds[id] = decoded;
                        }
                    }
                }
            }
        }
        const UInt32 next = ((header >> 8U) & 0xFFU) * 4U;
        if (((header & 0xFFU) == 2U) && (next != 0U) && (next < 16U + (m_mmio.Read32(offset + 8U) >> 28U) * 4U))
        {
            return false;
        }
        if (next == 0U)
        {
            return true;
        }
        if (next > (barBytes - offset))
        {
            return false;
        }
        offset += next;
    }
    return offset == 0U;
}
bool XhciController::Capabilities(const UInt32 barBytes) noexcept
{
    if (barBytes < 32U)
    {
        return false;
    }
    const UInt32 cap = m_mmio.Read32(0U);
    const UInt32 hcs = m_mmio.Read32(4U);
    const UInt32 hcs2 = m_mmio.Read32(8U);
    const UInt32 hcc = m_mmio.Read32(16U);
    m_operational = cap & 0xFFU;
    m_runtime = m_mmio.Read32(24U) & ~31U;
    m_doorbells = m_mmio.Read32(20U) & ~3U;
    const UInt32 ports = hcs >> 24U;
    if ((((cap >> 16U) != 0x96U) && ((cap >> 16U) < 0x100U)) || ((cap >> 16U) > 0x120U) || (m_operational < 32U) ||
        ((m_operational & 3U) != 0U) || (ports == 0U) || (ports > MaximumPorts) || ((hcs & 0xFFU) == 0U) ||
        (((hcs >> 8U) & 0x7FFU) == 0U) || !RegisterRange(m_operational, 0x400U + ports * 16U, barBytes) ||
        !RegisterRange(m_runtime, 64U, barBytes) || !RegisterRange(m_doorbells, ((hcs & 0xFFU) + 1U) * 4U, barBytes) ||
        (m_runtime < 32U) || (m_doorbells < 32U))
    {
        return false;
    }
    m_version = static_cast<UInt16>(cap >> 16U);
    m_portCount = static_cast<UInt8>(ports);
    m_maxSlots = static_cast<UInt8>(hcs & 0xFFU);
    m_address64 = (hcc & 1U) != 0U;
    m_contextBytes = (hcc & 4U) != 0U ? 64U : 32U;
    m_scratchpads = ((hcs2 >> 27U) & 31U) | (((hcs2 >> 21U) & 31U) << 5U);
    if ((m_scratchpads > MaximumScratchpads) || ((m_mmio.Read32(m_operational + 8U) & 0xFFFFU) != 1U))
    {
        return false;
    }
    for (Size i{0U}; i < m_portCount; ++i)
    {
        m_roots[i] = {};
        m_roots[i].Speeds[1U] = static_cast<UInt8>(Input::UsbSpeed::Full) + 1U;
        m_roots[i].Speeds[2U] = static_cast<UInt8>(Input::UsbSpeed::Low) + 1U;
        m_roots[i].Speeds[3U] = static_cast<UInt8>(Input::UsbSpeed::High) + 1U;
        m_roots[i].Speeds[4U] = static_cast<UInt8>(Input::UsbSpeed::Super) + 1U;
    }
    return Protocols((hcc >> 16U) * 4U, barBytes);
}
bool XhciController::ValidateMemory(const ControllerMemory& memory) const noexcept
{
    if ((memory.Ports == nullptr) || (memory.PortCount == 0U) || (memory.PortCount > MaximumDevices) ||
        (memory.PortCount > m_maxSlots) || !ValidMemory(memory.Dcbaa, (memory.PortCount + 1U) * 8U, 64U, m_address64) ||
        !RingMemory(memory.CommandRing, m_address64) || !RingMemory(memory.EventRing, m_address64) ||
        !ValidMemory(memory.Erst, 16U, 64U, m_address64) || (memory.ScratchpadCount != m_scratchpads))
    {
        return false;
    }
    if ((m_scratchpads != 0U) &&
        ((memory.Scratchpads == nullptr) || !ValidMemory(memory.ScratchpadArray, m_scratchpads * 8U, 64U, m_address64)))
    {
        return false;
    }
    for (Size i{0U}; i < memory.PortCount; ++i)
    {
        const PortMemory& port = memory.Ports[i];
        if (!ValidMemory(port.InputContext, 33U * m_contextBytes, 64U, m_address64) ||
            !ValidMemory(port.DeviceContext, 32U * m_contextBytes, 64U, m_address64) ||
            !RingMemory(port.ControlRing, m_address64) || !ValidMemory(port.Buffer, 1024U, 64U, m_address64) ||
            ((port.Buffer.Address & 0xFFFFU) + port.Buffer.Bytes > 65536U) || (port.Pipes == nullptr) ||
            (port.PipeCount == 0U) || (port.PipeCount > MaximumEndpointPipes))
        {
            return false;
        }
    }
    for (Size i{0U}; i < memory.PortCount; ++i)
    {
        for (Size j{0U}; j < memory.Ports[i].PipeCount; ++j)
        {
            const EndpointMemory& pipe = memory.Ports[i].Pipes[j];
            if (!RingMemory(pipe.Ring, m_address64) || !ValidMemory(pipe.Buffer, 1024U, 64U, m_address64) ||
                ((pipe.Buffer.Address & 0xFFFFU) + pipe.Buffer.Bytes > 65536U))
            {
                return false;
            }
        }
    }
    for (Size i{0U}; i < m_scratchpads; ++i)
    {
        if (!ValidMemory(memory.Scratchpads[i], 4096U, 4096U, m_address64) || (memory.Scratchpads[i].Bytes != 4096U))
        {
            return false;
        }
    }
    Size count = 4U + memory.PortCount * 4U + (m_scratchpads == 0U ? 0U : (1U + m_scratchpads));
    for (Size i{0U}; i < memory.PortCount; ++i)
    {
        count += memory.Ports[i].PipeCount * 2U;
    }
    for (Size i{0U}; i < count; ++i)
    {
        for (Size j{0U}; j < i; ++j)
        {
            if (Overlap(Region(memory, i), Region(memory, j)))
            {
                return false;
            }
        }
    }
    return true;
}
bool XhciController::Initialize(const ControllerMemory& memory, const UInt32 barBytes, const UInt32 pollLimit,
                                const EventDelivery delivery) noexcept
{
    // Reinitialization requires a fresh owner: pending completions must be drained.
    if ((m_state != ControllerState::Uninitialized) || (pollLimit == 0U) || (pollLimit > 100000U) ||
        ((m_handoffBar != barBytes) && !Handoff(barBytes, pollLimit)) || !Capabilities(barBytes) ||
        !ValidateMemory(memory))
    {
        return false;
    }
    m_pollLimit = pollLimit;
    m_delivery = delivery;
    m_memory = memory;
    m_slotCount = static_cast<UInt8>(memory.PortCount);
    m_state = ControllerState::Halted;
    if (!Wait(m_operational + 4U, 0x800U, 0U, pollLimit) || !Halt(pollLimit))
    {
        m_state = ControllerState::Faulted;
        return false;
    }
    m_mmio.Write32(m_operational, 2U);
    if (!Wait(m_operational, 2U, 0U, pollLimit) || !Wait(m_operational + 4U, 0x800U, 0U, pollLimit))
    {
        m_state = ControllerState::Faulted;
        return false;
    }
    Clear(memory.Dcbaa);
    Clear(memory.EventRing);
    Clear(memory.Erst);
    m_commands.Initialize(memory.CommandRing);
    if (m_scratchpads != 0U)
    {
        Clear(memory.ScratchpadArray);
        Store64(memory.Dcbaa.Cpu, memory.ScratchpadArray.Address);
        for (Size i{0U}; i < m_scratchpads; ++i)
        {
            Clear(memory.Scratchpads[i]);
            Store64(memory.ScratchpadArray.Cpu + i * 8U, memory.Scratchpads[i].Address);
        }
    }
    for (Size i{0U}; i < memory.PortCount; ++i)
    {
        Port& port = m_ports[i];
        port.Memory = memory.Ports[i];
        Clear(port.Memory.InputContext);
        Clear(port.Memory.DeviceContext);
        Clear(port.Memory.Buffer);
        port.Control.Initialize(port.Memory.ControlRing);
        for (Size j{0U}; j < port.Memory.PipeCount; ++j)
        {
            Port& pipe = m_endpointPipes[i][j];
            pipe.Memory = port.Memory;
            pipe.Memory.Buffer = port.Memory.Pipes[j].Buffer;
            pipe.Index = static_cast<UInt8>(j + 1U);
            pipe.Interrupt.Initialize(port.Memory.Pipes[j].Ring);
            Clear(pipe.Memory.Buffer);
        }
    }
    Store64(memory.Erst.Cpu, memory.EventRing.Address);
    Store32(memory.Erst.Cpu + 8U, static_cast<UInt32>(memory.EventRing.Bytes / 16U));
    m_mmio.PublishDma();
    m_mmio.Write64(m_operational + 0x30U, memory.Dcbaa.Address);
    m_mmio.Write64(m_operational + 0x18U, memory.CommandRing.Address | 1U);
    m_mmio.Write32(m_operational + 0x38U, m_slotCount);
    m_mmio.Write32(m_runtime + 0x28U, 1U);
    m_mmio.Write64(m_runtime + 0x30U, memory.Erst.Address);
    m_mmio.Write64(m_runtime + 0x38U, memory.EventRing.Address | 8U);
    m_mmio.Write32(m_runtime + 0x24U, 4000U); // 1ms moderation, 250ns units.
    const bool interrupts = m_delivery == EventDelivery::Interrupt;
    m_mmio.Write32(m_runtime + 0x20U, interrupts ? 3U : 1U); // IE and W1C IP.
    m_mmio.Write32(m_operational, interrupts ? 5U : 1U);     // Run and optional interrupt enable.
    if (!Wait(m_operational + 4U, 1U, 0U, pollLimit))
    {
        static_cast<void>(Halt(pollLimit));
        m_state = ControllerState::Faulted;
        return false;
    }
    m_state = ControllerState::Running;
    // Devices attached before start have had power since firmware or reset.
    m_initialScan = true;
    for (UInt32 rootNumber{1U}; rootNumber <= m_portCount; ++rootNumber)
    {
        const UInt8 i = static_cast<UInt8>(rootNumber);
        ScanPort(i);
    }
    m_initialScan = false;
    return true;
}
void XhciController::PowerPorts() noexcept
{
    if ((m_state != ControllerState::Running) || ((m_mmio.Read32(16U) & 8U) == 0U))
    {
        return;
    }
    for (UInt32 rootNumber{1U}; rootNumber <= m_portCount; ++rootNumber)
    {
        const UInt8 number = static_cast<UInt8>(rootNumber);
        const UInt32 status = m_mmio.Read32(PortOffset(number));
        if (((status & 8U) == 0U) && !m_roots[number - 1U].PoweredOff)
        {
            m_mmio.Write32(PortOffset(number), (status & PortWritable) | 0x200U);
        }
    }
}
bool XhciController::Halt(const UInt32 pollLimit) noexcept
{
    if ((m_state == ControllerState::Uninitialized) || (m_operational == 0U) || (pollLimit == 0U) ||
        (pollLimit > 100000U))
    {
        return false;
    }
    m_mmio.Write32(m_operational, m_mmio.Read32(m_operational) & ~5U);
    if (!Wait(m_operational + 4U, 1U, 1U, pollLimit))
    {
        m_state = ControllerState::Faulted;
        return false;
    }
    m_mmio.ObserveDma();
    m_state = ControllerState::Halted;
    m_commandPort = 0U;
    m_commandPointer = 0U;
    for (Size i{0U}; i < m_slotCount; ++i)
    {
        Port& port = m_ports[i];
        if (port.RootNumber == 0U)
        {
            continue;
        }
        RetirePipes(port.RootNumber,
                    port.Detaching ? Input::TransferStatus::Disconnected : Input::TransferStatus::Retryable);
        if (port.Detaching)
        {
            port.Connected = false;
            port.Change = true;
            port.Detaching = false;
        }
    }
    return true;
}
UInt32 XhciController::PortOffset(const UInt8 number) const noexcept
{
    return m_operational + 0x400U + UInt32{number - 1U} * 16U;
}
void XhciController::Record(const Input::UsbDiagnosticKind kind, const UInt8 port, const UInt8 detail, const UInt8 code,
                            const UInt32 value0, const UInt32 value1) noexcept
{
    if (m_diagnostics != nullptr)
    {
        m_diagnostics->Record({.Kind = kind,
                               .Port = port,
                               .Detail = detail,
                               .Code = code,
                               .Value0 = value0,
                               .Value1 = value1,
                               .Data = 0U});
    }
}
Input::PortStatus XhciController::GetPortStatus(const UInt8 number) noexcept
{
    if ((number == 0U) || (number > m_portCount) || (DeviceIndex(number) == MaximumDevices))
    {
        return {.Connected = false, .Speed = Input::UsbSpeed::Full};
    }
    const Port& port = Root(number);
    // Disconnect is visible only after Disable Slot completion or successful Halt.
    return {.Connected = port.Connected, .Speed = port.Speed};
}
void XhciController::ScanPort(const UInt8 number) noexcept
{
    RootPort& root = m_roots[number - 1U];
    const UInt32 status = m_mmio.Read32(PortOffset(number));
    if (status == 0xFFFFFFFFU)
    {
        return;
    }
    const bool attached = ((status & 1U) != 0U) && !root.Present;
    if (((status & 1U) != 0U) != root.Present)
    {
        root.Present = (status & 1U) != 0U;
        Record(root.Present ? Input::UsbDiagnosticKind::Connect : Input::UsbDiagnosticKind::Disconnect, number,
               root.SuperPort ? 3U : 2U, 0U, status);
    }
    if ((status & 8U) != 0U)
    {
        root.PoweredOff = true;
        m_mmio.Write32(PortOffset(number), (status & PortWritable & ~0x200U) | (status & PortChanges));
    }
    if ((root.Device == 0U) && ((status & 1U) != 0U) && !root.PoweredOff)
    {
        for (Size index{0U}; index < m_slotCount; ++index)
        {
            if (m_ports[index].RootNumber == 0U)
            {
                root.Device = static_cast<UInt8>(index + 1U);
                m_ports[index].RootNumber = number;
                m_ports[index].SuperPort = root.SuperPort;
                m_ports[index].SlotType = root.SlotType;
                // Provisional until the port is enabled; never a previous
                // occupant's speed, which would select its reset policy.
                m_ports[index].Speed = root.SuperPort ? Input::UsbSpeed::Super : Input::UsbSpeed::Full;
                m_ports[index].SpeedIdentifier = 0U;
                m_settling[index] = !m_initialScan;
                m_attachedAt[index] = m_now;
                break;
            }
        }
    }
    if (root.Device == 0U)
    {
        if ((status & PortChanges) != 0U)
        {
            m_mmio.Write32(PortOffset(number), (status & PortWritable) | (status & PortChanges));
        }
        return;
    }
    Port& port = Root(number);
    if (attached && !m_initialScan)
    {
        m_settling[root.Device - 1U] = true;
        m_attachedAt[root.Device - 1U] = m_now;
    }
    if (((status & 0x200010U) != 0U) && (port.Stage == Phase::Reset) && port.ResetIssued)
    {
        port.ResetSeen = true; // PR asserted or PRC latched for the requested reset.
    }
    const UInt32 speed = (status >> 10U) & 15U;
    Input::UsbSpeed decoded{port.Speed};
    const UInt8 mapping = root.Speeds[speed];
    // A USB2 port's speed may read as undefined until a reset enables it,
    // and again while PR is asserted (xHCI 1.2 sections 4.19.1.1 and 5.4.8).
    // Connection is CCS alone; an undecodable speed keeps the provisional
    // one until the enabled port shows the negotiated speed.
    const bool valid = (mapping >= 1U) && (mapping <= 4U);
    if (valid)
    {
        decoded = static_cast<Input::UsbSpeed>(mapping - 1U);
    }
    if ((status & 8U) != 0U)
    {
        port.PoweredOff = true;
        m_mmio.Write32(PortOffset(number), (status & PortWritable & ~0x200U) | (status & PortChanges));
    }
    const bool connected = ((status & 1U) != 0U) && !port.PoweredOff;
    if (connected && valid)
    {
        port.Speed = decoded;
        port.SpeedIdentifier = static_cast<UInt8>(speed);
    }
    if (!connected && port.Connected && ((port.Slot != 0U) || port.Busy))
    {
        port.Detaching = true;
    }
    else if (!port.Detaching && (connected != port.Connected))
    {
        port.Connected = connected;
        port.Change = true;
    }
    if ((status & PortChanges) != 0U)
    {
        m_mmio.Write32(PortOffset(number),
                       (status & PortWritable & (port.PoweredOff ? ~0x200U : ~UInt32{0U})) | (status & PortChanges));
    }
}
bool XhciController::Command(Port& port, const UInt8 number, const UInt32 type, const UInt64 parameter,
                             const UInt32 flags) noexcept
{
    if (m_commandPort != 0U)
    {
        return false;
    }
    m_commands.Begin();
    m_commandPointer = m_commands.Push(
        {.Parameter = parameter, .Status = 0U, .Control = (type << 10U) | (UInt32{port.Slot} << 24U) | flags});
    m_commandPort = number;
    m_commandType = static_cast<UInt8>(type);
    m_commandPipe = &port;
    m_commandStarted = m_now;
    m_mmio.PublishDma();
    m_commands.Commit();
    m_mmio.PublishDma();
    m_mmio.Write32(m_doorbells, 0U);
    return true;
}
void XhciController::StartReset(Port& port, const UInt8 number) noexcept
{
    const Size index = DeviceIndex(number);
    if (port.ResetIssued || (m_settling[index] && (m_now - m_attachedAt[index] < AttachDebounceMs)))
    {
        return;
    }
    m_settling[index] = false;
    const UInt32 status = m_mmio.Read32(PortOffset(number));
    m_mmio.Write32(PortOffset(number), (status & PortWritable) | 16U);
    port.ResetIssued = true;
}
void XhciController::BuildAddress(Port& port, const UInt8 number) noexcept
{
    Clear(port.Memory.InputContext);
    UInt8* const slot = port.Memory.InputContext.Cpu + m_contextBytes;
    UInt8* const ep0 = slot + m_contextBytes;
    Store32(port.Memory.InputContext.Cpu + 4U, 3U);
    Store32(slot, (1U << 27U) | (UInt32{port.SpeedIdentifier} << 20U));
    Store32(slot + 4U, UInt32{number} << 16U);
    Store32(ep0 + 4U, (UInt32{port.Request.PacketSize0} << 16U) | (4U << 3U) | (3U << 1U));
    Store64(ep0 + 8U, port.Control.Dequeue());
    Store32(ep0 + 16U, 8U);
}
bool XhciController::BuildEndpoint(Port& port) noexcept
{
    const Input::EndpointDescriptor& endpoint = port.Request.Endpoint;
    const UInt32 packet = endpoint.MaxPacketSize & 0x7FFU;
    const bool bulk = (endpoint.Attributes & 3U) == 2U;
    const bool in = (endpoint.Address & 0x80U) != 0U;
    const UInt32 transactions = (endpoint.MaxPacketSize >> 11U) & 3U;
    if (((endpoint.Address & 0x70U) != 0U) || ((endpoint.Address & 15U) == 0U) ||
        (!bulk && (((endpoint.Attributes & 3U) != 3U) || !in)) || ((endpoint.MaxPacketSize & 0xE000U) != 0U) ||
        (packet == 0U) || (packet > 1024U) || (!bulk && (endpoint.Interval == 0U)) || (transactions == 3U))
    {
        return false;
    }
    UInt32 interval{0U};
    UInt32 burst{0U};
    UInt32 payload{packet};
    if (bulk)
    {
        if ((port.Speed == Input::UsbSpeed::Low) || (transactions != 0U) ||
            ((port.Speed == Input::UsbSpeed::Full) && (packet != 8U) && (packet != 16U) && (packet != 32U) &&
             (packet != 64U)) ||
            ((port.Speed == Input::UsbSpeed::High) && (packet != 512U)) ||
            ((port.Speed != Input::UsbSpeed::Super) && endpoint.HasCompanion) ||
            ((port.Speed == Input::UsbSpeed::Super) &&
             ((packet != 1024U) || !endpoint.HasCompanion || (endpoint.MaxBurst > 15U) ||
              (endpoint.CompanionAttributes != 0U) || (endpoint.BytesPerInterval != 0U))))
        {
            return false;
        }
        burst = port.Speed == Input::UsbSpeed::Super ? endpoint.MaxBurst : 0U;
        payload = 0U; // ESIT payload applies only to periodic endpoints.
        if ((port.Speed == Input::UsbSpeed::High) && !in)
        {
            interval = endpoint.Interval; // Maximum NAK rate, not a periodic exponent.
        }
    }
    else
    {
        switch (port.Speed)
        {
        case Input::UsbSpeed::Low:
        case Input::UsbSpeed::Full:
        {
            const UInt32 limit = port.Speed == Input::UsbSpeed::Low ? 8U : 64U;
            if ((packet > limit) || (transactions != 0U) || endpoint.HasCompanion)
            {
                return false;
            }
            UInt32 microframes = UInt32{endpoint.Interval} * 8U;
            for (Size i{0U}; (i < 10U) && (microframes > 1U); ++i)
            {
                ++interval;
                microframes >>= 1U;
            }
            break;
        }
        case Input::UsbSpeed::High:
            if ((endpoint.Interval > 16U) || endpoint.HasCompanion)
            {
                return false;
            }
            interval = UInt32{endpoint.Interval} - 1U;
            burst = transactions;
            payload = packet * (burst + 1U);
            break;
        case Input::UsbSpeed::Super:
            if ((endpoint.Interval > 16U) || !endpoint.HasCompanion || (endpoint.MaxBurst > 15U) ||
                (endpoint.CompanionAttributes != 0U) || (transactions != 0U) || (endpoint.BytesPerInterval == 0U) ||
                (endpoint.BytesPerInterval > packet * (UInt32{endpoint.MaxBurst} + 1U)))
            {
                return false;
            }
            interval = UInt32{endpoint.Interval} - 1U;
            burst = endpoint.MaxBurst;
            payload = endpoint.BytesPerInterval;
            break;
        }
    }
    Port* pipe = FindPipe(port.RootNumber, endpoint.Address);
    if (pipe != nullptr)
    {
        return false; // Reconfiguration is unsupported without dropping the existing context.
    }
    const Size number = static_cast<Size>(&port - m_ports);
    for (Size i{0U}; i < port.Memory.PipeCount; ++i)
    {
        if ((m_endpointPipes[number][i].Endpoint == 0U) && !m_endpointPipes[number][i].Ready)
        {
            pipe = &m_endpointPipes[number][i];
            break;
        }
    }
    if (pipe == nullptr)
    {
        return false;
    }
    port.Endpoint = static_cast<UInt8>(UInt32{endpoint.Address & 15U} * 2U + (in ? 1U : 0U));
    pipe->Endpoint = port.Endpoint;
    pipe->Descriptor = endpoint;
    pipe->Detaching = false;
    pipe->Speed = port.Speed;
    pipe->Slot = port.Slot;
    Clear(port.Memory.InputContext);
    // Preserve slot configuration; address/state and opaque bytes are output-only.
    m_mmio.ObserveDma();
    for (Size i{0U}; i < 12U; ++i)
    {
        port.Memory.InputContext.Cpu[m_contextBytes + i] = port.Memory.DeviceContext.Cpu[i];
    }
    Store32(port.Memory.InputContext.Cpu + 4U, 1U | (1U << port.Endpoint));
    UInt8* const slot = port.Memory.InputContext.Cpu + m_contextBytes;
    const UInt32 oldEntries = Load32(slot) >> 27U;
    const UInt32 entries = oldEntries > port.Endpoint ? oldEntries : port.Endpoint;
    Store32(slot, (Load32(slot) & 0x07FFFFFFU) | (entries << 27U));
    UInt8* const ep = slot + UInt32{port.Endpoint} * m_contextBytes;
    Store32(ep, interval << 16U);
    const UInt32 bulkType = in ? 6U : 2U;
    const UInt32 endpointType = bulk ? bulkType : 7U;
    Store32(ep + 4U, (packet << 16U) | (burst << 8U) | (endpointType << 3U) | (3U << 1U));
    Store64(ep + 8U, pipe->Interrupt.Dequeue());
    Store32(ep + 16U, (payload << 16U) | packet);
    return true;
}
bool XhciController::Submit(const UInt8 number, const Input::UsbRequest& request) noexcept
{
    // Routed slots need separate context/ring ownership; never reset the root slot.
    if (request.Route.Depth != 0U)
    {
        return false;
    }
    if ((m_state != ControllerState::Running) || (number == 0U) || (number > m_portCount) ||
        (DeviceIndex(number) == MaximumDevices))
    {
        return false;
    }
    Port& root = Root(number);
    const bool endpointTransfer = (request.Operation == Input::UsbOperation::InterruptIn) ||
                                  (request.Operation == Input::UsbOperation::BulkIn) ||
                                  (request.Operation == Input::UsbOperation::BulkOut);
    Port* selected = endpointTransfer ? FindPipe(number, request.Endpoint.Address) : &root;
    if ((selected == nullptr) || root.Detaching || !root.Connected)
    {
        return false;
    }
    Port& port = *selected;
    port.Speed = root.Speed;
    port.Slot = root.Slot;
    if (port.Busy || port.Ready || port.Detaching || (request.Token == 0U) || (SpeedId(request.Speed) == 0U) ||
        (request.Speed != port.Speed) || (request.Address > 127U) || (request.Length > 65536U) ||
        (request.Length > port.Memory.Buffer.Bytes) || ((request.Length != 0U) && (request.Data == nullptr)) ||
        !PacketSize(port.Speed, request.PacketSize0))
    {
        return false;
    }
    for (Size i{0U}; i <= root.Memory.PipeCount; ++i)
    {
        const Port& other = Pipe(number, i);
        if ((other.Busy || other.Ready) && (other.Request.Token == request.Token))
        {
            return false;
        }
    }
    switch (request.Operation)
    {
    case Input::UsbOperation::ResetPort:
        if (port.Slot != 0U)
        {
            return false;
        }
        break;
    case Input::UsbOperation::UpdateEndpointZero:
    case Input::UsbOperation::AssignAddress:
    case Input::UsbOperation::ConfigureEndpoint:
        if (port.Slot == 0U)
        {
            return false;
        }
        break;
    case Input::UsbOperation::Control:
        if ((port.Slot == 0U) ||
            (request.Length != (UInt32{request.Setup.Bytes[6U]} | (UInt32{request.Setup.Bytes[7U]} << 8U))))
        {
            return false;
        }
        break;
    case Input::UsbOperation::InterruptIn:
    case Input::UsbOperation::BulkIn:
    case Input::UsbOperation::BulkOut:
        if ((port.Slot == 0U) || !port.Configured ||
            ((request.Operation == Input::UsbOperation::InterruptIn) && (request.Length == 0U)) ||
            (root.RecoveryEndpoint == port.Endpoint) ||
            ((request.Operation == Input::UsbOperation::InterruptIn) && ((port.Descriptor.Attributes & 3U) != 3U)) ||
            ((request.Operation != Input::UsbOperation::InterruptIn) && ((port.Descriptor.Attributes & 3U) != 2U)) ||
            ((request.Operation == Input::UsbOperation::BulkOut) != ((port.Descriptor.Address & 0x80U) == 0U)))
        {
            return false;
        }
        break;
    default:
        return false;
    }
    // ConfigureEndpoint validation must not retain any borrow on refusal.
    port.Request = request;
    if ((request.Operation == Input::UsbOperation::ConfigureEndpoint) && !BuildEndpoint(port))
    {
        port.Request.Data = nullptr;
        return false;
    }
    port.Busy = true;
    port.Started = m_now;
    port.Stage = Phase::Operation;
    switch (request.Operation)
    {
    case Input::UsbOperation::ResetPort:
    {
        port.Stage = Phase::Reset;
        const bool super = port.SuperPort || (port.Speed == Input::UsbSpeed::Super);
        // A firmware-enabled USB2 port keeps PED across HCRST (RW1CS) with the
        // device still at its firmware address, so completion needs evidence
        // of this reset (PR or PRC), not PED alone. USB3 trains on its own.
        port.ResetSeen = super;
        port.ResetIssued = super;
        StartReset(port, number);
        break;
    }
    case Input::UsbOperation::UpdateEndpointZero:
    {
        Clear(port.Memory.InputContext);
        Store32(port.Memory.InputContext.Cpu + 4U, 2U);
        // Present the current Default Control Endpoint with only Max Packet
        // Size changed (xHCI 1.2 section 4.6.7); EP State is output-only.
        m_mmio.ObserveDma();
        UInt8* const ep0 = port.Memory.InputContext.Cpu + m_contextBytes * 2U;
        for (Size i{0U}; i < 20U; ++i)
        {
            ep0[i] = port.Memory.DeviceContext.Cpu[m_contextBytes + i];
        }
        Store32(ep0, Load32(ep0) & ~7U);
        Store32(ep0 + 4U, (Load32(ep0 + 4U) & 0xFFFFU) | (UInt32{request.PacketSize0} << 16U));
        port.Stage = Phase::Context;
        break;
    }
    case Input::UsbOperation::AssignAddress:
        BuildAddress(port, number);
        port.Stage = Phase::Context;
        break;
    case Input::UsbOperation::ConfigureEndpoint:
        port.Stage = Phase::Context;
        break;
    case Input::UsbOperation::Control:
    case Input::UsbOperation::InterruptIn:
    case Input::UsbOperation::BulkIn:
    case Input::UsbOperation::BulkOut:
        Transfer(port);
        break;
    }
    if (port.Stage == Phase::Context)
    {
        Progress(port, number);
    }
    return true;
}
bool XhciController::Abort(const UInt8 number, const UInt64 token) noexcept
{
    return Cancel(number, token, Input::TransferStatus::Cancelled);
}
bool XhciController::Abort(const UInt8 number, const UInt8 endpoint, const UInt64 token) noexcept
{
    Port* pipe = FindPipe(number, endpoint);
    return (pipe != nullptr) && CancelPipe(*pipe, token, Input::TransferStatus::Cancelled);
}
bool XhciController::Cancel(const UInt8 number, const UInt64 token, const Input::TransferStatus status) noexcept
{
    if ((number == 0U) || (number > m_portCount) || (DeviceIndex(number) == MaximumDevices))
    {
        return false;
    }
    for (Size i{0U}; i <= Root(number).Memory.PipeCount; ++i)
    {
        Port& pipe = Pipe(number, i);
        if (pipe.Busy && (pipe.Request.Token == token))
        {
            return CancelPipe(pipe, token, status);
        }
    }
    return false;
}
bool XhciController::CancelPipe(Port& port, const UInt64 token, const Input::TransferStatus status) noexcept
{
    if ((m_state != ControllerState::Running) || !port.Busy || port.Detaching || (port.Request.Token != token) ||
        ((port.Request.Operation != Input::UsbOperation::Control) &&
         (port.Request.Operation != Input::UsbOperation::InterruptIn) &&
         (port.Request.Operation != Input::UsbOperation::BulkIn) &&
         (port.Request.Operation != Input::UsbOperation::BulkOut)) ||
        (port.Stage != Phase::Operation))
    {
        return false;
    }
    port.RecoveryEndpoint = 0U;
    port.AbortStatus = status;
    port.Stage = Phase::StopEndpoint;
    return true;
}
void XhciController::Transfer(Port& port) noexcept
{
    const bool control = port.Request.Operation == Input::UsbOperation::Control;
    XhciRing& ring = control ? port.Control : port.Interrupt;
    ring.Begin();
    const Size length = port.Request.Length;
    const bool in = (control ? ((port.Request.Setup.Bytes[0U] & 0x80U) != 0U)
                             : (port.Request.Operation != Input::UsbOperation::BulkOut));
    port.TransferCount = 0U;
    port.ShortSeen = false;
    port.ActiveEndpoint = control ? 1U : port.Endpoint;
    if (control)
    {
        const UInt64 setup = Load64(port.Request.Setup.Bytes);
        // Transfer Type entered the Setup Stage TRB in xHCI 1.0; those bits
        // are reserved, written as zero, on a 0.96 controller.
        const UInt32 transferType = (length == 0U) || (m_version < 0x100U) ? 0U : (in ? 3U : 2U);
        port.TransferPointers[port.TransferCount++] =
            ring.Push({.Parameter = setup, .Status = 8U, .Control = (2U << 10U) | 64U | (transferType << 16U)}, true);
        if (length != 0U)
        {
            if (!in)
            {
                for (Size i{0U}; i < length; ++i)
                {
                    port.Memory.Buffer.Cpu[i] = port.Request.Data[i];
                }
            }
            port.TransferPointers[port.TransferCount++] = ring.Push({.Parameter = port.Memory.Buffer.Address,
                                                                     .Status = static_cast<UInt32>(length),
                                                                     .Control = (3U << 10U) | 4U | (in ? 65536U : 0U)},
                                                                    true);
        }
        port.TransferPointers[port.TransferCount++] = ring.Push(
            {.Parameter = 0U, .Status = 0U, .Control = (4U << 10U) | 32U | ((length == 0U) || !in ? 65536U : 0U)});
    }
    else
    {
        if (!in)
        {
            for (Size i{0U}; i < length; ++i)
            {
                port.Memory.Buffer.Cpu[i] = port.Request.Data[i];
            }
        }
        port.TransferPointers[port.TransferCount++] = ring.Push({.Parameter = port.Memory.Buffer.Address,
                                                                 .Status = static_cast<UInt32>(length),
                                                                 .Control = (1U << 10U) | 32U | (in ? 4U : 0U)});
    }
    m_mmio.PublishDma();
    ring.Commit();
    m_mmio.PublishDma();
    m_mmio.Write32(m_doorbells + UInt32{port.Slot} * 4U, port.ActiveEndpoint);
}
void XhciController::Finish(Port& port, const Input::TransferStatus status, const Size actual) noexcept
{
    if (port.Busy)
    {
        port.Result = {.Token = port.Request.Token,
                       .Status = status,
                       .ActualLength = actual,
                       .EndpointAddress = port.Index == 0U ? UInt8{0U} : port.Descriptor.Address};
        port.Ready = true;
    }
    port.RecoveryEndpoint = 0U;
    port.Busy = false;
    port.Request.Data = nullptr;
    port.TransferCount = 0U;
    port.Stage = Phase::Idle;
}
bool XhciController::TakeCompletion(const UInt8 number, Completion& output) noexcept
{
    if ((number == 0U) || (number > m_portCount) || (DeviceIndex(number) == MaximumDevices))
    {
        return false;
    }
    for (Size i{0U}; i <= Root(number).Memory.PipeCount; ++i)
    {
        Port& pipe = Pipe(number, i);
        if (pipe.Ready)
        {
            output = pipe.Result;
            pipe.Ready = false;
            return true;
        }
    }
    return false;
}
bool XhciController::TakeCompletion(const UInt8 number, const UInt8 endpoint, Completion& output) noexcept
{
    Port* pipe = FindPipe(number, endpoint);
    if ((pipe == nullptr) || !pipe->Ready)
    {
        return false;
    }
    output = pipe->Result;
    pipe->Ready = false;
    return true;
}
bool XhciController::TakePortChange(PortChange& output) noexcept
{
    for (UInt32 rootNumber{1U}; rootNumber <= m_portCount; ++rootNumber)
    {
        const UInt8 i = static_cast<UInt8>(rootNumber);
        if (DeviceIndex(i) == MaximumDevices)
        {
            continue;
        }
        Port& port = Root(i);
        if (port.Change)
        {
            output = {.Port = i, .Status = GetPortStatus(i)};
            port.Change = false;
            return true;
        }
    }
    return false;
}
void XhciController::HandleCommand(Port& port, const XhciEvent& event) noexcept
{
    const UInt8 number = m_commandPort;
    if ((event.Pointer != m_commandPointer) || ((port.Stage != Phase::Enable) && (event.Slot != port.Slot)) ||
        ((port.Stage == Phase::Enable) && ((event.Code == 1U) && ((event.Slot == 0U) || (event.Slot > m_slotCount)))))
    {
        ++m_rejected;
        return;
    }
    m_commandPort = 0U;
    m_commandPointer = 0U;
    if (event.Code != 1U)
    {
        Record(Input::UsbDiagnosticKind::Command, number, m_commandType, event.Code, 0U);
        // Command failure does not prove a disconnect quiescent. Halt before release.
        if ((port.Stage == Phase::StopEndpoint) && (event.Code == 19U) &&
            ((Load32(port.Memory.DeviceContext.Cpu + UInt32{port.ActiveEndpoint} * m_contextBytes) & 7U) == 2U))
        {
            // A stall can win the Stop Endpoint race, leaving Halted rather
            // than Stopped. Reset Endpoint is required before setting dequeue.
            port.Stage = Phase::AbortReset;
        }
        else if (port.Detaching)
        {
            FailController(Input::TransferStatus::Disconnected);
        }
        else if ((port.Stage == Phase::StopEndpoint) || (port.Stage == Phase::AbortReset) ||
                 (port.Stage == Phase::AbortDequeue))
        {
            FailController(Input::TransferStatus::Error);
        }
        else
        {
            if (port.Request.Operation == Input::UsbOperation::ConfigureEndpoint)
            {
                Port* pipe = FindPipe(number, port.Request.Endpoint.Address);
                if (pipe != nullptr)
                {
                    pipe->Endpoint = 0U;
                    pipe->Configured = false;
                }
            }
            Finish(port, CompletionStatus(event.Code));
        }
        return;
    }
    switch (port.Stage)
    {
    case Phase::Enable:
        for (Size i{0U}; i < m_slotCount; ++i)
        {
            if (m_ports[i].Slot == event.Slot)
            {
                FailController(Input::TransferStatus::Error);
                return;
            }
        }
        port.Slot = event.Slot;
        Record(Input::UsbDiagnosticKind::SlotEnabled, number, event.Slot, 0U, 0U);
        Store64(m_memory.Dcbaa.Cpu + UInt32{port.Slot} * 8U, port.Memory.DeviceContext.Address);
        // Read reset-negotiated speed before constructing default EP0.
        ScanPort(number);
        switch (port.Speed)
        {
        case Input::UsbSpeed::Low:
        case Input::UsbSpeed::Full:
            port.Request.PacketSize0 = 8U;
            break;
        case Input::UsbSpeed::High:
            port.Request.PacketSize0 = 64U;
            break;
        case Input::UsbSpeed::Super:
            port.Request.PacketSize0 = 512U;
            break;
        }
        BuildAddress(port, number);
        port.Stage = Phase::DefaultAddress;
        static_cast<void>(Command(port, number, 11U, port.Memory.InputContext.Address, 512U));
        break;
    case Phase::StopEndpoint:
    case Phase::AbortReset:
        port.Stage = Phase::AbortDequeue;
        break;
    case Phase::AbortDequeue:
        port.Stage = Phase::AbortDrain;
        break;
    case Phase::ResetEndpoint:
        port.Stage = Phase::Dequeue;
        break;
    case Phase::Dequeue:
        if (port.StallRecovery)
        {
            // The host side is running again; the device clears a protocol
            // stall at its next SETUP. The request itself failed with STALL.
            port.StallRecovery = false;
            Finish(port, Input::TransferStatus::Stall);
            break;
        }
        port.Stage = Phase::EndpointRecovery;
        port.RecoveryStarted = m_now;
        break;
    case Phase::Disable:
        Store64(m_memory.Dcbaa.Cpu + UInt32{port.Slot} * 8U, 0U);
        port.Slot = 0U;
        port.Endpoint = 0U;
        port.Connected = false;
        port.Change = true;
        port.Detaching = false;
        Finish(port, Input::TransferStatus::Disconnected);
        port.Control.Initialize(port.Memory.ControlRing);
        RetirePipes(number, Input::TransferStatus::Disconnected);
        for (Size i{0U}; i < port.Memory.PipeCount; ++i)
        {
            Port& pipe = m_endpointPipes[DeviceIndex(number)][i];
            pipe.Endpoint = 0U;
            pipe.Configured = false;
            pipe.Interrupt.Initialize(port.Memory.Pipes[i].Ring);
        }
        break;
    case Phase::Operation:
        if (port.Request.Operation == Input::UsbOperation::AssignAddress)
        {
            m_mmio.ObserveDma();
            Record(Input::UsbDiagnosticKind::Addressed, number, port.Slot, 0U,
                   Load32(port.Memory.DeviceContext.Cpu + 12U) & 0xFFU);
            port.Stage = Phase::AddressRecovery;
            port.RecoveryStarted = m_now;
        }
        else
        {
            if (port.Request.Operation == Input::UsbOperation::ConfigureEndpoint)
            {
                Port* pipe = FindPipe(number, port.Request.Endpoint.Address);
                if (pipe != nullptr)
                {
                    pipe->Configured = true;
                }
                const UInt8* const ep = port.Memory.InputContext.Cpu + (UInt32{port.Endpoint} + 1U) * m_contextBytes;
                Record(Input::UsbDiagnosticKind::EndpointConfigured, number, port.Request.Endpoint.Address,
                       port.Endpoint, (Load32(ep) >> 16U) & 0xFFU,
                       (Load32(ep + 4U) >> 16U) | (Load32(ep + 16U) & 0xFFFF0000U));
            }
            Finish(port, Input::TransferStatus::Success);
        }
        break;
    case Phase::DefaultAddress:
        m_mmio.ObserveDma();
        Record(Input::UsbDiagnosticKind::Addressed, number, port.Slot, 1U,
               Load32(port.Memory.DeviceContext.Cpu + 12U) & 0xFFU);
        Finish(port, Input::TransferStatus::Success);
        break;
    case Phase::Idle:
    case Phase::Context:
    case Phase::Reset:
    case Phase::ResetRecovery:
    case Phase::AddressRecovery:
    case Phase::EndpointRecovery:
    case Phase::AbortDrain:
        ++m_rejected;
        break;
    }
}
void XhciController::HandleTransfer(Port& port, const UInt8 number, const XhciEvent& event) noexcept
{
    if (port.Busy && (event.Endpoint == port.ActiveEndpoint) &&
        ((port.Stage == Phase::StopEndpoint) || (port.Stage == Phase::AbortReset) ||
         (port.Stage == Phase::AbortDequeue) || (port.Stage == Phase::AbortDrain)))
    {
        // Stop Endpoint may complete before a cancelled TD's Transfer Event.
        // Keep its TRBs/bounce buffer owned until Set Dequeue completes and the
        // shared event ring drains. Late Success/Stopped/Stall cannot complete
        // the request or start endpoint recovery a second time.
        return;
    }
    if (!port.Busy || (event.Endpoint != port.ActiveEndpoint) || (port.Stage != Phase::Operation))
    {
        ++m_rejected;
        return;
    }
    Size index{port.TransferCount};
    for (Size i{0U}; i < port.TransferCount; ++i)
    {
        if (port.TransferPointers[i] == event.Pointer)
        {
            index = i;
            break;
        }
    }
    if (index == port.TransferCount)
    {
        ++m_rejected;
        return;
    }
    const bool control = port.ActiveEndpoint == 1U;
    const bool data = !control || ((port.TransferCount == 3U) && (index == 1U));
    const Size length = port.Request.Length;
    // Setup/Status events describe their TRB, not the whole control TD's residual.
    const Size bound = data ? length : (index == 0U ? 8U : 0U);
    if ((event.Residual > bound) || ((event.Code == 13U) && !data) || ((event.Code == 1U) && (event.Residual != 0U)))
    {
        ++m_rejected;
        return;
    }
    if (port.Detaching)
    {
        return; // Disable Slot, not a transfer event, proves DMA quiescence.
    }
    if ((event.Code != 1U) && (event.Code != 13U))
    {
        Record(Input::UsbDiagnosticKind::Transfer, number, control ? UInt8{0U} : port.Descriptor.Address, event.Code,
               control ? UInt32{port.Request.Setup.Bytes[1U]} : 0U, event.Residual);
    }
    if (event.Code == 6U)
    {
        if (control)
        {
            // A STALL halts the Default Control Endpoint in the controller
            // (xHCI 1.2 section 4.10.2.1) although the device clears this
            // protocol stall at its next SETUP (USB 2.0 section 8.5.3.4).
            // Reset it and move the dequeue past the TD before reporting, so
            // the next control request (for example after an optional
            // SET_IDLE) runs instead of waiting on a halted ring.
            port.StallRecovery = true;
            port.Stage = Phase::ResetEndpoint;
            return;
        }
        Finish(port, Input::TransferStatus::Stall);
        return;
    }
    if ((event.Code == 4U) || (event.Code == 3U))
    {
        // Transaction error and babble both halt the endpoint (section
        // 4.10.2); recover the ring and let the caller's bounded retry run.
        port.StallRecovery = false;
        port.Stage = Phase::ResetEndpoint;
        return;
    }
    if ((event.Code != 1U) && (event.Code != 13U))
    {
        Finish(port, CompletionStatus(event.Code));
        return;
    }
    if (control && (index + 1U != port.TransferCount))
    {
        if ((event.Code != 13U) || port.ShortSeen)
        {
            ++m_rejected;
            return;
        }
        port.ShortLength = length - event.Residual;
        port.ShortSeen = true;
        return; // Wait for Status IOC before releasing/reusing the TD.
    }
    if (port.RecoveryEndpoint != 0U)
    {
        port.ActiveEndpoint = port.RecoveryEndpoint;
        const UInt32 state = Load32(port.Memory.DeviceContext.Cpu + UInt32{port.ActiveEndpoint} * m_contextBytes) & 7U;
        port.Stage = state == 2U ? Phase::AbortReset : Phase::StopEndpoint;
        port.AbortStatus = Input::TransferStatus::Success;
        return;
    }
    const Size actual = port.ShortSeen ? port.ShortLength : (length - event.Residual);
    if ((port.Request.Operation == Input::UsbOperation::InterruptIn) ||
        (port.Request.Operation == Input::UsbOperation::BulkIn) ||
        ((port.Request.Operation == Input::UsbOperation::Control) && ((port.Request.Setup.Bytes[0U] & 0x80U) != 0U)))
    {
        m_mmio.ObserveDma();
        for (Size i{0U}; i < actual; ++i)
        {
            port.Request.Data[i] = port.Memory.Buffer.Cpu[i];
        }
    }
    Finish(port, Input::TransferStatus::Success, actual);
}
void XhciController::HandleEvent(const XhciEvent& event) noexcept
{
    switch (event.Kind)
    {
    case EventKind::Command:
        if (m_commandPort == 0U)
        {
            ++m_rejected;
            return;
        }
        HandleCommand(*m_commandPipe, event);
        break;
    case EventKind::Transfer:
        for (Size i{0U}; i < m_slotCount; ++i)
        {
            if (m_ports[i].Slot == event.Slot)
            {
                for (Size j{0U}; j <= m_ports[i].Memory.PipeCount; ++j)
                {
                    Port& pipe = Pipe(m_ports[i].RootNumber, j);
                    if (pipe.Busy && (pipe.ActiveEndpoint == event.Endpoint))
                    {
                        pipe.Detaching = m_ports[i].Detaching;
                        HandleTransfer(pipe, m_ports[i].RootNumber, event);
                        return;
                    }
                }
                ++m_rejected;
                return;
            }
        }
        ++m_rejected;
        break;
    case EventKind::Port:
        if (event.Port > m_portCount)
        {
            ++m_rejected;
            return;
        }
        ScanPort(event.Port);
        break;
    case EventKind::Controller:
        FailController(Input::TransferStatus::Error);
        break;
    }
}
void XhciController::Progress(Port& port, const UInt8 number) noexcept
{
    if (port.Detaching && (port.Index == 0U))
    {
        if (m_commandPort != 0U)
        {
            return;
        }
        if (port.Slot != 0U)
        {
            port.Stage = Phase::Disable;
            static_cast<void>(Command(port, number, 10U, 0U));
        }
        else
        {
            port.Connected = false;
            port.Change = true;
            port.Detaching = false;
            Finish(port, Input::TransferStatus::Disconnected);
        }
        return;
    }
    if (port.Detaching || !port.Busy || (m_commandPort != 0U))
    {
        return;
    }
    switch (port.Stage)
    {
    case Phase::Reset:
    {
        StartReset(port, number);
        if (!port.ResetIssued)
        {
            break;
        }
        const UInt32 status = m_mmio.Read32(PortOffset(number));
        if ((status & 0x200010U) != 0U)
        {
            port.ResetSeen = true;
        }
        if (port.ResetSeen && ((status & 2U) != 0U) && ((status & 0x80000010U) == 0U))
        {
            const UInt32 id = (status >> 10U) & 15U;
            const UInt8 mapping = m_roots[number - 1U].Speeds[id];
            const bool valid = (mapping >= 1U) && (mapping <= 4U);
            Record(Input::UsbDiagnosticKind::ResetDone, number, static_cast<UInt8>(id), valid ? mapping : 0U, status);
            if (!valid)
            {
                // No Slot Context speed exists for this port; never address it.
                Finish(port, Input::TransferStatus::Error);
                break;
            }
            // ScanPort samples the enabled port's speed before Enable Slot.
            port.Stage = Phase::ResetRecovery;
            port.RecoveryStarted = m_now;
        }
        break;
    }
    case Phase::ResetRecovery:
        // USB reset recovery is software-timed, even if PORTSC completes instantly.
        if (m_now - port.RecoveryStarted >= 10U)
        {
            port.Stage = Phase::Enable;
            static_cast<void>(Command(port, number, 9U, 0U, UInt32{port.SlotType} << 16U));
        }
        break;
    case Phase::Context:
    {
        // Each root owns its input context. Accept and queue context work
        // while another port owns the shared command ring, rather than
        // charging controller contention to the device's retry limit.
        UInt32 type{0U};
        switch (port.Request.Operation)
        {
        case Input::UsbOperation::UpdateEndpointZero:
            type = 13U;
            break;
        case Input::UsbOperation::AssignAddress:
            type = 11U;
            break;
        case Input::UsbOperation::ConfigureEndpoint:
            type = 12U;
            break;
        case Input::UsbOperation::ResetPort:
        case Input::UsbOperation::Control:
        case Input::UsbOperation::InterruptIn:
        case Input::UsbOperation::BulkIn:
        case Input::UsbOperation::BulkOut:
            Finish(port, Input::TransferStatus::Error);
            return;
        }
        port.Stage = Phase::Operation;
        static_cast<void>(Command(port, number, type, port.Memory.InputContext.Address));
        break;
    }
    case Phase::AddressRecovery:
        if (m_now - port.RecoveryStarted >= 2U)
        {
            Finish(port, Input::TransferStatus::Success);
        }
        break;
    case Phase::EndpointRecovery:
        if (m_now - port.RecoveryStarted >= 10U)
        {
            Finish(port, Input::TransferStatus::Retryable);
        }
        break;
    case Phase::StopEndpoint:
        static_cast<void>(Command(port, number, 15U, 0U, UInt32{port.ActiveEndpoint} << 16U));
        break;
    case Phase::AbortReset:
        static_cast<void>(Command(port, number, 14U, 0U, UInt32{port.ActiveEndpoint} << 16U));
        break;
    case Phase::AbortDrain:
        m_mmio.ObserveDma();
        if ((Load32(m_memory.EventRing.Cpu + m_eventIndex * 16U + 12U) & 1U) != m_eventCycle)
        {
            Finish(port, port.AbortStatus);
        }
        break;
    case Phase::ResetEndpoint:
        static_cast<void>(Command(port, number, 14U, 0U, UInt32{port.ActiveEndpoint} << 16U));
        break;
    case Phase::Dequeue:
    case Phase::AbortDequeue:
    {
        const XhciRing& ring = TransferRing(port);
        static_cast<void>(Command(port, number, 16U, ring.Dequeue(), UInt32{port.ActiveEndpoint} << 16U));
        break;
    }
    case Phase::Idle:
    case Phase::Enable:
    case Phase::DefaultAddress:
    case Phase::Operation:
    case Phase::Disable:
        break;
    }
}
void XhciController::FailController(const Input::TransferStatus status) noexcept
{
    if (!Halt(m_pollLimit))
    {
        // Caller must revoke DMA/reset the device before releasing mappings or
        // core buffers. No unsafe Disconnected completion is fabricated.
        return;
    }
    for (Size i{0U}; i < m_slotCount; ++i)
    {
        if (m_ports[i].RootNumber == 0U)
        {
            continue;
        }
        for (Size j{0U}; j <= m_ports[i].Memory.PipeCount; ++j)
        {
            Port& pipe = Pipe(m_ports[i].RootNumber, j);
            if (pipe.Ready && (pipe.Result.Status != Input::TransferStatus::Disconnected))
            {
                pipe.Result.Status = status;
            }
        }
    }
}
void XhciController::Poll(const UInt64 milliseconds, const Size eventBudget) noexcept
{
    if (m_state != ControllerState::Running)
    {
        return;
    }
    if (milliseconds < m_now)
    {
        FailController(Input::TransferStatus::Error);
        return;
    }
    m_now = milliseconds;
    const Size budget = eventBudget < 64U ? eventBudget : 64U;
    for (Size i{0U}; i < budget; ++i)
    {
        m_mmio.ObserveDma();
        const UInt8* const bytes = m_memory.EventRing.Cpu + m_eventIndex * 16U;
        const UInt32 control = Load32(bytes + 12U);
        if ((control & 1U) != m_eventCycle)
        {
            break;
        }
        const Trb trb{.Parameter = Load64(bytes), .Status = Load32(bytes + 8U), .Control = control};
        XhciEvent event{};
        ++m_eventIndex;
        if (m_eventIndex == m_memory.EventRing.Bytes / 16U)
        {
            m_eventIndex = 0U;
            m_eventCycle ^= 1U;
        }
        if (DecodeEvent(trb, event))
        {
            HandleEvent(event);
        }
        else
        {
            ++m_rejected;
        }
        if (m_state != ControllerState::Running)
        {
            return;
        }
    }
    m_mmio.Write32(m_operational + 4U, 8U); // W1C EINT, never other status bits.
    m_mmio.Write32(m_runtime + 0x20U, m_delivery == EventDelivery::Interrupt ? 3U : 1U);
    m_mmio.Write64(m_runtime + 0x38U, (m_memory.EventRing.Address + m_eventIndex * 16U) | 8U);
    const UInt32 usbStatus = m_mmio.Read32(m_operational + 4U);
    if ((usbStatus & 0x1004U) != 0U)
    {
        Record(Input::UsbDiagnosticKind::Controller, 0U, 0U, 0U, usbStatus);
        FailController(Input::TransferStatus::Error);
        return;
    }
    if ((m_commandPort != 0U) && (m_now - m_commandStarted >= OperationTimeoutMs))
    {
        Record(Input::UsbDiagnosticKind::Command, m_commandPort, m_commandType, 0U, 0U);
        FailController(Input::TransferStatus::Retryable);
        return;
    }
    for (UInt32 rootNumber{1U}; rootNumber <= m_portCount; ++rootNumber)
    {
        const UInt8 i = static_cast<UInt8>(rootNumber);
        ScanPort(i);
        if (DeviceIndex(i) == MaximumDevices)
        {
            continue;
        }
        for (Size j{0U}; j <= Root(i).Memory.PipeCount; ++j)
        {
            Port& port = Pipe(i, j);
            port.Detaching = Root(i).Detaching;
            if (port.Busy && !port.Detaching)
            {
                const UInt64 timeout = port.Request.TimeoutMilliseconds;
                const UInt64 started = port.Started;
                if ((port.Stage == Phase::Operation) && (port.TransferCount != 0U) && (timeout != 0U) &&
                    (m_now - started >= timeout))
                {
                    static_cast<void>(Cancel(i, port.Request.Token, Input::TransferStatus::TimedOut));
                }
                else if (((port.Stage == Phase::Reset) || (port.Stage == Phase::ResetRecovery)) &&
                         (m_now - port.Started >= OperationTimeoutMs))
                {
                    Record(Input::UsbDiagnosticKind::ResetTimeout, i, 0U, 0U, m_mmio.Read32(PortOffset(i)));
                    Finish(port, Input::TransferStatus::TimedOut);
                }
            }
            Progress(port, i);
        }
    }
}
} // Namespace Asteria::User::Usb
