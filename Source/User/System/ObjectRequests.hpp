// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_SYSTEM_OBJECT_REQUESTS_HPP_INCLUDED
#define ASTERIA_USER_SYSTEM_OBJECT_REQUESTS_HPP_INCLUDED

#include "Abi/SystemCallAbi.hpp"
#include "Foundation/Types.hpp"
#include "User/System/SystemCallClient.hpp"

namespace Asteria::User::System
{
/// Little-endian words and handles in the request and delivery buffers user
/// code exchanges with the kernel.
[[nodiscard]] inline Foundation::UInt64 LoadWord(const Foundation::UInt8* const bytes) noexcept
{
    Foundation::UInt64 value{0U};
    for (Foundation::Size index{0U}; index < 8U; ++index)
    {
        value |= static_cast<Foundation::UInt64>(bytes[index]) << (8U * index);
    }
    return value;
}

inline void StoreWord(Foundation::UInt8* const bytes, const Foundation::UInt64 value) noexcept
{
    for (Foundation::Size index{0U}; index < 8U; ++index)
    {
        bytes[index] = static_cast<Foundation::UInt8>(value >> (8U * index));
    }
}

inline void StoreHandle(Foundation::UInt8* const bytes, const CapabilityHandle handle) noexcept
{
    StoreWord(bytes, handle.Selector);
    StoreWord(bytes + 8U, handle.Authenticator);
}

[[nodiscard]] inline Foundation::UInt64 AddressOf(const volatile void* const bytes) noexcept
{
    return reinterpret_cast<Foundation::UIntPointer>(bytes);
}

/// The kernel's default Endpoint configuration: 4032 queued bytes, 1008
/// reserved control bytes and 2016 bytes per sender; 8 messages, 2 reserved,
/// 4 per sender; 32 attachments, 8 reserved, 16 per sender. Its creation
/// charges the queued bytes as IPC queue bytes.
inline constexpr Foundation::UInt16 DefaultEndpointQueuedBytes{4'032U};
inline constexpr Foundation::UInt16 DefaultEndpointReservedControlBytes{1'008U};
inline constexpr Foundation::UInt16 DefaultEndpointBytesPerSender{2'016U};
inline constexpr Foundation::UInt8 DefaultEndpointMessageLimits[3U]{8U, 2U, 4U};
inline constexpr Foundation::UInt8 DefaultEndpointAttachmentLimits[3U]{32U, 8U, 16U};

/// Writes the request of an Endpoint with the kernel's default configuration
/// (Abi::ObjectCreateEndpointRequestSizeBytes) and the caller's own space as
/// its receiver.
inline void
BuildDefaultEndpointRequest(Foundation::UInt8 (&request)[Abi::ObjectCreateEndpointRequestSizeBytes]) noexcept
{
    for (Foundation::UInt8& byte : request)
    {
        byte = 0U;
    }
    const auto store16 = [&request](const Foundation::Size offset, const Foundation::UInt16 value) noexcept
    {
        request[offset] = static_cast<Foundation::UInt8>(value);
        request[offset + 1U] = static_cast<Foundation::UInt8>(value >> 8U);
    };
    store16(Abi::ObjectCreateEndpointQueuedBytesOffset, DefaultEndpointQueuedBytes);
    store16(Abi::ObjectCreateEndpointReservedControlBytesOffset, DefaultEndpointReservedControlBytes);
    store16(Abi::ObjectCreateEndpointBytesPerSenderOffset, DefaultEndpointBytesPerSender);
    for (Foundation::Size index{0U}; index < 3U; ++index)
    {
        request[Abi::ObjectCreateEndpointQueuedMessagesOffset + index] = DefaultEndpointMessageLimits[index];
        request[Abi::ObjectCreateEndpointQueuedAttachmentsOffset + index] = DefaultEndpointAttachmentLimits[index];
    }
}
} // namespace Asteria::User::System

#endif // ASTERIA_USER_SYSTEM_OBJECT_REQUESTS_HPP_INCLUDED
