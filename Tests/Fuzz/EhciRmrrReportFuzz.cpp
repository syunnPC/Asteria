// SPDX-License-Identifier: Apache-2.0
#include "Inventory/EhciRmrrReport.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace
{
using Asteria::Foundation::UInt16;
using Asteria::Foundation::UInt32;
using Asteria::Foundation::UInt64;
[[nodiscard]] UInt32 Word(const std::uint8_t* bytes) noexcept
{
    return UInt32{bytes[0U]} | (UInt32{bytes[1U]} << 8U) | (UInt32{bytes[2U]} << 16U) | (UInt32{bytes[3U]} << 24U);
}
[[nodiscard]] UInt64 Quad(const std::uint8_t* bytes) noexcept
{
    return UInt64{Word(bytes)} | (UInt64{Word(bytes + 4U)} << 32U);
}
void Store(std::uint8_t* bytes, const UInt32 value) noexcept
{
    for (std::size_t index{0U}; index < 4U; ++index)
    {
        bytes[index] = static_cast<std::uint8_t>((value >> (index * 8U)) & 255U);
    }
}
// Input: the BAR extent (4 bytes), 256 configuration bytes, then register
// bytes; reads past the input are all-ones. Every access is bounded the way
// the formatter promises, and every capability walk is finite.
class Function final : public Asteria::Inventory::IEhciEvidenceAccess, public Asteria::Inventory::IPchSmiAccess
{
public:
    Function(const std::uint8_t* bytes, const std::size_t size) noexcept
        : m_bytes{bytes}, m_size{size}, m_extent{Word(bytes)}
    {
    }
    [[nodiscard]] UInt32 ReadConfiguration(const UInt16 offset) noexcept override
    {
        if ((offset >= 0x100U) || ((offset & 3U) != 0U) || (++m_reads > 512U))
        {
            std::abort();
        }
        return At(4U + offset);
    }
    [[nodiscard]] UInt32 ReadRegister(const UInt32 offset) noexcept override
    {
        if (((offset & 3U) != 0U) || (UInt64{offset} + 4U > m_extent) || (++m_reads > 512U))
        {
            std::abort();
        }
        return At(4U + 0x100U + offset);
    }
    [[nodiscard]] UInt32 ReadLpcConfiguration(const UInt16 offset) noexcept override
    {
        return ReadConfiguration(offset);
    }
    [[nodiscard]] UInt32 ReadPort32(const UInt16) noexcept override
    {
        return At(0U);
    }
    [[nodiscard]] UInt64 GetExtent() const noexcept
    {
        return m_extent;
    }

private:
    [[nodiscard]] UInt32 At(const std::size_t offset) const noexcept
    {
        return ((offset <= m_size) && (m_size - offset >= 4U)) ? Word(m_bytes + offset) : 0xFFFF'FFFFU;
    }
    const std::uint8_t* m_bytes;
    std::size_t m_size;
    UInt64 m_extent;
    UInt32 m_reads{0U};
};
} // namespace
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, const std::size_t size) noexcept
{
    if (size < 4U)
    {
        return 0;
    }
    Function function{data, size};
    char bytes[2049U]{};
    bytes[2048U] = '!';
    Asteria::Inventory::ReportText text{bytes, 2048U};
    const Asteria::Inventory::EhciEvidenceFunction identity{0U,
                                                            0U,
                                                            0x1DU,
                                                            0U,
                                                            {},
                                                            0x8086U,
                                                            static_cast<UInt16>((size & 1U) != 0U ? 0x1C26U : 0x293AU),
                                                            0U,
                                                            function.GetExtent()};
    Asteria::Inventory::FormatEhciEvidence(text, function, identity);
    Asteria::Inventory::FormatPchSmi(text, function, size);
    Asteria::Inventory::EhciEvidenceMemory memory[8U]{};
    std::size_t count{0U};
    for (std::size_t offset{4U}; (offset + 20U <= size) && (count < 8U); offset += 20U)
    {
        memory[count++] = {Quad(data + offset), Quad(data + offset + 8U), Word(data + offset + 16U), 0U};
    }
    const UInt64 base = (count == 0U) ? 0U : memory[0U].Start;
    const UInt64 limit = base + ((size & 0xFFFFU) * 4096U) + 4095U;
    Asteria::Inventory::FormatRmrrMemory(text, 0U, base, (limit < base) ? ~UInt64{0U} : limit, memory, count);
    if ((text.Bytes() > 2048U) || (bytes[2048U] != '!'))
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
    // Synthetic PCI PM and EHCI capability chains, followed by loops and a
    // function without memory decode; all MMIO is bounded by a 1 KiB BAR.
    std::uint8_t bytes[4U + 0x100U + 0x400U]{};
    Store(bytes, 0x400U);
    Store(bytes + 4U + 0x00U, 0x1C2D'8086U);
    Store(bytes + 4U + 0x04U, 0x0290'0006U);
    Store(bytes + 4U + 0x34U, 0x48U);
    Store(bytes + 4U + 0x40U, 0x0000'0401U);
    Store(bytes + 4U + 0x44U, 0x80U);
    Store(bytes + 4U + 0x48U, 0x0000'5001U);
    Store(bytes + 4U + 0x50U, 0x0000'000AU);
    Store(bytes + 4U + 0x80U, 0x0101'0001U);
    Store(bytes + 4U + 0x84U, 0x2000'E03FU);
    Store(bytes + 4U + 0x70U, 0x0102'0FFFU);
    Store(bytes + 4U + 0x100U + 0x00U, 0x0100'0020U);
    Store(bytes + 4U + 0x100U + 0x04U, 0x0020'0006U);
    Store(bytes + 4U + 0x100U + 0x08U, 0x0000'8001U);
    Store(bytes + 4U + 0x100U + 0x20U, 0x0001'0031U);
    emit(bytes, sizeof(bytes));
    Store(bytes + 4U + 0x80U, 0x0000'8002U);
    Store(bytes + 4U + 0x48U, 0x0000'4802U);
    emit(bytes, sizeof(bytes));
    Store(bytes + 4U + 0x04U, 0x0290'0004U);
    emit(bytes, sizeof(bytes) - 1U);
    Store(bytes, 0x10U);
    emit(bytes, 64U);
}
