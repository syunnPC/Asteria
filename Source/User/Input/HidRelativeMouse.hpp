// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_INPUT_HID_RELATIVE_MOUSE_HPP_INCLUDED
#define ASTERIA_USER_INPUT_HID_RELATIVE_MOUSE_HPP_INCLUDED
#include "User/Input/HidBoot.hpp"
namespace Asteria::User::Input
{
// USB HID 1.11, sections 6.2.2.4–8 and 8.1; HID Usage Tables 1.5,
// sections 4.1/4.2 (Mouse, X/Y/Wheel) and 12 (Buttons).
// Bounded report-protocol mouse subset: four IDs, 8 fields, signed relative
// axes up to 32 bits, first three buttons. Absolute axes are refused.
// Descriptor scratch is owner storage, never target automatic storage.
class HidRelativeMouse final
{
public:
    static constexpr Size MaximumDescriptorBytes{1024U};
    [[nodiscard]] bool Open(const UInt8* bytes, Size length) noexcept;
    void Release(UInt32 source, EventBatch& output) noexcept;
    [[nodiscard]] ReportResult Mouse(const UInt8* bytes, Size length, UInt32 source, EventBatch& output) noexcept;

private:
    static constexpr Size MaximumFields{8U};
    static constexpr Size MaximumReports{4U};
    static constexpr Size MaximumUsages{16U};
    static constexpr Size MaximumDepth{8U};
    struct Field
    {
        UInt16 Offset;
        UInt8 Width;
        UInt8 Kind;
        UInt8 Id;
        bool Signed;
    };
    struct Report
    {
        UInt16 Bits;
        UInt8 Id;
    };
    struct Layout
    {
        Field Fields[MaximumFields]{};
        Report Reports[MaximumReports]{};
        Size Count{0U};
        Size ReportsCount{0U};
        bool Ids{false};
    };
    struct Global
    {
        UInt32 Page{0U};
        UInt32 SizeBits{0U};
        UInt32 Count{0U};
        UInt8 Id{0U};
        bool Signed{false};
    };
    [[nodiscard]] bool Item(UInt8 type, UInt8 tag, UInt32 value, Size size) noexcept;
    [[nodiscard]] bool Main(UInt8 tag, UInt32 value) noexcept;
    [[nodiscard]] UInt32 Usage(Size index) const noexcept;
    Layout m_layout{};
    Layout m_work{};
    Global m_global{};
    Global m_stack[4U]{};
    Size m_stackCount{0U};
    Size m_usageCount{0U};
    Size m_depth{0U};
    UInt32 m_usages[MaximumUsages]{};
    UInt32 m_minimum{0U};
    UInt32 m_maximum{0U};
    bool m_range{false};
    bool m_mouse{false};
    bool m_collections[MaximumDepth]{};
    UInt8 m_buttons{0U};
};
} // namespace Asteria::User::Input
#endif
