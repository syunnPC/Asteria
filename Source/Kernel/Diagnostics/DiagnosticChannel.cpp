// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Diagnostics/DiagnosticChannel.hpp"

#include "Abi/SystemCallAbi.hpp"

namespace Asteria::Kernel::Diagnostics
{
namespace
{
constexpr Foundation::UInt8 LineFeed{0x0AU};

[[nodiscard]] constexpr bool IsPrintable(const Foundation::UInt8 value) noexcept
{
    return (value >= 0x20U) && (value <= 0x7EU);
}
} // namespace

Object::ObjectType DiagnosticChannel::GetObjectType() const noexcept
{
    return Object::ObjectType::DiagnosticChannel;
}

Object::ResourceDomainId DiagnosticChannel::GetChargeOwner() const noexcept
{
    return m_chargeOwner;
}

DiagnosticWriteError DiagnosticChannel::Validate(const Foundation::UInt8* const text,
                                                 const Foundation::Size sizeBytes) noexcept
{
    if ((text == nullptr) || (sizeBytes == 0U))
    {
        return DiagnosticWriteError::Empty;
    }
    if (sizeBytes > Abi::DebugWriteMaximumBytes)
    {
        return DiagnosticWriteError::TooLong;
    }
    for (Foundation::Size index{0U}; index < sizeBytes; ++index)
    {
        if (!IsPrintable(text[index]) && (text[index] != LineFeed))
        {
            return DiagnosticWriteError::InvalidByte;
        }
    }
    if (text[sizeBytes - 1U] != LineFeed)
    {
        return DiagnosticWriteError::UnterminatedLine;
    }
    return DiagnosticWriteError::None;
}

DiagnosticWriteError DiagnosticChannel::Write(const Foundation::UInt8* const text,
                                              const Foundation::Size sizeBytes) noexcept
{
    const DiagnosticWriteError valid = Validate(text, sizeBytes);
    if (valid != DiagnosticWriteError::None)
    {
        return valid;
    }
    Foundation::Size lineStart{0U};
    for (Foundation::Size index{0U}; index < sizeBytes; ++index)
    {
        if (text[index] == LineFeed)
        {
            m_sink.WriteLine(text + lineStart, index - lineStart);
            lineStart = index + 1U;
        }
    }
    return DiagnosticWriteError::None;
}
} // namespace Asteria::Kernel::Diagnostics
