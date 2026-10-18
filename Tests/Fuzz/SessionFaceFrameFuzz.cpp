// SPDX-License-Identifier: Apache-2.0
#include "Support/BrokerCodecFuzz.hpp"
namespace Foundation = Asteria::Foundation;
namespace TestSupport = Asteria::TestSupport;
namespace User = Asteria::User;
namespace S = User::Session;
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    return TestSupport::BrokerFuzz<S::SessionFaceCodec>(data, size);
}
extern "C" void AsteriaFuzzSeeds(void (*emit)(const std::uint8_t*, std::size_t))
{
    for (Foundation::UInt32 method{3U}; method <= 16U; ++method)
    {
        if (S::SessionFaceCodec::Count(method) == 0U)
        {
            continue;
        }
        Foundation::UInt64 w[32U]{};
        w[0U] = 1U;
        Foundation::Size attachments{0U};
        switch (method)
        {
        case 3U:
            w[1U] = 1U;
            w[2U] = 2U;
            w[4U] = S::PackSessionPair(640U, 400U);
            w[5U] = S::PackSessionPair(80U, 25U);
            w[6U] = 1U;
            attachments = 2U;
            break;
        case 5U:
            w[1U] = 1U;
            w[2U] = 1U;
            w[4U] = S::PackSessionPair(1U, 1U);
            w[5U] = 1U;
            w[6U] = 1U;
            w[8U] = 1U;
            attachments = 1U;
            break;
        case 6U:
            w[1U] = 1U;
            w[2U] = 1U;
            w[4U] = S::PackSessionDamage(0U, 0U, 1U, 1U);
            attachments = 1U;
            break;
        case 10U:
            attachments = 4U;
            w[1U] = 12U;
            break;
        case 11U:
            attachments = 1U;
            w[1U] = 7U;
            break;
        case 12U:
            attachments = 1U;
            break;
        case 14U:
            w[0U] = 0U;
            break;
        case 16U:
            w[1U] = 1U;
            w[4U] = 1U;
            w[7U] = 7U;
            break;
        default:
            break;
        }
        TestSupport::BrokerSeed<S::SessionFaceCodec>(emit, method, w, attachments);
    }
}
