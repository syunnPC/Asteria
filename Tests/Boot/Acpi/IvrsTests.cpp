// SPDX-License-Identifier: Apache-2.0
#include "Boot/Acpi/Ivrs.hpp"
#include "Platform/Amd64/Device/AmdViTopology.hpp"

#include <array>

#include "Support/TestContext.hpp"

namespace
{
using Asteria::Foundation::UInt8;
using Asteria::Foundation::UInt64;
using Asteria::Foundation::Size;
using Asteria::Boot::Acpi::IvrsTopology;
namespace Acpi = Asteria::Boot::Acpi;
namespace Device = Asteria::Platform::Amd64::Device;

class Bytes final
{
public:
    void Resize(const Size size) noexcept
    {
        for (Size index{m_size}; index < size; ++index)
        {
            m_bytes[index] = 0U;
        }
        m_size = size;
    }
    void Reset() noexcept
    {
        m_size = 0U;
        Resize(48U);
    }
    void PopBack() noexcept
    {
        --m_size;
    }
    [[nodiscard]] Size GetSize() const noexcept
    {
        return m_size;
    }
    [[nodiscard]] UInt8* Data() noexcept
    {
        return m_bytes;
    }
    [[nodiscard]] UInt8& operator[](const Size index) noexcept
    {
        return m_bytes[index];
    }

private:
    UInt8 m_bytes[4096U]{};
    Size m_size{0U};
};

void Put(Bytes& bytes, const Size offset, const UInt64 value, const Size count)
{
    for (Size index{0U}; index < count; ++index)
    {
        bytes[offset + index] = static_cast<UInt8>(value >> (index * 8U));
    }
}

void Checksum(Bytes& bytes)
{
    Put(bytes, 4U, bytes.GetSize(), 4U);
    bytes[9U] = 0U;
    UInt8 sum{0U};
    for (Size index{0U}; index < bytes.GetSize(); ++index)
    {
        sum = static_cast<UInt8>(sum + bytes[index]);
    }
    bytes[9U] = static_cast<UInt8>(0U - sum);
}

void Table(Bytes& bytes)
{
    bytes.Reset();
    bytes[0U] = 'I';
    bytes[1U] = 'V';
    bytes[2U] = 'R';
    bytes[3U] = 'S';
    bytes[8U] = 2U;
}

Size Unit(Bytes& bytes, const UInt8 type, const UInt64 base = 0xFED80000U)
{
    const Size offset = bytes.GetSize();
    bytes.Resize(offset + ((type == 0x10U) ? 24U : 40U));
    bytes[offset] = type;
    Put(bytes, offset + 2U, bytes.GetSize() - offset, 2U);
    Put(bytes, offset + 4U, 0x30U, 2U);
    Put(bytes, offset + 6U, 0x40U, 2U);
    Put(bytes, offset + 8U, base, 8U);
    if (type != 0x10U)
    {
        Put(bytes, offset + 24U, 0x2051U, 8U);
    }
    return offset;
}

void Entry(Bytes& bytes, const Size unit, const UInt8 type, const UInt64 requester, const UInt64 extended = 0U)
{
    const Size offset = bytes.GetSize();
    const Size length = (type == 0xF0U) ? 22U : ((type < 0x40U) ? 4U : 8U);
    bytes.Resize(offset + length);
    bytes[offset] = type;
    Put(bytes, offset + 1U, requester, 2U);
    bytes[offset + 3U] = 0x80U;
    if (length >= 8U)
    {
        Put(bytes, offset + 4U, extended, 4U);
    }
    Put(bytes, unit + 2U, bytes.GetSize() - unit, 2U);
}

void Memory(Bytes& bytes, const UInt8 type, const UInt8 flags, const UInt64 first, const UInt64 last)
{
    const Size offset = bytes.GetSize();
    bytes.Resize(offset + 32U);
    bytes[offset] = type;
    bytes[offset + 1U] = flags;
    Put(bytes, offset + 2U, 32U, 2U);
    Put(bytes, offset + 4U, first, 2U);
    Put(bytes, offset + 6U, last, 2U);
    Put(bytes, offset + 16U, 0x80000000U, 8U);
    Put(bytes, offset + 24U, 0x2000U, 8U);
}

class Configuration final : public Asteria::Hal::Device::IPciConfigurationSpace
{
public:
    [[nodiscard]] Asteria::Hal::Device::PciConfigurationReadError
    Read32(const Asteria::Kernel::Device::PciFunctionAddress address, const Asteria::Foundation::UInt16 offset,
           Asteria::Foundation::UInt32& output) noexcept override
    {
        output = 0U;
        if ((offset == 8U) && (address.GetDevice() == 0U) && (address.GetFunction() == 0U))
        {
            output = 0x06000000U;
        }
        if ((offset == 8U) && (address.GetDevice() == 3U))
        {
            output = 0x06040000U;
        }
        if ((offset == 0x18U) && (address.GetDevice() == 3U))
        {
            output = 0x00010100U;
        }
        if ((offset == 8U) && (address.GetDevice() == 2U))
        {
            output = 3U << 24U;
        }
        if ((offset == 0x0CU) && (address.GetDevice() == 3U))
        {
            output = 1U << 16U;
        }
        return Asteria::Hal::Device::PciConfigurationReadError::None;
    }
};

void TestDmaExclusions(Asteria::TestSupport::TestContext& context)
{
    // Physical memory management keeps even IVMD ranges ignored by routing.
    static Bytes bytes;
    static Asteria::Boot::BootDmaExclusions exclusions;
    Table(bytes);
    static_cast<void>(Unit(bytes, 0x40U));
    const Size retained = bytes.GetSize();
    Memory(bytes, 0x22U, 8U, 1U, 2U);
    Put(bytes, retained + 8U, 7U, 2U);
    Put(bytes, retained + 16U, 0x80000001U, 8U);
    Put(bytes, retained + 24U, 0x1001U, 8U);
    Memory(bytes, 0x21U, 0U, 1U, 0U);
    Memory(bytes, 0x22U, 0x88U, 2U, 1U);
    const Size empty = bytes.GetSize();
    Memory(bytes, 0x20U, 8U, 0U, 0U);
    Put(bytes, empty + 24U, 0U, 8U);
    Checksum(bytes);
    exclusions = {};
    Acpi::CollectIvrsExclusions(bytes.Data(), bytes.GetSize(), exclusions);
    ASTERIA_EXPECT(context, "IVMD exclusions count every declaration and retain inert and malformed ranges",
                   exclusions.DeclaredCount == 4U, exclusions.Count == 3U, exclusions.Ranges[0U].Base == 0x80000000U,
                   exclusions.Ranges[0U].Limit == 0x80001FFFU, exclusions.Ranges[0U].Segment == 7U,
                   exclusions.Ranges[0U].Kind == Asteria::Boot::BootDmaExclusionIvmd,
                   exclusions.Ranges[0U].Malformed == 0U, exclusions.Ranges[1U].Malformed == 1U,
                   exclusions.Ranges[2U].Malformed == 1U, Asteria::Boot::IsValidBootDmaExclusions(exclusions));
    // A framing fault retains the preceding declarations, like DMAR.
    Put(bytes, empty + 2U, 33U, 2U);
    exclusions = {};
    Acpi::CollectIvrsExclusions(bytes.Data(), bytes.GetSize(), exclusions);
    ASTERIA_EXPECT(context, "a framing fault keeps preceding IVMD exclusions", exclusions.DeclaredCount == 3U,
                   exclusions.Count == 3U);
    Table(bytes);
    for (Size index{0U}; index < 65U; ++index)
    {
        Memory(bytes, 0x20U, 8U, 0U, 0U);
    }
    Checksum(bytes);
    exclusions = {};
    Acpi::CollectIvrsExclusions(bytes.Data(), bytes.GetSize(), exclusions);
    ASTERIA_EXPECT(context, "an IVMD exclusion overflow stays bounded and stops the handoff",
                   exclusions.Count == Asteria::Boot::BootDmaExclusionMaximumCount, exclusions.DeclaredCount == 65U,
                   Asteria::Boot::IsValidBootDmaExclusions(exclusions),
                   !Asteria::Boot::IsCompleteBootDmaExclusions(exclusions));
}
} // Namespace

