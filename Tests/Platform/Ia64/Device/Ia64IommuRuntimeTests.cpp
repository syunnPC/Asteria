// SPDX-License-Identifier: Apache-2.0
#include "Platform/Ia64/Device/Ia64IommuRuntime.hpp"

#include <memory>

#include "Support/TestContext.hpp"

namespace Asteria::Test
{
namespace
{
using TestSupport::TestContext;
using Foundation::UInt8;
using Foundation::UInt16;
using Foundation::UInt32;
using Foundation::UInt64;
using Width = Hal::Device::MemoryMappedIoWidth;
using ReadError = Hal::Device::MemoryMappedIoReadError;
using WriteError = Hal::Device::MemoryMappedIoWriteError;

// Independent type-zero PCI model with a 64-bit aperture in process memory.
class BarModel final : public Hal::Device::IPciConfigurationSpace, public Hal::Device::IPciConfigurationWriter
{
public:
    BarModel() noexcept
    {
        const UInt64 base = reinterpret_cast<Foundation::UIntPointer>(Bytes);
        Low = static_cast<UInt32>(base & 0xFFFF'FFFFU) | 4U;
        High = static_cast<UInt32>(base >> 32U);
    }
    [[nodiscard]] Hal::Device::PciConfigurationReadError Read32(Kernel::Device::PciFunctionAddress, const UInt16 offset,
                                                                UInt32& output) noexcept override
    {
        output = 0U;
        if (offset == 0x10U)
        {
            output = (Low == 0xFFFF'FFFFU) ? 0xFFFF'F004U : Low;
        }
        if (offset == 0x14U)
        {
            output = (High == 0xFFFF'FFFFU) ? 0xFFFF'FFFFU : High;
        }
        return Hal::Device::PciConfigurationReadError::None;
    }
    [[nodiscard]] Hal::Device::PciConfigurationWriteError
    Write32(Kernel::Device::PciFunctionAddress, const UInt16 offset, const UInt32 value) noexcept override
    {
        if (offset == 0x10U)
        {
            Low = value;
        }
        if (offset == 0x14U)
        {
            High = value;
        }
        return Hal::Device::PciConfigurationWriteError::None;
    }
    alignas(4096) UInt8 Bytes[4096U]{};
    UInt32 Low{0U};
    UInt32 High{0U};
};

void TestAperture(TestContext& context)
{
    auto model = std::make_unique<BarModel>();
    Platform::Ia64::Device::Ia64PhysicalMemoryAccess access{};
    Platform::Ia64::Device::Ia64VirtioMmio mmio{access};
    const UInt32 low = model->Low;
    const UInt32 high = model->High;
    ASTERIA_EXPECT(context, "BAR probing restores both halves and discovers the full physical aperture",
                   mmio.Discover(*model, *model, {0U, 0U, 7U, 0U}, 63U), model->Low == low, model->High == high,
                   mmio.GetBars()[0U].Base == reinterpret_cast<Foundation::UIntPointer>(model->Bytes),
                   mmio.GetBars()[0U].Length == sizeof(model->Bytes), mmio.GetBars()[1U].Length == 0U);
    const UInt64 base = mmio.GetBars()[0U].Base;
    UInt64 value{0U};
    ASTERIA_EXPECT(context, "byte and word transactions retain their width and neighboring bytes",
                   mmio.Write(base + 1U, Width::Byte, 0xA5U) == WriteError::None,
                   mmio.Write(base + 2U, Width::Word, 0x1234U) == WriteError::None,
                   mmio.Read(base + 1U, Width::Byte, value) == ReadError::None, value == 0xA5U,
                   mmio.Read(base + 2U, Width::Word, value) == ReadError::None, value == 0x1234U,
                   model->Bytes[0U] == 0U, model->Bytes[1U] == 0xA5U, model->Bytes[2U] == 0x34U,
                   model->Bytes[3U] == 0x12U, model->Bytes[4U] == 0U);
    value = 0xCAFEU;
    ASTERIA_EXPECT(context, "misalignment, overflow, unsupported widths and missing authority perform no transaction",
                   mmio.Read(base + 1U, Width::Word, value) == ReadError::InvalidAddress, value == 0xCAFEU,
                   mmio.Read(base + sizeof(model->Bytes), Width::Byte, value) == ReadError::InvalidAddress,
                   mmio.Read(~UInt64{0U}, Width::Word, value) == ReadError::InvalidAddress,
                   mmio.Write(base, Width::QuadWord, 0U) == WriteError::UnsupportedWidth,
                   mmio.Write(base + 1U, Width::Byte, 256U) == WriteError::ValueOutOfRange,
                   mmio.Write(base + 2U, Width::Word, 65536U) == WriteError::ValueOutOfRange, model->Bytes[0U] == 0U,
                   model->Bytes[1U] == 0xA5U, model->Bytes[2U] == 0x34U);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    Asteria::TestSupport::TestContext context{"Ia64IommuRuntimeTests"};
    Asteria::Test::TestAperture(context);
    return context.Finish();
}
