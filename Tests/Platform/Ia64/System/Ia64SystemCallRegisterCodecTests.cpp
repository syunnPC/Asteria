// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Foundation/Types.hpp"
#include "Kernel/System/SystemCallDispatcher.hpp"
#include "Platform/Ia64/System/Ia64SystemCallRegisterCodec.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Kernel::System::SystemCallError;
using Kernel::System::SystemCallRequest;
using Kernel::System::SystemCallResponse;
using Platform::Ia64::System::Ia64SystemCallKnownInputNatBitsMask;
using Platform::Ia64::System::Ia64SystemCallRegisterCodec;
using Platform::Ia64::System::Ia64SystemCallRegisterDecodeError;
using Platform::Ia64::System::Ia64SystemCallRegisterImage;

static_assert(Ia64SystemCallKnownInputNatBitsMask == 0x7FU);
static_assert(static_cast<Foundation::UInt64>(SystemCallError::None) == 0U);
static_assert(static_cast<Foundation::UInt64>(SystemCallError::UnknownSystemCall) == 1U);
static_assert(static_cast<Foundation::UInt64>(SystemCallError::InvalidArguments) == 2U);
static_assert(static_cast<Foundation::UInt64>(SystemCallError::InvalidConfiguration) == 3U);
static_assert(static_cast<Foundation::UInt64>(SystemCallError::InvalidHandle) == 4U);
static_assert(static_cast<Foundation::UInt64>(SystemCallError::ObjectNotFound) == 5U);
static_assert(static_cast<Foundation::UInt64>(SystemCallError::ResourceExhausted) == 6U);
static_assert(static_cast<Foundation::UInt64>(SystemCallError::SpaceExhausted) == 7U);
static_assert(static_cast<Foundation::UInt64>(SystemCallError::RightsDenied) == 8U);
static_assert(static_cast<Foundation::UInt64>(SystemCallError::RightsAmplification) == 9U);
static_assert(static_cast<Foundation::UInt64>(SystemCallError::NoAttenuation) == 10U);
static_assert(static_cast<Foundation::UInt64>(SystemCallError::TypeMismatch) == 11U);
static_assert(static_cast<Foundation::UInt64>(SystemCallError::Revoked) == 12U);
static_assert(static_cast<Foundation::UInt64>(SystemCallError::Expired) == 13U);
static_assert(static_cast<Foundation::UInt64>(SystemCallError::LeaseExtension) == 14U);
static_assert(static_cast<Foundation::UInt64>(SystemCallError::RevocationDomainExhausted) == 15U);
static_assert(static_cast<Foundation::UInt64>(SystemCallError::WouldBlock) == 16U);
static_assert(static_cast<Foundation::UInt64>(SystemCallError::UserMemoryFault) == 17U);

[[nodiscard]] constexpr Ia64SystemCallRegisterImage MakePatternImage(const Foundation::UInt64 inputNatBits) noexcept
{
    return Ia64SystemCallRegisterImage{
        .R15 = 0x1501'0203'0405'0607ULL,
        .R32 = 0x3201'1213'1415'1617ULL,
        .R33 = 0x3301'2223'2425'2627ULL,
        .R34 = 0x3401'3233'3435'3637ULL,
        .R35 = 0x3501'4243'4445'4647ULL,
        .R36 = 0x3601'5253'5455'5657ULL,
        .R37 = 0x3701'6263'6465'6667ULL,
        .InputNatBits = inputNatBits,
        .R8 = 0x0801'7273'7475'7677ULL,
        .R9 = 0x0901'8283'8485'8687ULL,
        .R10 = 0x1001'9293'9495'9697ULL,
    };
}

[[nodiscard]] constexpr SystemCallRequest MakeSentinelRequest() noexcept
{
    return SystemCallRequest{
        .Number = 0xD001'0203'0405'0607ULL,
        .Arguments =
            {
                0xD101'1213'1415'1617ULL,
                0xD201'2223'2425'2627ULL,
                0xD301'3233'3435'3637ULL,
                0xD401'4243'4445'4647ULL,
                0xD501'5253'5455'5657ULL,
                0xD601'6263'6465'6667ULL,
            },
    };
}