int main()
{
    Asteria::TestSupport::TestContext context{"Ivrs"};
    TestDmaExclusions(context);
    for (const UInt8 type : std::array<UInt8, 3U>{0x10U, 0x11U, 0x40U})
    {
        static Bytes bytes;
        Table(bytes);
        const Size unit = Unit(bytes, type);
        Entry(bytes, unit, 2U, 0x10U);
        Entry(bytes, unit, 3U, 0x100U);
        Entry(bytes, unit, 4U, 0x1FFU);
        Entry(bytes, unit, 0x42U, 0x200U, 0x00030000U);
        Entry(bytes, unit, 0x43U, 0x400U, 0x00050000U);
        Entry(bytes, unit, 4U, 0x4FFU);
        Entry(bytes, unit, 0x46U, 0x600U, 0x10000000U);
        Entry(bytes, unit, 0x47U, 0x700U, 0x10000000U);
        Entry(bytes, unit, 4U, 0x7FFU);
        Entry(bytes, unit, 0x48U, 0U, 0x01002002U);
        Entry(bytes, unit, 0x48U, 0U, 0x02002103U);
        Entry(bytes, unit, 0xF0U, 0x900U);
        Memory(bytes, 0x20U, 7U, 0U, 0U);
        Memory(bytes, 0x21U, 3U, 0x600U, 0U);
        Memory(bytes, 0x22U, 8U, 0x700U, 0x7FFU);
        Checksum(bytes);
        IvrsTopology output{};
        ASTERIA_EXPECT(context, "IVHD formats retain scopes, aliases, EFR, special devices and IVMD permissions",
                       Acpi::ParseIvrs(bytes.Data(), bytes.GetSize(), output), output.UnitCount == 1U,
                       output.Type == type, output.RegisterBase == 0xFED80000U, output.IommuDevice == 0x30U,
                       output.DeviceCount == 8U, output.Devices[1U].Last == 0x1FFU, output.Devices[2U].Alias == 0x300U,
                       output.Devices[3U].Alias == 0x500U, output.Devices[4U].ExtendedFlags == 0x10000000U,
                       output.Devices[6U].Variety == 1U, output.Devices[7U].Variety == 2U, output.SkippedHid == 1U,
                       output.Malformed == 0U, output.ExtendedFeatures == ((type == 0x10U) ? 0U : 0x2051U),
                       output.MemoryCount == 3U, output.Memory[0U].Last == 0xFFFFU, output.Memory[1U].First == 0x600U,
                       output.Memory[1U].Flags == 3U, output.Memory[2U].Flags == 8U,
                       output.Memory[2U].Limit == 0x80001FFFU);
    }
    // AMD 48882 section 5.2.2.2 table 109: model the requested byte
    // interval separately from its enclosing 4-Kbyte mapping interval.
    for (UInt64 offset{0U}; offset < 4096U; ++offset)
    {
        static Bytes alignment;
        Table(alignment);
        static_cast<void>(Unit(alignment, 0x10U));
        const Size memory = alignment.GetSize();
        Memory(alignment, 0x21U, 7U, 0x10U, 0U);
        const UInt64 start = 0x8000'0000U + offset;
        const UInt64 length = 4097U - offset / 2U;
        Put(alignment, memory + 16U, start, 8U);
        Put(alignment, memory + 24U, length, 8U);
        Checksum(alignment);
        IvrsTopology range{};
        ASTERIA_EXPECT(context, "every firmware byte survives page enclosure",
                       Acpi::ParseIvrs(alignment.Data(), alignment.GetSize(), range), range.Malformed == 0U,
                       range.MemoryCount == 1U, range.Memory[0U].Base <= start,
                       range.Memory[0U].Limit >= start + length - 1U, start - range.Memory[0U].Base < 4096U,
                       range.Memory[0U].Limit - (start + length - 1U) < 4096U, (range.Memory[0U].Base % 4096U) == 0U,
                       (range.Memory[0U].Limit % 4096U) == 4095U);
    }
    // Byte-interval arithmetic at the full UInt64 boundary is independent
    // of any later platform physical-address limit (AMD table 109).
    const UInt64 starts[]{0U, ~UInt64{0U}, ~UInt64{0U}, ~UInt64{0U} - 1U};
    const UInt64 lengths[]{0U, 1U, 2U, 2U};
    for (Size index{0U}; index < 4U; ++index)
    {
        static Bytes boundary;
        Table(boundary);
        static_cast<void>(Unit(boundary, 0x10U));
        const Size memory = boundary.GetSize();
        Memory(boundary, 0x21U, 7U, 0x10U, 0U);
        Put(boundary, memory + 16U, starts[index], 8U);
        Put(boundary, memory + 24U, lengths[index], 8U);
        Checksum(boundary);
        IvrsTopology result{};
        const bool accepted = index == 1U || index == 3U;
        ASTERIA_EXPECT(context, "zero/overflow refused; final address retained without wrap",
                       Acpi::ParseIvrs(boundary.Data(), boundary.GetSize(), result),
                       result.MemoryCount == (accepted ? 1U : 0U), result.Malformed == (accepted ? 0U : 1U),
                       !accepted || (result.Memory[0U].Base == (~UInt64{0U} & ~UInt64{4095U}) &&
                                     result.Memory[0U].Limit == ~UInt64{0U}));
    }
    static Bytes bytes;
    Table(bytes);
    Size unit = Unit(bytes, 0x10U);
    Entry(bytes, unit, 1U, 0U);
    unit = Unit(bytes, 0x40U);
    Entry(bytes, unit, 2U, 0x10U);
    Checksum(bytes);
    IvrsTopology output{};
    ASTERIA_EXPECT(context, "alternate descriptions are one unit and the highest type wins",
                   Acpi::ParseIvrs(bytes.Data(), bytes.GetSize(), output), output.UnitCount == 1U, output.Type == 0x40U,
                   output.DeviceCount == 1U, output.Devices[0U].First == 0x10U);
    static_cast<void>(Unit(bytes, 0x11U, 0xFED90000U));
    Checksum(bytes);
    ASTERIA_EXPECT(context, "a second distinct IOMMU is refused with a unit-count reason",
                   Acpi::ParseIvrs(bytes.Data(), bytes.GetSize(), output), output.UnitCount == 2U);
    output.PhysicalAddress = 0x1000U;
    ASTERIA_EXPECT(context, "selection preserves the multiple-unit refusal",
                   Device::SelectAmdViUnit(output).Error == Device::VtdTopologyError::UnitCount);
    Table(bytes);
    unit = Unit(bytes, 0x10U);
    Entry(bytes, unit, 4U, 0x10U);
    Entry(bytes, unit, 3U, 0x20U);
    Entry(bytes, unit, 4U, 0x10U);
    Entry(bytes, unit, 2U, 0x30U);
    Memory(bytes, 0x22U, 7U, 0x20U, 0x10U);
    Memory(bytes, 0x21U, 7U, 0x30U, 0U);
    Checksum(bytes);
    ASTERIA_EXPECT(context, "independently malformed entries are counted and valid records survive",
                   Acpi::ParseIvrs(bytes.Data(), bytes.GetSize(), output), output.Malformed == 3U,
                   output.DeviceCount == 1U, output.MemoryCount == 1U);
    bytes.PopBack();
    Checksum(bytes);
    ASTERIA_EXPECT(context, "bad framing clears all published topology",
                   !Acpi::ParseIvrs(bytes.Data(), bytes.GetSize(), output), output.DeviceCount == 0U,
                   output.UnitCount == 0U);
    Table(bytes);
    unit = Unit(bytes, 0x40U);
    for (Size index{0U}; index < 65U; ++index)
    {
        Entry(bytes, unit, 2U, index);
    }
    Checksum(bytes);
    ASTERIA_EXPECT(context, "device capacity is counted without indexing beyond bounded storage",
                   Acpi::ParseIvrs(bytes.Data(), bytes.GetSize(), output), output.DeviceCount == 65U);
    output.PhysicalAddress = 0x1000U;
    ASTERIA_EXPECT(context, "a partial device image cannot enable translation",
                   Device::SelectAmdViUnit(output).Error == Device::VtdTopologyError::ScopeCapacity);
    // Ryzen-shaped routing: one segment, endpoint unity range, display, bridge,
    // own IOMMU function and a shared alias. No topology invents authority.
    Table(bytes);
    unit = Unit(bytes, 0x40U);
    Entry(bytes, unit, 1U, 0U);
    Entry(bytes, unit, 0x42U, 0x28U, 0x00003800U);
    Memory(bytes, 0x21U, 7U, 0x20U, 0U);
    Checksum(bytes);
    ASTERIA_EXPECT(context, "Ryzen-shaped IVRS decodes", Acpi::ParseIvrs(bytes.Data(), bytes.GetSize(), output));
    output.PhysicalAddress = 0x1000U;
    Configuration configuration;
    constexpr Asteria::Kernel::Device::PciFunctionAddress functions[]{
        {0U, 0U, 1U, 0U}, {0U, 0U, 2U, 0U}, {0U, 0U, 3U, 0U}, {0U, 0U, 4U, 0U},
        {0U, 0U, 5U, 0U}, {0U, 0U, 6U, 0U}, {0U, 0U, 7U, 0U}};
    Device::VtdTopologyPlan plan{};
    ASTERIA_EXPECT(context, "only ordinary unreserved endpoints may be assigned or swept",
                   Device::ResolveAmdViTopology(output, configuration, functions, 7U, plan) ==
                       Device::VtdTopologyError::None,
                   plan.Functions[0U].Policy == Device::VtdFunctionPolicy::Assignable,
                   plan.Functions[1U].Policy == Device::VtdFunctionPolicy::Display,
                   plan.Functions[2U].Policy == Device::VtdFunctionPolicy::Bridge,
                   plan.Functions[3U].Policy == Device::VtdFunctionPolicy::ReservedMemory,
                   plan.Functions[4U].Policy == Device::VtdFunctionPolicy::Unserved,
                   plan.Functions[5U].Policy == Device::VtdFunctionPolicy::Unserved,
                   plan.Functions[6U].Policy == Device::VtdFunctionPolicy::Unserved,
                   Asteria::Platform::Device::ShouldSweepVtdFunction(plan.Functions[0U], plan),
                   !Asteria::Platform::Device::ShouldSweepVtdFunction(plan.Functions[3U], plan));
    Table(bytes);
    unit = Unit(bytes, 0x11U);
    Entry(bytes, unit, 1U, 0U);
    Memory(bytes, 0x20U, 7U, 0U, 0U);
    Checksum(bytes);
    ASTERIA_EXPECT(context, "IVMD all-devices records decode", Acpi::ParseIvrs(bytes.Data(), bytes.GetSize(), output));
    output.PhysicalAddress = 0x1000U;
    ASTERIA_EXPECT(context, "all-devices unity also preserves kernel-only display and bridge firmware contexts",
                   Device::ResolveAmdViTopology(output, configuration, functions, 7U, plan) ==
                       Device::VtdTopologyError::None,
                   plan.Functions[0U].Policy == Device::VtdFunctionPolicy::ReservedMemory,
                   plan.Functions[1U].Policy == Device::VtdFunctionPolicy::Display, plan.Functions[1U].Regions == 1U,
                   plan.Functions[2U].Policy == Device::VtdFunctionPolicy::Bridge, plan.Functions[2U].Regions == 1U,
                   plan.Functions[5U].Policy == Device::VtdFunctionPolicy::Unserved, plan.Functions[5U].Regions == 0U);
    Table(bytes);
    unit = Unit(bytes, 0x10U);
    Entry(bytes, unit, 4U, 1U);
    Entry(bytes, unit, 0xF0U, 0U);
    unit = Unit(bytes, 0x40U);
    Entry(bytes, unit, 3U, 3U);
    Entry(bytes, unit, 4U, 0xFFFEU);
    Entry(bytes, unit, 0x43U, 0xFF00U, 0x0000FF00U);
    Entry(bytes, unit, 4U, 0xFFFFU);
    Memory(bytes, 0x21U, 0U, 0x999U, 0U);
    Memory(bytes, 0x21U, 1U, 0x999U, 0U);
    const Size memory = bytes.GetSize();
    Memory(bytes, 0x21U, 3U, 0x999U, 0U);
    Put(bytes, memory + 16U, 0x80000001U, 8U);
    Put(bytes, memory + 24U, 0x1001U, 8U);
    Checksum(bytes);
    ASTERIA_EXPECT(context, "replacement discards lower-image errors; ignored IVMDs do not poison aligned unity maps",
                   Acpi::ParseIvrs(bytes.Data(), bytes.GetSize(), output), output.Malformed == 0U,
                   output.SkippedHid == 0U, output.MemoryCount == 1U, output.Memory[0U].Base == 0x80000000U,
                   output.Memory[0U].Limit == 0x80001FFFU, Device::AmdViDevicePages(output, functions, 7U) == 512U,
                   Device::AmdViReservedRegions(output, 0x999U) == 1U);
    constexpr Asteria::Kernel::Device::PciFunctionAddress conventional[]{{0U, 0U, 3U, 0U}, {0U, 1U, 1U, 0U}};
    output.PhysicalAddress = 0x1000U;
    ASTERIA_EXPECT(
        context, "AMD-Vi also blocks an unaliased conventional PCI descendant",
        Device::ResolveAmdViTopology(output, configuration, conventional, 2U, plan) == Device::VtdTopologyError::None,
        plan.Functions[1U].Policy == Device::VtdFunctionPolicy::Unserved,
        Device::ResolveAmdViTopology(output, configuration, conventional, Device::VtdUnit::MaximumRequesters + 1U,
                                     plan) == Device::VtdTopologyError::FunctionCapacity);
    // X870E-shaped: the IVHD range starts at 00:00.3 while an IVMD range
    // spans 00:00.0-0f:1f.7. The host bridge 00:00.0 is no requester; an
    // unserved endpoint in the range still refuses the unit.
    Table(bytes);
    unit = Unit(bytes, 0x40U);
    Entry(bytes, unit, 3U, 3U);
    Entry(bytes, unit, 4U, 0xFFFEU);
    Memory(bytes, 0x22U, 8U, 0U, 0x0FFFU);
    Checksum(bytes);
    ASTERIA_EXPECT(context, "X870E-shaped IVRS decodes", Acpi::ParseIvrs(bytes.Data(), bytes.GetSize(), output));
    output.PhysicalAddress = 0x1000U;
    constexpr Asteria::Kernel::Device::PciFunctionAddress hostBridge[]{{0U, 0U, 0U, 0U}, {0U, 0U, 1U, 0U}};
    ASTERIA_EXPECT(
        context, "an IVMD range over an unserved host bridge does not refuse the unit",
        Device::ResolveAmdViTopology(output, configuration, hostBridge, 2U, plan) == Device::VtdTopologyError::None,
        plan.Functions[0U].Policy == Device::VtdFunctionPolicy::Unserved, plan.Functions[0U].Regions == 0U,
        plan.Functions[1U].Policy == Device::VtdFunctionPolicy::ReservedMemory, plan.Functions[1U].Regions == 1U);
    constexpr Asteria::Kernel::Device::PciFunctionAddress unservedEndpoint[]{{0U, 0U, 0U, 0U}, {0U, 0U, 0U, 1U}};
    ASTERIA_EXPECT(context, "an IVMD range over an unserved endpoint still refuses the unit",
                   Device::ResolveAmdViTopology(output, configuration, unservedEndpoint, 2U, plan) ==
                       Device::VtdTopologyError::UnservedReservedMemory);
    return context.Finish();
}
