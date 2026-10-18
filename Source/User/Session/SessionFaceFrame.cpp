// SPDX-License-Identifier: Apache-2.0

#include "User/Session/SessionFaceFrame.hpp"
namespace Asteria::User::Session
{
[[nodiscard]] Foundation::Size WriteSessionFaceFrame(Foundation::UInt8* frame, Foundation::UInt32 method,
                                                     Foundation::UInt64 txn, const Foundation::UInt64* words,
                                                     const System::CapabilityHandle* handles,
                                                     Foundation::Size attachments) noexcept
{
    return words == nullptr
               ? 0U
               : Wire::Write<SessionFaceCodec>(frame, method, txn, Wire::ArrayWords{words}, handles, attachments);
}
[[nodiscard]] bool ReadSessionFaceFrame(const Foundation::UInt8* delivery, Foundation::Size bytes,
                                        SessionFaceMessage& message) noexcept
{
    return Wire::Read<SessionFaceCodec>(delivery, bytes, message);
}
} // namespace Asteria::User::Session
