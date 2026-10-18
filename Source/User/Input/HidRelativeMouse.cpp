// SPDX-License-Identifier: Apache-2.0

#include "User/Input/HidRelativeMouse.hpp"
namespace Asteria::User::Input
{
namespace
{
[[nodiscard]] UInt32 Bits(const UInt8* const bytes, const UInt16 offset, const UInt8 width) noexcept
{
    UInt32 value{0U};
    for (UInt8 i{0U}; i < width; ++i)
    {
        value |= ((UInt32{bytes[(offset + i) / 8U]} >> ((offset + i) % 8U)) & 1U) << i;
    }
    return value;
}
[[nodiscard]] Int32 Signed(const UInt32 value, const UInt8 width, const bool sign) noexcept
{
    if (!sign || (value & (1U << (width - 1U))) == 0U)
    {
        return static_cast<Int32>(value);
    }
    const UInt32 mask = width == 32U ? ~UInt32{0U} : (1U << width) - 1U;
    return -1 - static_cast<Int32>(mask - value);
}
} // namespace
UInt32 HidRelativeMouse::Usage(const Size index) const noexcept
{
    if (m_usageCount != 0U)
    {
        return m_usages[index < m_usageCount ? index : m_usageCount - 1U];
    }
    if (m_range && m_maximum >= m_minimum)
    {
        return m_minimum + (index <= m_maximum - m_minimum ? static_cast<UInt32>(index) : m_maximum - m_minimum);
    }
    return 0U;
}
bool HidRelativeMouse::Main(const UInt8 tag, const UInt32 value) noexcept
{
    if (tag == 10U)
    {
        if (m_depth == MaximumDepth)
        {
            return false;
        }
        m_collections[m_depth++] = m_mouse;
        if (value == 1U)
        {
            m_mouse = Usage(0U) == 0x00010002U;
        }
    }
    else if (tag == 12U)
    {
        if (m_depth == 0U)
        {
            return false;
        }
        m_mouse = m_collections[--m_depth];
    }
    else if (tag == 8U)
    {
        if (m_global.SizeBits == 0U || m_global.SizeBits > 32U || m_global.Count > 256U)
        {
            return false;
        }
        Size report{0U};
        while (report < m_work.ReportsCount && m_work.Reports[report].Id != m_global.Id)
        {
            ++report;
        }
        if (report == m_work.ReportsCount)
        {
            if (report == MaximumReports)
            {
                return false;
            }
            m_work.Reports[report].Id = m_global.Id;
            ++m_work.ReportsCount;
        }
        auto& bits = m_work.Reports[report].Bits;
        const UInt32 count = m_global.SizeBits * m_global.Count;
        if (count > 8192U - bits)
        {
            return false;
        }
        if (m_mouse && (value & 3U) == 2U)
        {
            for (Size i{0U}; i < m_global.Count; ++i)
            {
                const auto usage = Usage(i);
                UInt8 kind{0U};
                if (usage >= 0x00090001U && usage <= 0x00090003U)
                {
                    kind = static_cast<UInt8>(usage - 0x00090000U);
                }
                else if (usage == 0x00010030U || usage == 0x00010031U || usage == 0x00010038U)
                {
                    if ((value & 4U) == 0U || !m_global.Signed)
                    {
                        return false;
                    }
                    kind = usage == 0x00010030U ? 4U : usage == 0x00010031U ? 5U : 6U;
                }
                if (kind == 0U)
                {
                    continue;
                }
                if (m_work.Count == MaximumFields)
                {
                    return false;
                }
                m_work.Fields[m_work.Count++] = {.Offset = static_cast<UInt16>(bits + i * m_global.SizeBits),
                                                 .Width = static_cast<UInt8>(m_global.SizeBits),
                                                 .Kind = kind,
                                                 .Id = m_global.Id,
                                                 .Signed = m_global.Signed};
            }
        }
        bits = static_cast<UInt16>(bits + count);
    }
    m_usageCount = 0U;
    m_range = false;
    m_minimum = 0U;
    m_maximum = 0U;
    return true;
}
bool HidRelativeMouse::Item(const UInt8 type, const UInt8 tag, const UInt32 value, const Size size) noexcept
{
    if (type == 0U)
    {
        return Main(tag, value);
    }
    if (type == 1U)
    {
        switch (tag)
        {
        case 0U:
            m_global.Page = value;
            return value <= 0xFFFFU;
        case 1U:
            m_global.Signed = size != 0U && (value & (1U << (size * 8U - 1U))) != 0U;
            break;
        case 7U:
            m_global.SizeBits = value;
            break;
        case 8U:
            if (value == 0U || value > 255U)
            {
                return false;
            }
            m_global.Id = static_cast<UInt8>(value);
            m_work.Ids = true;
            break;
        case 9U:
            m_global.Count = value;
            break;
        case 10U:
            if (m_stackCount == 4U)
            {
                return false;
            }
            m_stack[m_stackCount++] = m_global;
            break;
        case 11U:
            if (m_stackCount == 0U)
            {
                return false;
            }
            m_global = m_stack[--m_stackCount];
            break;
        default:
            break;
        }
    }
    else if (type == 2U)
    {
        const UInt32 usage = size == 4U ? value : (m_global.Page << 16U) | value;
        if (tag == 0U)
        {
            if (m_usageCount == MaximumUsages)
            {
                return false;
            }
            m_usages[m_usageCount++] = usage;
        }
        else if (tag == 1U)
        {
            m_minimum = usage;
            m_range = true;
        }
        else if (tag == 2U)
        {
            m_maximum = usage;
        }
        else if (tag == 10U)
        {
            return false;
        } // alternative usage sets are outside this subset
    }
    return true;
}
bool HidRelativeMouse::Open(const UInt8* const bytes, const Size length) noexcept
{
    if (bytes == nullptr || length == 0U || length > MaximumDescriptorBytes)
    {
        return false;
    }
    m_work = {};
    m_global = {};
    m_stackCount = 0U;
    m_depth = 0U;
    m_usageCount = 0U;
    m_range = false;
    m_mouse = false;
    for (Size at{0U}; at < length;)
    {
        const UInt8 prefix = bytes[at++];
        if (prefix == 0xFEU)
        {
            return false;
        }
        const Size size = (prefix & 3U) == 3U ? 4U : prefix & 3U;
        if (size > length - at)
        {
            return false;
        }
        UInt32 value{0U};
        for (Size i{0U}; i < size; ++i)
        {
            value |= UInt32{bytes[at++]} << (8U * i);
        }
        if (!Item(static_cast<UInt8>((prefix >> 2U) & 3U), static_cast<UInt8>(prefix >> 4U), value, size))
        {
            return false;
        }
    }
    bool xy{false};
    for (Size i{0U}; i < m_work.Count; ++i)
    {
        for (Size j{0U}; j < m_work.Count; ++j)
        {
            xy = xy || (m_work.Fields[i].Kind == 4U && m_work.Fields[j].Kind == 5U &&
                        m_work.Fields[i].Id == m_work.Fields[j].Id);
        }
    }
    if (!xy || m_depth != 0U || m_stackCount != 0U)
    {
        return false;
    }
    for (Size i{0U}; i < m_work.ReportsCount; ++i)
    {
        if (m_work.Ids && m_work.Reports[i].Id == 0U)
        {
            return false;
        }
    }
    m_layout = m_work;
    m_buttons = 0U;
    return true;
}
void HidRelativeMouse::Release(const UInt32 source, EventBatch& output) noexcept
{
    output.Count = 0U;
    PointerEvents(output, source, 0, 0, 0U, 0, m_buttons);
}
ReportResult HidRelativeMouse::Mouse(const UInt8* const bytes, const Size length, const UInt32 source,
                                     EventBatch& output) noexcept
{
    output.Count = 0U;
    if (bytes == nullptr || length == 0U)
    {
        return ReportResult::Short;
    }
    const UInt8 id = m_layout.Ids ? bytes[0U] : 0U;
    Size report{0U};
    while (report < m_layout.ReportsCount && m_layout.Reports[report].Id != id)
    {
        ++report;
    }
    if (report == m_layout.ReportsCount)
    {
        return ReportResult::Invalid;
    }
    const Size prefix = m_layout.Ids ? 1U : 0U;
    if ((m_layout.Reports[report].Bits + 7U) / 8U > length - prefix)
    {
        return ReportResult::Short;
    }
    Int32 dx{0};
    Int32 dy{0};
    Int32 wheel{0};
    UInt8 buttons = m_buttons;
    for (Size i{0U}; i < m_layout.Count; ++i)
    {
        const auto& field = m_layout.Fields[i];
        if (field.Id != id)
        {
            continue;
        }
        const auto value = Bits(bytes + prefix, field.Offset, field.Width);
        if (field.Kind <= 3U)
        {
            const auto bit = static_cast<UInt8>(1U << (field.Kind - 1U));
            buttons = value == 0U ? static_cast<UInt8>(buttons & ~bit) : static_cast<UInt8>(buttons | bit);
        }
        else if (field.Kind == 4U)
        {
            dx = Signed(value, field.Width, field.Signed);
        }
        else if (field.Kind == 5U)
        {
            dy = Signed(value, field.Width, field.Signed);
        }
        else
        {
            wheel = Signed(value, field.Width, field.Signed);
        }
    }
    PointerEvents(output, source, dx, dy, buttons, wheel, m_buttons);
    return ReportResult::Accepted;
}
} // namespace Asteria::User::Input
