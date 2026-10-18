// SPDX-License-Identifier: Apache-2.0
#include "Support/BrokerCodecFuzz.hpp"
namespace Foundation = Asteria::Foundation;
namespace TestSupport = Asteria::TestSupport;
namespace User = Asteria::User;
namespace S = User::Session;
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    TestSupport::BrokerFuzz<S::InputV3Codec>(data, size);
    return TestSupport::BrokerFuzz<S::InputV3PointerCodec>(data, size);
}
extern "C" void AsteriaFuzzSeeds(void (*emit)(const std::uint8_t*, std::size_t))
{
    for (Foundation::UInt32 method{1U}; method <= 13U; ++method)
    {
        Foundation::UInt64 w[28U]{};
        w[0U] = 1U;
        Foundation::Size attachments = method <= 8U ? 1U : 0U;
        if ((method == 2U) || (method == 3U) || (method == 4U) || (method >= 9U))
        {
            w[1U] = 1U;
        }
        if (method == 6U)
        {
            w[1U] = 1U;
            w[2U] = 1U;
        }
        if ((method == 3U) || (method == 10U))
        {
            w[4U] = 1U | (4ULL << 16U) | (1ULL << 32U);
        }
        if (method == 9U)
        {
            w[2U] = 1U;
            w[3U] = 3U;
            attachments = 1U;
        }
        if (method == 10U)
        {
            w[2U] = 1U;
            w[3U] = 100U;
        }
        if (method == 13U)
        {
            w[2U] = 2U;
            attachments = 2U;
        }
        TestSupport::BrokerSeed<S::InputV3Codec>(emit, method, w, attachments);
    }
    const Foundation::UInt64 pointer[8U]{1U, 1U, 1U, 0U, 7U, 3U, 0U, 100U};
    TestSupport::BrokerSeed<S::InputV3PointerCodec>(emit, 9U, pointer, 0U);
}
