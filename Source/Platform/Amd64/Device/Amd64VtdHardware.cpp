// SPDX-License-Identifier: Apache-2.0

#include "Platform/Amd64/Device/Amd64VtdHardware.hpp"

#include "Platform/Amd64/Device/Amd64PhysicalMemoryAccess.hpp"
#include "Kernel/Diagnostics/StopDisplay.hpp"
#include "Kernel/Diagnostics/StopFont.hpp"
#include "Kernel/Diagnostics/StopTextWriter.hpp"
#include "Platform/Firmware/Diagnostic.hpp"

namespace Asteria::Platform::Amd64::Device
{
namespace
{
// Exclusive BSP boot output before user scanout or AP startup. Each character
// touches only its clipped 8x16 cell through the already-open physical window;
// it neither changes the firmware mode nor uses post-exit UEFI services.
constinit Foundation::UInt32 g_handoffColumn{0U};
constinit Foundation::UInt32 g_handoffRow{0U};

class HandoffWriter final : public Kernel::Diagnostics::StopTextWriter<HandoffWriter>
{
public:
    explicit HandoffWriter(const Boot::BootInfo* info) noexcept : m_info{info}
    {
    }

    void Put(const Foundation::UInt8 byte) noexcept
    {
        Platform::Firmware::WriteDiagnosticBytes(&byte, 1U);
        if (m_info == nullptr || byte == '\r')
        {
            return;
        }
        const auto& display = m_info->Display;
        if (Kernel::Diagnostics::StopDisplayBytes(display, display.FrameBufferBytes) == 0U)
        {
            return;
        }
        if (byte == '\n')
        {
            g_handoffColumn = 0U;
            ++g_handoffRow;
            return;
        }
        if (byte < 32U || byte > 126U)
        {
            return;
        }
        if (g_handoffColumn >= display.Width / 8U)
        {
            g_handoffColumn = 0U;
            ++g_handoffRow;
        }
        if (g_handoffRow >= display.Height / 16U)
        {
            return;
        }
        const Amd64PhysicalMemoryAccess memory;
        for (Foundation::UInt32 row{0U}; row < 16U; ++row)
        {
            const Foundation::UInt8 glyph = Kernel::Diagnostics::StopGlyphs[(byte - 32U) * 16U + row];
            for (Foundation::UInt32 column{0U}; column < 8U; ++column)
            {
                const Foundation::UInt64 pixel =
                    Foundation::UInt64{g_handoffRow * 16U + row} * display.PixelsPerScanLine + g_handoffColumn * 8U +
                    column;
                memory.WriteDoubleWord(display.FrameBufferBase + pixel * 4U,
                                       (glyph & (0x80U >> column)) != 0U ? 0x00FF'FFFFU : 0U);
            }
        }
        ++g_handoffColumn;
    }

private:
    const Boot::BootInfo* m_info;
};

template <typename Value>
[[nodiscard]] volatile Value* Register(const Foundation::UInt64 base, const Foundation::Size offset) noexcept
{
    return reinterpret_cast<volatile Value*>(static_cast<Foundation::UIntPointer>(base + offset));
}
} // namespace

void Amd64VtdRegisters::SetFirmwareMemoryPolicy(const Boot::BootInfo& info,
                                                const Platform::Device::VtdFirmwareMemoryOwnership& ownership) noexcept
{
    m_bootInfo = &info;
    m_ownership = ownership;
}

void Amd64VtdRegisters::ReportHandoff(const Platform::Device::VtdFirmwareState& state) noexcept
{
    HandoffWriter writer{m_bootInfo};
    writer.Text("VT-D handoff BASE=");
    writer.Hex(m_base);
    writer.Text(" GSTS=");
    writer.Code(state.Status);
    writer.Text("\r\nIQA=");
    writer.Hex(state.QueueAddress);
    writer.Text(" IQH=");
    writer.Hex(state.QueueHead);
    writer.Text(" IQT=");
    writer.Hex(state.QueueTail);
    writer.Text("\r\nFSTS=");
    writer.Code(state.FaultStatus);
    writer.Text("\r\n");
}

void Amd64VtdRegisters::ReportRefusal(const Platform::Device::VtdHandoffFailure failure) noexcept
{
    HandoffWriter writer{m_bootInfo};
    writer.Text("VT-D refused BASE=");
    writer.Hex(m_base);
    writer.Text(" REASON=");
    writer.Text(Platform::Device::VtdHandoffFailureName(failure));
    writer.Text("\r\n");
}

bool Amd64VtdRegisters::CanWriteFirmwareMemory(const Foundation::UInt64 address,
                                               const Foundation::UInt64 bytes) const noexcept
{
    const Amd64PhysicalMemoryAccess memory;
    return m_bootInfo != nullptr && bytes != 0U &&
           memory.IsPhysicalRangeImplemented(address, bytes, m_queueAddressBits) &&
           Platform::Device::VtdFirmwareQueueRangeIsWritable(*m_bootInfo, address, address + bytes - 1U, m_ownership);
}

void Amd64VtdRegisters::Bind(const Foundation::UInt64 base) noexcept
{
    m_base = base;
    // Intel SDM Vol. 2 CPUID leaves 80000000H/80000008H: do not form
    // a queue-memory mapping beyond this CPU's physical-address width.
    Foundation::UInt32 eax{0U};
    Foundation::UInt32 ebx{0U};
    Foundation::UInt32 ecx{0U};
    Foundation::UInt32 edx{0U};
    __asm__ __volatile__("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x8000'0000U), "c"(0U));
    m_queueAddressBits = 0U;
    if (eax >= 0x8000'0008U)
    {
        __asm__ __volatile__("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x8000'0008U), "c"(0U));
        const Foundation::UInt32 bits = eax & 0xFFU;
        if (bits >= 32U && bits <= 52U)
        {
            m_queueAddressBits = static_cast<Foundation::UInt8>(bits);
        }
    }
}

Foundation::UInt32 Amd64VtdRegisters::Read32(const Foundation::Size offset) const noexcept
{
    return *Register<Foundation::UInt32>(m_base, offset);
}

Foundation::UInt64 Amd64VtdRegisters::Read64(const Foundation::Size offset) const noexcept
{
    return *Register<Foundation::UInt64>(m_base, offset);
}

void Amd64VtdRegisters::Write32(const Foundation::Size offset, const Foundation::UInt32 value) noexcept
{
    *Register<Foundation::UInt32>(m_base, offset) = value;
}

void Amd64VtdRegisters::Write64(const Foundation::Size offset, const Foundation::UInt64 value) noexcept
{
    *Register<Foundation::UInt64>(m_base, offset) = value;
}

bool Amd64VtdRegisters::ReadQueueWord(const Foundation::UInt64 address, Foundation::UInt32& value) const noexcept
{
    const Amd64PhysicalMemoryAccess memory;
    // Boot physical-window contract: exclusive CPU0 access before closure.
    if ((address & 3U) != 0U || !memory.IsPhysicalRangeImplemented(address, 4U, m_queueAddressBits))
    {
        return false;
    }
    value = static_cast<Foundation::UInt32>(memory.ReadDoubleWord(address));
    return true;
}

bool Amd64VtdRegisters::WriteQueueWord(const Foundation::UInt64 address, const Foundation::UInt32 value) noexcept
{
    const Amd64PhysicalMemoryAccess memory;
    // Boot physical-window contract: exclusive CPU0 access before closure.
    if ((address & 3U) != 0U || !CanWriteFirmwareMemory(address, 4U))
    {
        return false;
    }
    memory.WriteDoubleWord(address, value);
    return true;
}

void Amd64CacheMaintenance::Flush(const volatile void* const line) noexcept
{
    __asm__ __volatile__("clflush (%0)" : : "r"(line) : "memory");
}

void Amd64CacheMaintenance::FlushTables(const volatile void* const start, const Foundation::Size bytes) noexcept
{
    const auto first = reinterpret_cast<Foundation::UIntPointer>(start) & ~(Amd64VtdFlushLineBytes - 1U);
    const auto end = reinterpret_cast<Foundation::UIntPointer>(start) + bytes;
    for (Foundation::UIntPointer line{first}; line < end; line += Amd64VtdFlushLineBytes)
    {
        Flush(reinterpret_cast<const volatile void*>(line));
    }
}

void Amd64CacheMaintenance::Fence() noexcept
{
    __asm__ __volatile__("mfence" : : : "memory");
}
} // namespace Asteria::Platform::Amd64::Device
