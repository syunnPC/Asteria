// SPDX-License-Identifier: Apache-2.0
#ifndef ASTERIA_TESTS_SUPPORT_BROKER_CODEC_FUZZ_HPP_INCLUDED
#define ASTERIA_TESTS_SUPPORT_BROKER_CODEC_FUZZ_HPP_INCLUDED
#include "User/Session/SessionFaceFrame.hpp"
#include "User/Session/InputV3Frame.hpp"
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <cstddef>
namespace Asteria::TestSupport
{
namespace S = User::Session;
inline Foundation::UInt8 BrokerDelivery[560U];
inline Foundation::UInt8 BrokerFrame[504U];
inline S::Wire::Message BrokerMessage;
inline S::Wire::Message BrokerBefore;
template <class Codec> int BrokerFuzz(const std::uint8_t* data, std::size_t size)
{
    if (size > 512U)
    {
        return 0;
    }
    std::memcpy(BrokerDelivery + 48U, data, size);
    std::memset(&BrokerMessage, 0xA5, sizeof(BrokerMessage));
    BrokerBefore = BrokerMessage;
    if (!S::Wire::Read<Codec>(BrokerDelivery, size + 48U, BrokerMessage))
    {
        if (std::memcmp(&BrokerMessage, &BrokerBefore, sizeof(BrokerMessage)) != 0)
        {
            std::abort();
        }
        return 0;
    }
    const auto& m = BrokerMessage;
    const Foundation::Size rebuilt = S::Wire::Write<Codec>(
        BrokerFrame, m.Method, m.Transaction, S::Wire::ArrayWords{m.Words}, m.Attachments, m.AttachmentCount);
    if ((rebuilt != size) || (std::memcmp(BrokerFrame, data, size) != 0))
    {
        std::abort();
    }
    return 0;
}
template <class Codec>
void BrokerSeed(void (*emit)(const std::uint8_t*, std::size_t), Foundation::UInt32 method,
                const Foundation::UInt64* words, Foundation::Size attachments)
{
    const User::System::CapabilityHandle handles[4U]{{1U, 2U}, {3U, 4U}, {5U, 6U}, {7U, 8U}};
    const auto size = S::Wire::Write<Codec>(BrokerFrame, method, 1U, S::Wire::ArrayWords{words}, handles, attachments);
    if (size == 0U)
    {
        std::abort();
    }
    emit(BrokerFrame, size);
}
} // namespace Asteria::TestSupport
#endif
