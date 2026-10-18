// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_INPUT_USBCONFIGURATION_HPP_INCLUDED
#define ASTERIA_USER_INPUT_USBCONFIGURATION_HPP_INCLUDED

#include "User/Input/UsbDescriptors.hpp"

namespace Asteria::User::Input
{
enum class DescriptorError : UInt8
{
    None,
    Null,
    Header,
    Truncated,
    Length,
    InterfaceCount,
    DuplicateInterface,
    MissingDefault,
    EndpointCount,
    Orphan,
    Endpoint,
    DuplicateEndpoint,
    Hid,
    Companion,
    Association,
};
enum class InterfaceClass : UInt8
{
    Unsupported,
    BootKeyboard,
    BootMouse,
    MassStorage,
    ReportMouse,
};
enum class BindingError : UInt8
{
    None,
    Unsupported,
    Alternate,
    MissingHid,
    MissingEndpoint,
    AmbiguousEndpoint,
    PacketSize,
};
struct HidDescriptor
{
    UInt16 Version;
    UInt8 Country;
    UInt8 DescriptorCount;
    UInt16 ReportLength;
};
struct HidClassEntry
{
    UInt8 Type;
    UInt16 Length;
};
struct InterfaceDescriptor
{
    UInt8 Number;
    UInt8 Alternate;
    UInt8 EndpointCount;
    UInt8 Class;
    UInt8 Subclass;
    UInt8 Protocol;
    UInt8 StringIndex;
    HidDescriptor Hid;
    bool HasHid;
    Size Offset;
    Size End;
    Size HidOffset;
};
struct InterfaceAssociation
{
    UInt8 First;
    UInt8 Count;
    UInt8 Class;
    UInt8 Subclass;
    UInt8 Protocol;
    UInt8 StringIndex;
};
struct InterfaceBinding
{
    InterfaceClass Class;
    UInt8 Number;
    EndpointDescriptor In;
    EndpointDescriptor Out;
    UInt16 ReportLength;
};

/// Immutable borrowed configuration, including every alternate, endpoint, IAD
/// and HID descriptor. Bytes must remain unchanged/alive while the view is used.
/// No allocation, blocking or callbacks; at most 512 descriptors per scan.
/// Parse and accessors leave outputs unchanged on failure. Parse validates the
/// entire set before publication, even descriptors belonging to unsupported classes.
class UsbConfiguration final
{
public:
    [[nodiscard]] static DescriptorError Parse(const UInt8* bytes, Size length, UsbConfiguration& output) noexcept;
    [[nodiscard]] bool Interface(Size index, InterfaceDescriptor& output) const noexcept;
    [[nodiscard]] bool Endpoint(const InterfaceDescriptor& interface, Size index,
                                EndpointDescriptor& output) const noexcept;
    [[nodiscard]] bool HidEntry(const InterfaceDescriptor& interface, Size index, HidClassEntry& output) const noexcept;
    [[nodiscard]] bool Association(Size index, InterfaceAssociation& output) const noexcept;
    [[nodiscard]] BindingError Bind(Size index, InterfaceBinding& output) const noexcept;
    [[nodiscard]] ConfigurationHeader Header() const noexcept
    {
        return m_header;
    }
    [[nodiscard]] Size InterfaceCount() const noexcept
    {
        return m_interfaces;
    }
    [[nodiscard]] Size AssociationCount() const noexcept
    {
        return m_associations;
    }

private:
    const UInt8* m_bytes{nullptr};
    ConfigurationHeader m_header{};
    Size m_interfaces{0U};
    Size m_associations{0U};
};
} // namespace Asteria::User::Input

#endif
