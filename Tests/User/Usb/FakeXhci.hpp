// SPDX-License-Identifier: Apache-2.0
#ifndef ASTERIA_TESTS_USER_USB_FAKEXHCI_HPP_INCLUDED
#define ASTERIA_TESTS_USER_USB_FAKEXHCI_HPP_INCLUDED

#include "User/Usb/Xhci.hpp"

#include <cstring>

#include "User/Input/ProtocolFixtures.hpp"

namespace Asteria::Test::Usb
{
namespace X = User::Usb;
namespace In = User::Input;
namespace Fx = Test::Input;
using Foundation::Size;
using Foundation::UInt8;
using Foundation::UInt32;
using Foundation::UInt64;

// Device-visible model: only BAR registers and device addresses are consulted.
// Never reads the controller object's state. Register/ring behavior follows
// Intel xHCI 1.2 sections 4.9, 4.10, 5.4, 6.2 and 6.4 (32/64-byte contexts).
class FakeXhci final : public X::IXhciMmio
{
public:
    static constexpr UInt32 Operational{0x80U};
    static constexpr UInt32 Runtime{0x1800U};
    static constexpr UInt32 Doorbells{0x2000U};
    struct Mapping
    {
        UInt64 Address;
        UInt8* Cpu;
        Size Bytes;
    };
    struct Ring
    {
        UInt64 Pointer{0U};
        UInt32 Cycle{1U};
    };
    struct Slot
    {
        bool Enabled{false};
        bool Addressed{false};
        UInt8 Port{0U};
        UInt64 Context{0U};
        Ring Endpoints[32U]{};
        bool Halted[32U]{};
        bool Stopped[32U]{};
        bool DeviceHalt{false};
        bool DeviceHalts[32U]{};
    };
    explicit FakeXhci(const bool largeContexts = false, const UInt32 scratchpads = 1U) noexcept
        : ContextBytes{largeContexts ? 64U : 32U}
    {
        Registers[0U] = (0x120U << 16U) | Operational;
        Registers[1U] = (2U << 24U) | (1U << 8U) | 8U;
        Registers[2U] = ((scratchpads & 31U) << 27U) | ((scratchpads >> 5U) << 21U);
        Registers[4U] = (0x10U << 16U) | 1U | (largeContexts ? 4U : 0U);
        Registers[5U] = Doorbells;
        Registers[6U] = Runtime;
        Registers[0x40U / 4U] = (2U << 24U) | 2U;
        Registers[0x44U / 4U] = 0x20425355U;
        Registers[0x48U / 4U] = 0x201U;
        Registers[Operational / 4U + 1U] = 1U;
        Registers[Operational / 4U + 2U] = 1U;
        SetPort(1U, true, 1U);
        SetPort(2U, true, 1U);
    }
    void Map(const X::DmaMemory memory) noexcept
    {
        if (MappingCount < 1152U)
        {
            Mappings[MappingCount++] = {.Address = memory.Address, .Cpu = memory.Cpu, .Bytes = memory.Bytes};
        }
        else
        {
            Good = false;
        }
    }
    [[nodiscard]] UInt8* Translate(const UInt64 address, const Size bytes) noexcept
    {
        for (Size i{0U}; i < MappingCount; ++i)
        {
            const Mapping& map = Mappings[i];
            if ((address >= map.Address) && (address - map.Address <= map.Bytes) &&
                (bytes <= map.Bytes - (address - map.Address)))
            {
                return map.Cpu + (address - map.Address);
            }
        }
        Good = false;
        return nullptr;
    }
    [[nodiscard]] UInt32 Read32(const UInt32 offset) noexcept override
    {
        ++Reads;
        if ((offset >= sizeof(Registers)) || ((offset & 3U) != 0U))
        {
            Good = false;
            return 0U;
        }
        return Registers[offset / 4U];
    }
    [[nodiscard]] UInt64 Read64(const UInt32 offset) noexcept override
    {
        return UInt64{Read32(offset)} | (UInt64{Read32(offset + 4U)} << 32U);
    }
    void Write32(const UInt32 offset, const UInt32 value) noexcept override
    {
        if ((offset >= sizeof(Registers)) || ((offset & 3U) != 0U))
        {
            Good = false;
            return;
        }
        if (offset == Operational)
        {
            Registers[offset / 4U] = value;
            if ((value & 2U) != 0U)
            {
                ++Resets;
                if (!HoldReset)
                {
                    Registers[offset / 4U] = 0U;
                    Registers[offset / 4U + 1U] = 1U;
                }
            }
            else if (!HoldHalt)
            {
                if ((value & 1U) != 0U)
                {
                    Registers[offset / 4U + 1U] &= ~1U;
                }
                else
                {
                    Registers[offset / 4U + 1U] |= 1U;
                }
            }
            return;
        }
        if (offset == Operational + 4U)
        {
            Registers[offset / 4U] &= ~(value & 0x41CU);
            return;
        }
        if ((offset >= Operational + 0x400U) && (offset < Operational + 0x400U + (Registers[1U] >> 24U) * 16U) &&
            (((offset - Operational - 0x400U) % 16U) == 0U))
        {
            UInt32& port = Registers[offset / 4U];
            port &= ~(value & 0xFE0000U);
            port = (port & ~0x200U) | (value & 0x200U);
            if ((value & 2U) != 0U)
            {
                port &= ~2U; // PED is write-one-to-disable, not an ordinary RW bit.
            }
            if ((value & 0x80000010U) != 0U)
            {
                ++PortResets;
                LastReset = value;
                const UInt8 number = static_cast<UInt8>((offset - Operational - 0x400U) / 16U + 1U);
                if (!HoldPortReset)
                {
                    if ((value & 0x80000000U) != 0U)
                    {
                        port |= 0x80000U;
                    }
                    CompletePortReset(number);
                }
            }
            return;
        }
        if (offset == Runtime + 0x20U)
        {
            Registers[offset / 4U] = (Registers[offset / 4U] & ~(value & 1U)) | (value & 2U);
            return;
        }
        Registers[offset / 4U] = value;
        if ((offset >= Doorbells) && (offset < Doorbells + 12U))
        {
            ++DoorbellCount;
            if ((Registers[Operational / 4U + 1U] & 1U) != 0U)
            {
                Good = false;
                return;
            }
            const UInt8 slot = static_cast<UInt8>((offset - Doorbells) / 4U);
            if (slot == 0U)
            {
                if (!HoldCommands)
                {
                    Commands();
                }
            }
            else if (!HoldTransfers)
            {
                Transfers(slot, static_cast<UInt8>(value & 0xFFU));
            }
        }
    }
    void WriteOsOwned(const UInt32 offset) noexcept override
    {
        Registers[offset / 4U] |= 0x01000000U;
        if (!HoldBios)
        {
            Registers[offset / 4U] &= ~0x10000U;
        }
    }
    void Write64(const UInt32 offset, const UInt64 value) noexcept override
    {
        if ((offset > sizeof(Registers) - 8U) || ((offset & 3U) != 0U))
        {
            Good = false;
            return;
        }
        if (offset == Runtime + 0x38U)
        {
            LastErdp = value;
            ++ErdpWrites;
        }
        Registers[offset / 4U] = static_cast<UInt32>(value & 0xFFFFFFFFU);
        Registers[offset / 4U + 1U] = static_cast<UInt32>(value >> 32U);
        if (offset == Operational + 0x18U)
        {
            CommandRing = {.Pointer = value & ~UInt64{63U}, .Cycle = static_cast<UInt32>(value & 1U)};
        }
        if (offset == Runtime + 0x30U)
        {
            UInt8* const entry = Translate(value, 16U);
            if (entry != nullptr)
            {
                EventBase = X::Load64(entry);
                EventCount = X::Load32(entry + 8U);
                EventIndex = 0U;
                EventCycle = 1U;
                Good = Good && (EventCount >= 16U) && (EventCount <= 4096U) && (X::Load32(entry + 12U) == 0U);
            }
        }
    }
    void PublishDma() noexcept override
    {
        ++Publishes;
    }
    void ObserveDma() noexcept override
    {
        ++Observes;
    }
    void SetPort(const UInt8 number, const bool connected, const UInt32 speed) noexcept
    {
        Registers[(Operational + 0x400U + UInt32{number - 1U} * 16U) / 4U] =
            0x200U | 0x20000U | (connected ? 1U | (speed << 10U) | (speed == 4U ? 2U : 0U) : 0U);
    }
    // A USB2 attach whose Port Speed reads Undefined (0) until a reset
    // enables the port (xHCI 1.2 sections 4.19.1.1 and 5.4.8).
    void SetPortUnreset(const UInt8 number, const UInt32 speed) noexcept
    {
        Registers[(Operational + 0x400U + UInt32{number - 1U} * 16U) / 4U] = 0x200U | 0x20000U | 1U;
        PendingSpeed[number - 1U] = speed;
    }
    // Ends a USB2 reset: PED and PRC set, PR clear, negotiated speed shown.
    void CompletePortReset(const UInt8 number) noexcept
    {
        UInt32& port = Registers[(Operational + 0x400U + UInt32{number - 1U} * 16U) / 4U];
        port |= 2U | 0x200000U;
        port &= ~0x80000010U;
        if (PendingSpeed[number - 1U] != 0U)
        {
            port = (port & ~0x3C00U) | (PendingSpeed[number - 1U] << 10U);
            PendingSpeed[number - 1U] = 0U;
        }
        Emit({.Parameter = UInt64{number} << 24U, .Status = 1U << 24U, .Control = 34U << 10U});
    }
    void Disconnect(const UInt8 number) noexcept
    {
        SetPort(number, false, 0U);
        Emit({.Parameter = UInt64{number} << 24U, .Status = 1U << 24U, .Control = 34U << 10U});
    }
    void Emit(const X::Trb event) noexcept
    {
        if (EventBase == 0U)
        {
            return;
        }
        const UInt64 dequeue = Read64(Runtime + 0x38U) & ~UInt64{15U};
        const Size dequeueIndex = static_cast<Size>((dequeue - EventBase) / 16U);
        if (((EventIndex + 1U) % EventCount) == dequeueIndex)
        {
            Good = false;
            return;
        }
        X::Trb stored = event;
        if (((EventIndex + 2U) % EventCount) == dequeueIndex)
        {
            // Reserve an entry to report Event Ring Full Error (xHCI 1.2 section 4.9.4).
            stored = {.Parameter = 0U, .Status = 21U << 24U, .Control = 37U << 10U};
            ++EventRingFullCount;
        }
        UInt8* const bytes = Translate(EventBase + EventIndex * 16U, 16U);
        if (bytes == nullptr)
        {
            return;
        }
        X::Store64(bytes, stored.Parameter);
        X::Store32(bytes + 8U, stored.Status);
        X::Store32(bytes + 12U, stored.Control | EventCycle);
        ++EventIndex;
        if (EventIndex == EventCount)
        {
            EventIndex = 0U;
            EventCycle ^= 1U;
            ++EventWraps;
        }
        Registers[Runtime / 4U + 8U] |= 1U;
        Registers[Operational / 4U + 1U] |= 8U;
    }
    [[nodiscard]] bool Fetch(Ring& ring, X::Trb& output, UInt64& pointer) noexcept
    {
        for (Size i{0U}; i < 4U; ++i)
        {
            UInt8* const bytes = Translate(ring.Pointer, 16U);
            if (bytes == nullptr)
            {
                return false;
            }
            const UInt32 control = X::Load32(bytes + 12U);
            if ((control & 1U) != ring.Cycle)
            {
                return false;
            }
            const UInt32 type = (control >> 10U) & 63U;
            if (type == 6U)
            {
                ring.Pointer = X::Load64(bytes) & ~UInt64{15U};
                if ((control & 2U) != 0U)
                {
                    ring.Cycle ^= 1U;
                }
                ++LinkFetches;
                continue;
            }
            pointer = ring.Pointer;
            output = {.Parameter = X::Load64(bytes), .Status = X::Load32(bytes + 8U), .Control = control};
            ring.Pointer += 16U;
            return true;
        }
        Good = false;
        return false;
    }
    void Commands() noexcept
    {
        for (Size i{0U}; i < 16U; ++i)
        {
            X::Trb trb{};
            UInt64 pointer{0U};
            if (!Fetch(CommandRing, trb, pointer))
            {
                return;
            }
            const UInt32 type = (trb.Control >> 10U) & 63U;
            UInt8 slot = static_cast<UInt8>(trb.Control >> 24U);
            const UInt8 endpoint = static_cast<UInt8>((trb.Control >> 16U) & 31U);
            UInt32 code{1U};
            if (type == 9U)
            {
                slot = Slots[0U].Enabled ? 2U : 1U;
                Good = Good && !Slots[slot - 1U].Enabled;
                Slots[slot - 1U] = {};
                Slots[slot - 1U].Enabled = true;
            }
            else if ((slot == 0U) || (slot > 2U) || !Slots[slot - 1U].Enabled)
            {
                code = 11U;
            }
            else
            {
                Slot& device = Slots[slot - 1U];
                switch (type)
                {
                case 10U:
                    device.Enabled = false;
                    ++Disabled;
                    break;
                case 11U:
                case 12U:
                case 13U:
                    code = ContextCommand(device, slot, type, trb);
                    break;
                case 14U:
                    if (!device.Halted[endpoint])
                    {
                        code = 19U;
                    }
                    else
                    {
                        device.Halted[endpoint] = false;
                        device.Stopped[endpoint] = true;
                        ++EndpointResets;
                    }
                    break;
                case 15U:
                    if (device.Halted[endpoint])
                    {
                        code = 19U;
                    }
                    else
                    {
                        device.Stopped[endpoint] = true;
                        ++EndpointStops;
                        StopEvent = {.Parameter = device.Endpoints[endpoint].Pointer,
                                     .Status = StopCode << 24U,
                                     .Control = (32U << 10U) | (UInt32{endpoint} << 16U) | (UInt32{slot} << 24U)};
                        if (!StopEventAfterCommand && !SuppressStoppedEvent)
                        {
                            Emit(StopEvent);
                        }
                    }
                    break;
                case 16U:
                    if (!device.Stopped[endpoint])
                    {
                        code = 19U;
                    }
                    else
                    {
                        device.Endpoints[endpoint] = {.Pointer = trb.Parameter & ~UInt64{15U},
                                                      .Cycle = static_cast<UInt32>(trb.Parameter & 1U)};
                        device.Stopped[endpoint] = false;
                        ++DequeueSets;
                    }
                    break;
                default:
                    code = 5U;
                    Good = false;
                    break;
                }
            }
            ++CommandCount;
            LastCommand = {
                .Parameter = pointer, .Status = code << 24U, .Control = (33U << 10U) | (UInt32{slot} << 24U)};
            Emit(LastCommand);
            if (((type == 15U) || (type == 16U)) && (code == 1U) && StopEventAfterCommand && !SuppressStoppedEvent)
            {
                Emit(StopEvent);
            }
        }
        Good = false;
    }
    [[nodiscard]] UInt32 ContextCommand(Slot& device, const UInt8 slot, const UInt32 type, const X::Trb& trb) noexcept
    {
        UInt8* const input = Translate(trb.Parameter, ContextBytes * 33U);
        UInt8* const dcbaa = Translate(Read64(Operational + 0x30U), 24U);
        if ((input == nullptr) || (dcbaa == nullptr))
        {
            return 5U;
        }
        device.Context = X::Load64(dcbaa + UInt32{slot} * 8U);
        UInt8* const output = Translate(device.Context, ContextBytes * 32U);
        if (output == nullptr)
        {
            return 5U;
        }
        const UInt32 adds = X::Load32(input + 4U);
        Good = Good && (X::Load32(input) == 0U) && (X::Load32(input + ContextBytes + 12U) == 0U);
        if (type == 11U)
        {
            Good = Good && (adds == 3U);
            device.Port = static_cast<UInt8>((X::Load32(input + ContextBytes + 4U) >> 16U) & 0xFFU);
            Good = Good && (device.Port >= 1U) && (device.Port <= (Registers[1U] >> 24U));
            std::memcpy(output, input + ContextBytes, ContextBytes * 2U);
            device.Endpoints[1U] = {.Pointer = X::Load64(input + ContextBytes * 2U + 8U) & ~UInt64{15U},
                                    .Cycle = static_cast<UInt32>(X::Load64(input + ContextBytes * 2U + 8U) & 1U)};
            const UInt32 ep = X::Load32(input + ContextBytes * 2U + 4U);
            Good = Good && (((ep >> 3U) & 7U) == 4U) && ((ep & 6U) == 6U) &&
                   ((X::Load32(input + ContextBytes) >> 27U) == 1U);
            X::Store32(output + ContextBytes, (X::Load32(output + ContextBytes) & ~7U) | 1U);
            X::Store32(output + 12U, ((trb.Control & 512U) != 0U ? 1U : 2U) << 27U);
            if ((trb.Control & 512U) == 0U)
            {
                device.Addressed = true;
                ++Addresses;
                X::Store32(output + 12U, (2U << 27U) | slot);
            }
            else
            {
                ++DefaultAddresses;
            }
        }
        else if (type == 13U)
        {
            Good = Good && (adds == 2U);
            LastEvaluateEndpoint = X::Load32(input + ContextBytes * 2U + 4U);
            X::Store32(output + ContextBytes + 4U, (X::Load32(output + ContextBytes + 4U) & 0xFFFFU) |
                                                       (X::Load32(input + ContextBytes * 2U + 4U) & 0xFFFF0000U));
            ++Evaluations;
        }
        else
        {
            Good = Good && ((adds & 3U) == 1U) && device.Addressed;
            for (UInt32 endpoint{2U}; endpoint < 32U; ++endpoint)
            {
                if ((adds & (1U << endpoint)) != 0U)
                {
                    UInt8* const ep = input + (endpoint + 1U) * ContextBytes;
                    std::memcpy(output + endpoint * ContextBytes, ep, ContextBytes);
                    X::Store32(output + endpoint * ContextBytes, (X::Load32(ep) & ~7U) | 1U);
                    const UInt64 dequeue = X::Load64(ep + 8U);
                    device.Endpoints[endpoint] = {.Pointer = dequeue & ~UInt64{15U},
                                                  .Cycle = static_cast<UInt32>(dequeue & 1U)};
                    const UInt32 epType = (X::Load32(ep + 4U) >> 3U) & 7U;
                    Good = Good && ((epType == 7U) || (epType == 6U) || (epType == 2U)) &&
                           ((X::Load32(input + ContextBytes) >> 27U) >= endpoint);
                    LastInterval = (X::Load32(ep) >> 16U) & 0xFFU;
                    LastPacket = X::Load32(ep + 4U) >> 16U;
                    LastBurst = (X::Load32(ep + 4U) >> 8U) & 0xFFU;
                    LastPayload = X::Load32(ep + 16U) >> 16U;
                }
            }
            X::Store32(output, (X::Load32(output) & 0x07FFFFFFU) | (X::Load32(input + ContextBytes) & 0xF8000000U));
            X::Store32(output + 12U, (X::Load32(output + 12U) & 0x07FFFFFFU) | (3U << 27U));
            ++Configurations;
        }
        return Good ? 1U : 5U;
    }
    void Transfers(const UInt8 slot, const UInt8 endpoint) noexcept
    {
        if ((slot == 0U) || (slot > 2U) || (endpoint == 0U) || (endpoint > 31U))
        {
            Good = false;
            return;
        }
        Slot& device = Slots[slot - 1U];
        Ring& ring = device.Endpoints[endpoint];
        if (device.Halted[endpoint] || device.Stopped[endpoint] || (ring.Pointer == 0U))
        {
            return;
        }
        X::Trb trbs[3U]{};
        UInt64 pointers[3U]{};
        const Ring start = ring;
        if (!Fetch(ring, trbs[0U], pointers[0U]))
        {
            return;
        }
        Size count{1U};
        if (endpoint == 1U)
        {
            for (Size i{1U}; i < 3U; ++i)
            {
                if (!Fetch(ring, trbs[i], pointers[i]))
                {
                    Good = false;
                    return;
                }
                ++count;
                if (((trbs[i].Control >> 10U) & 63U) == 4U)
                {
                    break;
                }
            }
        }
        UInt32 code{NextCode};
        NextCode = 1U;
        if ((endpoint == 1U) && (StallRequest != 0U) && (((trbs[0U].Parameter >> 8U) & 0xFFU) == StallRequest) &&
            ((StallIndex == 0xFFU) || (((trbs[0U].Parameter >> 32U) & 0xFFU) == StallIndex)))
        {
            StallNext = true; // a protocol STALL; the device clears it at the next SETUP
            ++StalledRequests;
        }
        if (StallNext || ((endpoint != 1U) && device.DeviceHalts[endpoint]))
        {
            code = 6U;
            StallNext = false;
            device.DeviceHalt = endpoint != 1U;
            device.DeviceHalts[endpoint] = endpoint != 1U;
        }
        Size actual{0U};
        const Size requested =
            endpoint == 1U ? (count == 3U ? trbs[1U].Status & 0x1FFFFU : 0U) : trbs[0U].Status & 0x1FFFFU;
        if ((code == 1U) && (endpoint == 1U))
        {
            const UInt64 setup = trbs[0U].Parameter;
            const UInt8 request = static_cast<UInt8>((setup >> 8U) & 0xFFU);
            const bool in = (setup & 0x80U) != 0U;
            Good = Good && (((trbs[0U].Control >> 10U) & 63U) == 2U) && ((trbs[0U].Control & 64U) != 0U) &&
                   (trbs[0U].Status == 8U) && (((trbs[count - 1U].Control >> 10U) & 63U) == 4U) &&
                   ((trbs[count - 1U].Control & 32U) != 0U) &&
                   (((trbs[count - 1U].Control & 65536U) != 0U) == ((requested == 0U) || !in));
            // Setup Stage TRT exists from xHCI 1.0; a 0.96 controller reserves it.
            const bool transferType = (Registers[0U] >> 16U) >= 0x100U;
            if (count == 3U)
            {
                Good = Good && (((trbs[1U].Control >> 10U) & 63U) == 3U) &&
                       (((trbs[1U].Control & 65536U) != 0U) == in) &&
                       (((trbs[0U].Control >> 16U) & 3U) == (transferType ? (in ? 3U : 2U) : 0U));
            }
            else
            {
                Good = Good && (((trbs[0U].Control >> 16U) & 3U) == 0U);
            }
            if (request == 6U)
            {
                const UInt8 descriptor = static_cast<UInt8>((setup >> 24U) & 0xFFU);
                const bool mouse = device.Port == 2U;
                const UInt8* const standard = mouse ? Fx::MouseConfiguration : Fx::KeyboardConfiguration;
                const UInt8* const configuration = ConfigurationBytes == nullptr ? standard : ConfigurationBytes;
                const UInt8* const standard0 = mouse ? Fx::MouseDevice : Fx::KeyboardDevice;
                const UInt8* const source =
                    descriptor == 1U ? (DeviceBytes == nullptr ? standard0 : DeviceBytes) : configuration;
                const Size configurationLength = ConfigurationBytes == nullptr ? 34U : ConfigurationLength;
                const Size total = descriptor == 1U ? 18U : configurationLength;
                actual = requested < total ? requested : total;
                UInt8* const buffer = Translate(trbs[1U].Parameter, requested);
                if (buffer != nullptr)
                {
                    std::memcpy(buffer, source, actual);
                    if ((device.Port == 1U) && (UnsupportedClass != 0U))
                    {
                        if ((descriptor == 1U) && (actual == 18U) && (UnsupportedClass == 9U))
                        {
                            buffer[4U] = 9U;
                        }
                        if ((descriptor == 2U) && (actual >= 18U))
                        {
                            buffer[14U] = UnsupportedClass;
                            buffer[15U] = 6U;
                            buffer[16U] = 0x50U;
                        }
                    }
                }
            }
            else if (request == 1U)
            {
                const UInt32 address = (setup >> 32U) & 0xFFU;
                const UInt32 target = (address & 15U) * 2U + ((address & 0x80U) != 0U ? 1U : 0U);
                Good = Good && ((setup & 0xFFU) == 2U) && (target > 1U);
                device.DeviceHalts[target] = false;
                device.DeviceHalt = false;
                ++ClearHalts;
            }
            else
            {
                Good = Good && ((request == 9U) || (request == 11U) || (request == 10U));
                if (request == 11U)
                {
                    ProtocolMask |= 1U << ((setup >> 32U) & 15U);
                }
                if (request == 10U)
                {
                    IdleMask |= 1U << ((setup >> 32U) & 15U);
                }
                actual = requested;
            }
        }
        else if ((code == 1U) && (endpoint != 1U))
        {
            UInt8* const context = Translate(device.Context + UInt32{endpoint} * ContextBytes, ContextBytes);
            if (context == nullptr)
            {
                return;
            }
            const UInt32 type = (X::Load32(context + 4U) >> 3U) & 7U;
            const bool in = type != 2U;
            Good = Good && (((trbs[0U].Control >> 10U) & 63U) == 1U) && ((trbs[0U].Control & 32U) != 0U) &&
                   (((trbs[0U].Control & 4U) != 0U) == in);
            const bool mouse = MouseEndpoint != 0U
                                   ? endpoint == MouseEndpoint
                                   : ((device.Port == 2U) || ((ConfigurationBytes != nullptr) && (endpoint == 5U)));
            const UInt8* const report = mouse ? MouseReport : KeyboardReport;
            const Size total = type == 7U ? (mouse ? MouseReportLength : 8U) : requested;
            actual = requested < total ? requested : total;
            if (ShortNext)
            {
                actual = 2U;
                ShortNext = false;
            }
            UInt8* const buffer = Translate(trbs[0U].Parameter, requested);
            if (buffer != nullptr)
            {
                if (type == 7U)
                {
                    std::memcpy(buffer, report, actual);
                }
                else if (in)
                {
                    for (Size i{0U}; i < actual; ++i)
                    {
                        buffer[i] = static_cast<UInt8>(i & 255U);
                    }
                }
                else
                {
                    for (Size i{0U}; i < actual; ++i)
                    {
                        Good = Good && (buffer[i] == static_cast<UInt8>(i & 255U));
                    }
                    BulkOutBytes += actual;
                }
            }
        }
        ++TransferCount;
        if ((code == 6U) || (code == 4U) || (code == 3U))
        {
            device.Halted[endpoint] = true;
            UInt8* const output = Translate(device.Context + UInt32{endpoint} * ContextBytes, 4U);
            if (output != nullptr)
            {
                X::Store32(output, (X::Load32(output) & ~UInt32{7U}) | 2U);
            }
            ring = start; // QEMU's xhci_stall_ep rewinds to the first TRB.
        }
        if ((code != 1U) || (endpoint != 1U))
        {
            Emit({.Parameter = pointers[0U],
                  .Status = ((code == 1U && actual < requested ? 13U : code) << 24U) |
                            static_cast<UInt32>(endpoint == 1U ? 0U : requested - actual),
                  .Control = (32U << 10U) | (UInt32{endpoint} << 16U) | (UInt32{slot} << 24U)});
        }
        else
        {
            if (actual < requested)
            {
                Emit({.Parameter = pointers[1U],
                      .Status = (13U << 24U) | static_cast<UInt32>(requested - actual),
                      .Control = (32U << 10U) | (1U << 16U) | (UInt32{slot} << 24U)});
            }
            Emit({.Parameter = pointers[count - 1U],
                  .Status = 1U << 24U,
                  .Control = (32U << 10U) | (1U << 16U) | (UInt32{slot} << 24U)});
        }
    }