[[nodiscard]] constexpr bool RequestsEqual(const SystemCallRequest& left, const SystemCallRequest& right) noexcept
{
    if (left.Number != right.Number)
    {
        return false;
    }
    for (Foundation::Size index{0U}; index < Kernel::System::SystemCallArgumentCount; ++index)
    {
        if (left.Arguments[index] != right.Arguments[index])
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr bool ImagesEqual(const Ia64SystemCallRegisterImage& left,
                                         const Ia64SystemCallRegisterImage& right) noexcept
{
    return (left.R15 == right.R15) && (left.R32 == right.R32) && (left.R33 == right.R33) && (left.R34 == right.R34) &&
           (left.R35 == right.R35) && (left.R36 == right.R36) && (left.R37 == right.R37) &&
           (left.InputNatBits == right.InputNatBits) && (left.R8 == right.R8) && (left.R9 == right.R9) &&
           (left.R10 == right.R10);
}

[[nodiscard]] constexpr bool CaptureFieldsEqual(const Ia64SystemCallRegisterImage& left,
                                                const Ia64SystemCallRegisterImage& right) noexcept
{
    return (left.R15 == right.R15) && (left.R32 == right.R32) && (left.R33 == right.R33) && (left.R34 == right.R34) &&
           (left.R35 == right.R35) && (left.R36 == right.R36) && (left.R37 == right.R37) &&
           (left.InputNatBits == right.InputNatBits);
}

[[nodiscard]] bool DecodeFailureIsAtomic(const Foundation::UInt64 inputNatBits,
                                         const Ia64SystemCallRegisterDecodeError expected) noexcept
{
    const Ia64SystemCallRegisterImage image{MakePatternImage(inputNatBits)};
    const Ia64SystemCallRegisterImage imageSnapshot{image};
    const SystemCallRequest sentinel{MakeSentinelRequest()};
    SystemCallRequest request{sentinel};
    const Ia64SystemCallRegisterDecodeError result = Ia64SystemCallRegisterCodec::DecodeRequest(image, request);
    return (result == expected) && RequestsEqual(request, sentinel) && ImagesEqual(image, imageSnapshot);
}

void TestExactRequestDecode(TestContext& runner) noexcept
{
    const Ia64SystemCallRegisterImage image{MakePatternImage(0U)};
    const Ia64SystemCallRegisterImage snapshot{image};
    SystemCallRequest request{MakeSentinelRequest()};
    const Ia64SystemCallRegisterDecodeError result = Ia64SystemCallRegisterCodec::DecodeRequest(image, request);
    const SystemCallRequest expected{
        .Number = 0x1501'0203'0405'0607ULL,
        .Arguments =
            {
                0x3201'1213'1415'1617ULL,
                0x3301'2223'2425'2627ULL,
                0x3401'3233'3435'3637ULL,
                0x3501'4243'4445'4647ULL,
                0x3601'5253'5455'5657ULL,
                0x3701'6263'6465'6667ULL,
            },
    };
    ASTERIA_EXPECT(runner, "R15 and each distinct stacked input decode into the exact request slot",
                   result == Ia64SystemCallRegisterDecodeError::None, RequestsEqual(request, expected));
    ASTERIA_EXPECT(runner, "successful request decoding leaves the complete register image unchanged",
                   ImagesEqual(image, snapshot));
}

void TestKnownInputNats(TestContext& runner) noexcept
{
    constexpr Foundation::UInt64 KnownBits[]{
        0x01U, 0x02U, 0x04U, 0x08U, 0x10U, 0x20U, 0x40U,
    };
    constexpr const char* Descriptions[]{
        "R15 NaT rejects the request without changing its destination",
        "R32 NaT rejects the request without changing its destination",
        "R33 NaT rejects the request without changing its destination",
        "R34 NaT rejects the request without changing its destination",
        "R35 NaT rejects the request without changing its destination",
        "R36 NaT rejects the request without changing its destination",
        "R37 NaT rejects the request without changing its destination",
    };
    for (Foundation::Size index{0U}; index < 7U; ++index)
    {
        ASTERIA_EXPECT(runner, Descriptions[index],
                       DecodeFailureIsAtomic(KnownBits[index], Ia64SystemCallRegisterDecodeError::InputContainsNat));
    }
    ASTERIA_EXPECT(runner, "the combination of all seven known NaT bits is a user-input failure",
                   DecodeFailureIsAtomic(0x7FU, Ia64SystemCallRegisterDecodeError::InputContainsNat));
}

void TestInvalidNatMetadata(TestContext& runner) noexcept
{
    for (Foundation::Size bit{7U}; bit < 64U; ++bit)
    {
        const Foundation::UInt64 unknownBit = Foundation::UInt64{1U} << bit;
        ASTERIA_EXPECT(runner, "every individual undefined NaT metadata bit is an invariant failure",
                       DecodeFailureIsAtomic(unknownBit, Ia64SystemCallRegisterDecodeError::InvalidNatMetadata));
        ASTERIA_EXPECT(runner, "every undefined bit takes precedence over all known NaT bits",
                       DecodeFailureIsAtomic(unknownBit | Ia64SystemCallKnownInputNatBitsMask,
                                             Ia64SystemCallRegisterDecodeError::InvalidNatMetadata));
    }
}

void TestExactResponseMapping(TestContext& runner) noexcept
{
    Ia64SystemCallRegisterImage image{MakePatternImage(0xFEDC'BA98'7654'3281ULL)};
    const Ia64SystemCallRegisterImage snapshot{image};
    const SystemCallResponse response{
        .Error = SystemCallError::LeaseExtension,
        .Value0 = 0xA901'0203'0405'0607ULL,
        .Value1 = 0xB001'1213'1415'1617ULL,
    };
    Ia64SystemCallRegisterCodec::ApplyResponse(response, image);
    ASTERIA_EXPECT(runner, "response application preserves all seven inputs and NaT metadata bit-exact",
                   CaptureFieldsEqual(image, snapshot));
    ASTERIA_EXPECT(runner, "response error, value zero, and value one map exactly to R8, R9, and R10", image.R8 == 14U,
                   image.R9 == 0xA901'0203'0405'0607ULL, image.R10 == 0xB001'1213'1415'1617ULL);
}

struct ErrorAbiExpectation
{
    SystemCallError Error;
    Foundation::UInt64 RawValue;
    const char* Description;
};

void TestEveryErrorAndCanonicalZeros(TestContext& runner) noexcept
{
    constexpr ErrorAbiExpectation Expectations[]{
        {SystemCallError::None, 0U, "None has ABI value zero and canonical zero results"},
        {SystemCallError::UnknownSystemCall, 1U, "UnknownSystemCall has ABI value one"},
        {SystemCallError::InvalidArguments, 2U, "InvalidArguments has ABI value two"},
        {SystemCallError::InvalidConfiguration, 3U, "InvalidConfiguration has ABI value three"},
        {SystemCallError::InvalidHandle, 4U, "InvalidHandle has ABI value four"},
        {SystemCallError::ObjectNotFound, 5U, "ObjectNotFound has ABI value five"},
        {SystemCallError::ResourceExhausted, 6U, "ResourceExhausted has ABI value six"},
        {SystemCallError::SpaceExhausted, 7U, "SpaceExhausted has ABI value seven"},
        {SystemCallError::RightsDenied, 8U, "RightsDenied has ABI value eight"},
        {SystemCallError::RightsAmplification, 9U, "RightsAmplification has ABI value nine"},
        {SystemCallError::NoAttenuation, 10U, "NoAttenuation has ABI value ten"},
        {SystemCallError::TypeMismatch, 11U, "TypeMismatch has ABI value eleven"},
        {SystemCallError::Revoked, 12U, "Revoked has ABI value twelve"},
        {SystemCallError::Expired, 13U, "Expired has ABI value thirteen"},
        {SystemCallError::LeaseExtension, 14U, "LeaseExtension has ABI value fourteen"},
        {
            SystemCallError::RevocationDomainExhausted,
            15U,
            "RevocationDomainExhausted has ABI value fifteen",
        },
        {SystemCallError::WouldBlock, 16U, "WouldBlock has ABI value sixteen"},
        {
            SystemCallError::UserMemoryFault,
            17U,
            "UserMemoryFault has ABI value seventeen",
        },
    };

    for (const ErrorAbiExpectation& expectation : Expectations)
    {
        Ia64SystemCallRegisterImage image{MakePatternImage(0x8000'0000'0000'0041ULL)};
        const Ia64SystemCallRegisterImage snapshot{image};
        Ia64SystemCallRegisterCodec::ApplyResponse(
            SystemCallResponse{
                .Error = expectation.Error,
                .Value0 = 0U,
                .Value1 = 0U,
            },
            image);
        ASTERIA_EXPECT(runner, expectation.Description, CaptureFieldsEqual(image, snapshot),
                       image.R8 == expectation.RawValue, image.R9 == 0U, image.R10 == 0U);
    }
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"Ia64SystemCallRegisterCodecTests"};
    Asteria::Test::TestExactRequestDecode(runner);
    Asteria::Test::TestKnownInputNats(runner);
    Asteria::Test::TestInvalidNatMetadata(runner);
    Asteria::Test::TestExactResponseMapping(runner);
    Asteria::Test::TestEveryErrorAndCanonicalZeros(runner);
    return runner.Finish();
}
