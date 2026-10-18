// SPDX-License-Identifier: Apache-2.0

#include "Platform/Device/UsbHostPublication.hpp"

#include "Platform/Firmware/Diagnostic.hpp"

namespace Asteria::Platform::Device
{
namespace
{
constexpr Foundation::UInt8 PublicationStep{1U};
constexpr Foundation::UInt8 CapacityStep{3U};
constexpr Foundation::UInt8 WithheldStep{4U};
constexpr Foundation::UInt8 RestrictedStep{5U};

[[nodiscard]] bool IsUsbHost(const Kernel::Device::PciFunctionDescriptor& descriptor) noexcept
{
    return (descriptor.ClassCode == 0x0CU) && (descriptor.Subclass == 0x03U) &&
           ((descriptor.ProgrammingInterface == 0x00U) || (descriptor.ProgrammingInterface == 0x10U) ||
            (descriptor.ProgrammingInterface == 0x20U));
}

[[nodiscard]] bool SameDevice(const Kernel::Device::PciFunctionAddress left,
                              const Kernel::Device::PciFunctionAddress right) noexcept
{
    return (left.GetSegment() == right.GetSegment()) && (left.GetBus() == right.GetBus()) &&
           (left.GetDevice() == right.GetDevice());
}

// "ASTERIA:USB:<what><bbddf>[:<step>:<err>]\r\n" on the diagnostic channel.
void Report(const char* const what, const Kernel::Device::PciFunctionAddress address, const bool skipped,
            const Foundation::UInt8 step, const Foundation::UInt8 error) noexcept
{
    constexpr char Digits[] = "0123456789ABCDEF";
    char line[64]{};
    Foundation::Size length{0U};
    const auto append = [&](const char* text) noexcept
    {
        for (; (*text != '\0') && (length < sizeof(line) - 1U); ++text)
        {
            line[length++] = *text;
        }
    };
    const auto hex = [&](const Foundation::UInt64 value, const Foundation::Size digits) noexcept
    {
        for (Foundation::Size shift{digits * 4U}; shift != 0U; shift -= 4U)
        {
            if (length < sizeof(line) - 1U)
            {
                line[length++] = Digits[(value >> (shift - 4U)) & 0xFU];
            }
        }
    };
    append("ASTERIA:USB:");
    append(what);
    hex(address.GetBus(), 2U);
    hex(address.GetDevice(), 2U);
    hex(address.GetFunction(), 1U);
    if (skipped)
    {
        append(":");
        hex(step, 1U);
        append(":");
        hex(error, 2U);
    }
    append("\r\n");
    Firmware::WriteDiagnosticBytes(reinterpret_cast<const Foundation::UInt8*>(line), length);
}

void Skip(IUsbHostPublicationSite& site, const Kernel::Device::PciFunctionAddress address, const Foundation::UInt8 step,
          const Foundation::UInt8 error) noexcept
{
    site.Record(address, 0U, step, error);
    Report("SKIPPED:", address, true, step, error);
}
} // namespace

Foundation::Size PublishUsbHosts(const UsbHostPublicationInput& input,
                                 AssignableDevice (&output)[Abi::PagerUsbHostControllers]) noexcept
{
    if ((input.Runtime == nullptr) || (input.Site == nullptr) || (input.Resources == nullptr))
    {
        return 0U;
    }
    PciEcamRuntime& runtime = *input.Runtime;
    IUsbHostPublicationSite& site = *input.Site;
    PciEcamRuntimeSummary summary{};
    if (runtime.GetSummary(summary) != PciEcamFunctionQueryError::None)
    {
        return 0U;
    }
    Foundation::Size published{0U};
    for (Foundation::Size first{0U}; first < summary.PublishedFunctionCount; ++first)
    {
        PciFunctionPublication leader{};
        if ((runtime.GetPublishedFunction(first, leader) != PciEcamFunctionQueryError::None) ||
            !IsUsbHost(leader.Descriptor))
        {
            continue;
        }
        // A device is handled at its first USB host function in catalog
        // order; later functions of the same device were handled with it.
        bool handled{false};
        Foundation::Size members{0U};
        for (Foundation::Size index{0U}; index < summary.PublishedFunctionCount; ++index)
        {
            PciFunctionPublication other{};
            if ((runtime.GetPublishedFunction(index, other) != PciEcamFunctionQueryError::None) ||
                !IsUsbHost(other.Descriptor) || !SameDevice(other.Descriptor.Address, leader.Descriptor.Address))
            {
                continue;
            }
            handled = handled || (index < first);
            ++members;
        }
        if (handled)
        {
            continue;
        }
        const bool fits = members <= (Abi::PagerUsbHostControllers - published);
        for (Foundation::Size index{first}; index < summary.PublishedFunctionCount; ++index)
        {
            PciFunctionPublication member{};
            if ((runtime.GetPublishedFunction(index, member) != PciEcamFunctionQueryError::None) ||
                !IsUsbHost(member.Descriptor) || !SameDevice(member.Descriptor.Address, leader.Descriptor.Address))
            {
                continue;
            }
            const Kernel::Device::PciFunctionAddress address = member.Descriptor.Address;
            if (!fits)
            {
                Skip(site, address, CapacityStep,
                     static_cast<Foundation::UInt8>(PciEcamRuntimeError::FunctionCapacityExceeded));
                continue;
            }
            if (!site.Permits(member.Descriptor))
            {
                Skip(site, address, WithheldStep, 0U);
                continue;
            }
            if (!runtime.IsAssignable(address))
            {
                Skip(site, address, RestrictedStep, 0U);
                continue;
            }
            Firmware::AhciPciResource firmware{};
            const bool captured = site.FirmwareBar(address, firmware);
            const auto device =
                runtime.PublishUsbHostDevice(address, captured ? &firmware : nullptr, input.PhysicalAddressBits,
                                             input.ResourceIdentity, *input.Resources);
            if (!device.HasValue())
            {
                Skip(site, address, PublicationStep, static_cast<Foundation::UInt8>(device.GetError()));
                continue;
            }
            output[published++] = device.GetValue();
            site.Record(address, static_cast<Foundation::UInt8>(published), 0U, 0U);
            Report("PUBLISHED:", address, false, 0U, 0U);
        }
    }
    return published;
}
} // namespace Asteria::Platform::Device
