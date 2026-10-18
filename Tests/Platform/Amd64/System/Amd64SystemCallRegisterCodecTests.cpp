// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Foundation/Types.hpp"
#include "Kernel/System/SystemCallDispatcher.hpp"
#include "Platform/Amd64/System/Amd64SystemCallRegisterCodec.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Kernel::System::SystemCallError;
using Kernel::System::SystemCallRequest;
using Kernel::System::SystemCallResponse;
using Platform::Amd64::System::Amd64SystemCallRegisterCodec;
using Platform::Amd64::System::Amd64SystemCallRegisterImage;

constexpr Foundation::UInt64 InitialRax{0x0123'4567'89AB'CDEFULL};
constexpr Foundation::UInt64 InitialRdi{0x1023'4567'89AB'CDEFULL};
constexpr Foundation::UInt64 InitialRsi{0x2013'4567'89AB'CDEFULL};
constexpr Foundation::UInt64 InitialRdx{0x3012'4567'89AB'CDEFULL};
constexpr Foundation::UInt64 InitialR10{0x4012'3567'89AB'CDEFULL};
constexpr Foundation::UInt64 InitialR8{0x5012'3467'89AB'CDEFULL};
constexpr Foundation::UInt64 InitialR9{0x6012'3457'89AB'CDEFULL};

[[nodiscard]] constexpr Amd64SystemCallRegisterImage InitialImage() noexcept
{
    return Amd64SystemCallRegisterImage{
        .Rax = InitialRax,
        .Rdi = InitialRdi,
        .Rsi = InitialRsi,
        .Rdx = InitialRdx,
        .R10 = InitialR10,
        .R8 = InitialR8,
        .R9 = InitialR9,
    };
}

[[nodiscard]] constexpr bool NonResponseRegistersArePreserved(const Amd64SystemCallRegisterImage& registers) noexcept
{
    return (registers.Rdi == InitialRdi) && (registers.Rsi == InitialRsi) && (registers.R10 == InitialR10) &&
           (registers.R9 == InitialR9);
}

void TestRequestDecoding(TestContext& runner) noexcept
{
    const SystemCallRequest request{Amd64SystemCallRegisterCodec::DecodeRequest(InitialImage())};

    ASTERIA_EXPECT(
        runner, "all seven request registers map to the exact common request slots", request.Number == InitialRax,
        request.Arguments[0U] == InitialRdi, request.Arguments[1U] == InitialRsi, request.Arguments[2U] == InitialRdx,
        request.Arguments[3U] == InitialR10, request.Arguments[4U] == InitialR8, request.Arguments[5U] == InitialR9);
}

void TestResponseApplication(TestContext& runner) noexcept
{
    constexpr Foundation::UInt64 ResponseValue0{0xFEDC'BA98'7654'3210ULL};
    constexpr Foundation::UInt64 ResponseValue1{0x0F1E'2D3C'4B5A'6978ULL};
    Amd64SystemCallRegisterImage registers{InitialImage()};

    Amd64SystemCallRegisterCodec::ApplyResponse(
        SystemCallResponse{
            .Error = SystemCallError::RightsDenied,
            .Value0 = ResponseValue0,
            .Value1 = ResponseValue1,
        },
        registers);

    ASTERIA_EXPECT(runner, "a response changes exactly RAX, RDX, and R8", registers.Rax == 8U,
                   registers.Rdx == ResponseValue0, registers.R8 == ResponseValue1,
                   NonResponseRegistersArePreserved(registers));
}

struct ErrorEncodingCase
{
    SystemCallError Error;
    Foundation::UInt64 ExpectedRax;
};

void TestEveryErrorEncoding(TestContext& runner) noexcept
{
    constexpr ErrorEncodingCase Cases[]{
        {SystemCallError::None, 0U},
        {SystemCallError::UnknownSystemCall, 1U},
        {SystemCallError::InvalidArguments, 2U},
        {SystemCallError::InvalidConfiguration, 3U},
        {SystemCallError::InvalidHandle, 4U},
        {SystemCallError::ObjectNotFound, 5U},
        {SystemCallError::ResourceExhausted, 6U},
        {SystemCallError::SpaceExhausted, 7U},
        {SystemCallError::RightsDenied, 8U},
        {SystemCallError::RightsAmplification, 9U},
        {SystemCallError::NoAttenuation, 10U},
        {SystemCallError::TypeMismatch, 11U},
        {SystemCallError::Revoked, 12U},
        {SystemCallError::Expired, 13U},
        {SystemCallError::LeaseExtension, 14U},
        {SystemCallError::RevocationDomainExhausted, 15U},
        {SystemCallError::WouldBlock, 16U},
        {SystemCallError::UserMemoryFault, 17U},
    };

    for (const ErrorEncodingCase& testCase : Cases)
    {
        Amd64SystemCallRegisterImage registers{InitialImage()};
        Amd64SystemCallRegisterCodec::ApplyResponse(
            SystemCallResponse{
                .Error = testCase.Error,
                .Value0 = 0U,
                .Value1 = 0U,
            },
            registers);

        ASTERIA_EXPECT(runner, "an error uses its independent ABI value and canonical zero results",
                       registers.Rax == testCase.ExpectedRax, registers.Rdx == 0U, registers.R8 == 0U,
                       NonResponseRegistersArePreserved(registers));
    }
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"Amd64SystemCallRegisterCodecTests"};
    Asteria::Test::TestRequestDecoding(runner);
    Asteria::Test::TestResponseApplication(runner);
    Asteria::Test::TestEveryErrorEncoding(runner);
    return runner.Finish();
}