    UInt32 Registers[0x2100U / 4U]{};
    Mapping Mappings[1152U]{};
    Size MappingCount{0U};
    Slot Slots[2U]{};
    Ring CommandRing{};
    UInt32 ContextBytes;
    UInt64 EventBase{0U};
    Size EventCount{0U};
    Size EventIndex{0U};
    UInt32 EventCycle{1U};
    UInt64 LastErdp{0U};
    X::Trb LastCommand{};
    X::Trb StopEvent{};
    UInt32 EndpointStops{0U};
    UInt32 StopCode{26U};
    bool StopEventAfterCommand{false};
    UInt32 Reads{0U};
    UInt32 Publishes{0U};
    UInt32 Observes{0U};
    UInt32 DoorbellCount{0U};
    UInt32 ErdpWrites{0U};
    UInt32 Resets{0U};
    UInt32 PortResets{0U};
    UInt32 LastReset{0U};
    UInt32 CommandCount{0U};
    UInt32 TransferCount{0U};
    UInt32 DefaultAddresses{0U};
    UInt32 Addresses{0U};
    UInt32 Evaluations{0U};
    UInt32 Configurations{0U};
    UInt32 EndpointResets{0U};
    UInt32 DequeueSets{0U};
    UInt32 ClearHalts{0U};
    UInt32 Disabled{0U};
    UInt32 LinkFetches{0U};
    UInt32 EventWraps{0U};
    UInt32 EventRingFullCount{0U};
    UInt32 LastInterval{0U};
    UInt32 LastPacket{0U};
    UInt32 LastBurst{0U};
    UInt32 LastPayload{0U};
    UInt32 NextCode{1U};
    const UInt8* ConfigurationBytes{nullptr};
    Size ConfigurationLength{0U};
    UInt8 UnsupportedClass{0U}; // first root device; other roots retain HID descriptors
    UInt32 ProtocolMask{0U};
    UInt32 IdleMask{0U};
    Size BulkOutBytes{0U};
    UInt8 KeyboardReport[8U]{2U, 0U, 4U, 0U, 0U, 0U, 0U, 0U};
    UInt8 MouseReport[8U]{1U, 5U, 0xFDU, 0U};
    Size MouseReportLength{4U};
    UInt8 MouseEndpoint{0U}; // DCI answering mouse reports; zero keeps the port rule
    const UInt8* DeviceBytes{nullptr};
    UInt8 StallRequest{0U};  // bRequest answered with STALL on EP0; zero for none
    UInt8 StallIndex{0xFFU}; // its wIndex low byte, or 0xFF for any
    UInt32 StalledRequests{0U};
    UInt32 LastEvaluateEndpoint{0U}; // input EP0 dword 1 of the last Evaluate Context
    UInt32 PendingSpeed[255U]{};
    bool Good{true};
    bool HoldHalt{false};
    bool HoldBios{false};
    bool SuppressStoppedEvent{false};
    bool HoldReset{false};
    bool HoldPortReset{false};
    bool HoldCommands{false};
    bool HoldTransfers{false};
    bool StallNext{false};
    bool ShortNext{false};
};

struct Fixture
{
    explicit Fixture(const bool largeContexts = false, const UInt32 scratchpads = 1U,
                     const UInt64 base = 0x100000U) noexcept
        : Model{largeContexts, scratchpads}, Controller{Model}
    {
        Memory = {.Dcbaa = Map(base, Dcbaa, sizeof(Dcbaa)),
                  .CommandRing = Map(base + 0x10000U, Command, sizeof(Command)),
                  .EventRing = Map(base + 0x20000U, Events, sizeof(Events)),
                  .Erst = Map(base + 0x30000U, Erst, sizeof(Erst)),
                  .ScratchpadArray = Map(base + 0x40000U, ScratchArray, sizeof(ScratchArray)),
                  .Scratchpads = ScratchPages,
                  .ScratchpadCount = scratchpads,
                  .Ports = Ports,
                  .PortCount = 2U};
        ScratchPages[0U] = Map(base + 0x50000U, Scratch, sizeof(Scratch));
        for (Size i{0U}; i < 2U; ++i)
        {
            const UInt64 address = base + 0x60000U + i * 0x50000U;
            Ports[i] = {.InputContext = Map(address, Inputs[i], sizeof(Inputs[i])),
                        .DeviceContext = Map(address + 0x10000U, Outputs[i], sizeof(Outputs[i])),
                        .ControlRing = Map(address + 0x20000U, Controls[i], sizeof(Controls[i])),
                        .Buffer = Map(address + 0x40000U, Buffers[i], sizeof(Buffers[i])),
                        .Pipes = EndpointMappings[i],
                        .PipeCount = 1U};
            EndpointMappings[i][0U] = {
                .Ring = Map(address + 0x30000U, Interrupts[i], sizeof(Interrupts[i])),
                .Buffer = Map(base + 0x100000U + i * 0x10000U, InterruptBuffers[i], sizeof(InterruptBuffers[i]))};
        }
    }
    [[nodiscard]] X::DmaMemory Map(const UInt64 address, UInt8* const cpu, const Size bytes) noexcept
    {
        const X::DmaMemory memory{.Address = address, .Cpu = cpu, .Bytes = bytes};
        Model.Map(memory);
        return memory;
    }
    FakeXhci Model;
    X::XhciController Controller;
    X::ControllerMemory Memory{};
    X::PortMemory Ports[2U]{};
    X::EndpointMemory EndpointMappings[2U][1U]{};
    UInt8 InterruptBuffers[2U][1024U]{};
    X::DmaMemory ScratchPages[1U]{};
    UInt8 Dcbaa[64U]{};
    UInt8 Command[256U]{};
    UInt8 Events[256U]{};
    UInt8 Erst[64U]{};
    UInt8 ScratchArray[256U]{};
    UInt8 Scratch[4096U]{};
    UInt8 Inputs[2U][2112U]{};
    UInt8 Outputs[2U][2048U]{};
    UInt8 Controls[2U][256U]{};
    UInt8 Interrupts[2U][256U]{};
    UInt8 Buffers[2U][1024U]{};
};
} // namespace Asteria::Test::Usb
#endif
