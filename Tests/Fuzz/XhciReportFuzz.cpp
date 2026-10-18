// SPDX-License-Identifier: Apache-2.0
#include "Inventory/XhciReport.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace
{
using Asteria::Foundation::UInt32;
[[nodiscard]] UInt32 Word(const std::uint8_t* bytes) noexcept
{
    return UInt32{bytes[0U]} | (UInt32{bytes[1U]} << 8U) | (UInt32{bytes[2U]} << 16U) | (UInt32{bytes[3U]} << 24U);
}
class Registers final : public Asteria::Inventory::IXhciReportRegisters
{
public:
    Registers(const std::uint8_t* bytes, const std::size_t size, const UInt32 barBytes) noexcept
        : m_bytes{bytes}, m_size{size}, m_barBytes{barBytes}
    {
    }
    [[nodiscard]] UInt32 Read32(const UInt32 offset) noexcept override
    {
        if ((offset & 3U) != 0U || offset > m_barBytes || m_barBytes - offset < 4U || ++m_reads > 521U)
        {
            std::abort();
        }
        return offset <= m_size && m_size - offset >= 4U ? Word(m_bytes + offset) : 0xFFFF'FFFFU;
    }

private:
    const std::uint8_t* m_bytes;
    std::size_t m_size;
    UInt32 m_barBytes;
    UInt32 m_reads{0U};
};
void Store(std::uint8_t* bytes, const UInt32 value) noexcept
{
    for (std::size_t index{0U}; index < 4U; ++index)
    {
        bytes[index] = static_cast<std::uint8_t>((value >> (index * 8U)) & 255U);
    }
}
} // namespace
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, const std::size_t size) noexcept
{
    if (size < 4U)
    {
        return 0;
    }
    const UInt32 barBytes = Word(data);
    Registers registers{data + 4U, size - 4U, barBytes};
    char bytes[513U]{};
    bytes[512U] = '!';
    Asteria::Inventory::ReportText text{bytes, 512U};
    Asteria::Inventory::FormatXhci(text, registers, barBytes);
    if ((text.Bytes() > 512U) || (bytes[512U] != '!'))
    {
        std::abort();
    }
    for (std::size_t index{0U}; index < text.Bytes(); ++index)
    {
        if ((bytes[index] != '\r') && (bytes[index] != '\n') && ((bytes[index] < 32) || (bytes[index] > 126)))
        {
            std::abort();
        }
    }
    return 0;
}
extern "C" void AsteriaFuzzSeeds(void (*emit)(const std::uint8_t*, std::size_t)) noexcept
{
    // The regression driver accepts 4 KiB seeds. Higher ports in the 255-root
    // aperture return all ones beyond this supplied prefix.
    std::uint8_t bytes[4096U]{};
    Store(bytes, 0x1500U);
    Store(bytes + 4U, 0x01200080U);
    Store(bytes + 8U, (255U << 24U) | 16U);
    Store(bytes + 20U, 0x40U << 16U);
    Store(bytes + 4U + 0x88U, 1U);
    Store(bytes + 4U + 0x100U, 1U | (4U << 8U) | (1U << 16U));
    Store(bytes + 4U + 0x110U, 2U | (2U << 24U));
    Store(bytes + 4U + 0x118U, (255U << 8U) | 1U);
    Store(bytes + 4U + 0x480U + 169U * 16U, 1U);
    emit(bytes, sizeof(bytes));
    Store(bytes + 4U + 0x100U, 0xFFFF'FFFFU);
    emit(bytes, sizeof(bytes));
    Store(bytes + 4U, 0xFFFF'FFFFU);
    emit(bytes, sizeof(bytes));
    Store(bytes, 31U);
    emit(bytes, sizeof(bytes));
}
