// SPDX-License-Identifier: Apache-2.0

#include "User/Session/InputV3Frame.hpp"
namespace Asteria::User::Session
{
[[nodiscard]] Foundation::Size WriteInputV3Frame(Foundation::UInt8* frame, Foundation::UInt32 method,
                                                 Foundation::UInt64 txn, const Foundation::UInt64* words,
                                                 const System::CapabilityHandle* handles,
                                                 Foundation::Size attachments) noexcept
{
    return words == nullptr
               ? 0U
               : Wire::Write<InputV3Codec>(frame, method, txn, Wire::ArrayWords{words}, handles, attachments);
}
[[nodiscard]] bool ReadInputV3Frame(const Foundation::UInt8* delivery, Foundation::Size bytes,
                                    InputV3Message& message) noexcept
{
    return Wire::Read<InputV3Codec>(delivery, bytes, message);
}
[[nodiscard]] Foundation::Size WriteInputV3Pointer(Foundation::UInt8* frame, Foundation::UInt64 txn,
                                                   const Foundation::UInt64* words) noexcept
{
    return words == nullptr ? 0U
                            : Wire::Write<InputV3PointerCodec>(frame, Component::DisplayPointerMethod, txn,
                                                               Wire::ArrayWords{words}, nullptr, 0U);
}
[[nodiscard]] bool ReadInputV3Pointer(const Foundation::UInt8* delivery, Foundation::Size bytes,
                                      InputV3Message& message) noexcept
{
    return Wire::Read<InputV3PointerCodec>(delivery, bytes, message);
}
} // namespace Asteria::User::Session
