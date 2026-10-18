// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Foundation/Types.hpp"
#include "Kernel/Memory/FaultToken.hpp"
#include "Platform/Amd64/Memory/Amd64PageFaultDecoder.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Kernel::Memory::FaultAccess;
using Kernel::Memory::FaultCause;
using Kernel::Memory::FaultPrivilege;
using Kernel::Memory::FaultRecord;
using Platform::Amd64::Memory::Amd64PageFaultDecodeError;
using Platform::Amd64::Memory::Amd64PageFaultDecoder;
using Platform::Amd64::Memory::Amd64PageFaultState;

constexpr Foundation::UInt64 PresentBit{Foundation::UInt64{1U} << 0U};
constexpr Foundation::UInt64 WriteBit{Foundation::UInt64{1U} << 1U};
constexpr Foundation::UInt64 UserBit{Foundation::UInt64{1U} << 2U};
constexpr Foundation::UInt64 ReservedEncodingBit{Foundation::UInt64{1U} << 3U};
constexpr Foundation::UInt64 InstructionFetchBit{Foundation::UInt64{1U} << 4U};
constexpr Foundation::UInt64 ProtectionKeyBit{Foundation::UInt64{1U} << 5U};
constexpr Foundation::UInt64 ShadowStackBit{Foundation::UInt64{1U} << 6U};
constexpr Foundation::UInt64 ReverseMapViolationBit{Foundation::UInt64{1U} << 31U};

[[nodiscard]] constexpr FaultRecord Sentinel() noexcept
{
    return FaultRecord{
        .VirtualAddress = 0xA5A5'A5A5'A5A5'A5A5ULL,
        .InstructionPointer = 0x5A5A'5A5A'5A5A'5A5AULL,
        .ArchitectureSyndrome = 0x0123'4567'89AB'CDEFULL,
        .Access = FaultAccess::Execute,
        .Cause = FaultCause::InvalidMappingEncoding,
        .Privilege = FaultPrivilege::User,
        .Reserved = {1U, 2U, 3U, 4U, 5U},
    };
}

[[nodiscard]] constexpr bool RecordsEqual(const FaultRecord& left, const FaultRecord& right) noexcept
{
    if ((left.VirtualAddress != right.VirtualAddress) || (left.InstructionPointer != right.InstructionPointer) ||
        (left.ArchitectureSyndrome != right.ArchitectureSyndrome) || (left.Access != right.Access) ||
        (left.Cause != right.Cause) || (left.Privilege != right.Privilege))
    {
        return false;
    }
    for (Foundation::Size index{0U}; index < 5U; ++index)
    {
        if (left.Reserved[index] != right.Reserved[index])
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool DecodesAs(const Foundation::UInt64 errorCode, const FaultAccess access, const FaultCause cause,
                             const FaultPrivilege privilege) noexcept
{
    constexpr Foundation::UInt64 FaultAddress{0x1234'5678'9ABC'D000ULL};
    constexpr Foundation::UInt64 InstructionPointer{0xFFFF'8000'0012'3456ULL};
    FaultRecord record{Sentinel()};
    const auto error = Amd64PageFaultDecoder::Decode(
        Amd64PageFaultState{
            .FaultAddress = FaultAddress,
            .InstructionPointer = InstructionPointer,
            .ErrorCode = errorCode,
        },
        record);
    const FaultRecord expected{
        .VirtualAddress = FaultAddress,
        .InstructionPointer = InstructionPointer,
        .ArchitectureSyndrome = errorCode,
        .Access = access,
        .Cause = cause,
        .Privilege = privilege,
        .Reserved = {},
    };
    return (error == Amd64PageFaultDecodeError::None) && RecordsEqual(record, expected);
}

void TestArchitectedFaults(TestContext& runner) noexcept
{
    ASTERIA_EXPECT(runner, "a supervisor read of a missing page is normalized exactly",
                   DecodesAs(0U, FaultAccess::Read, FaultCause::MissingMapping, FaultPrivilege::Supervisor));
    ASTERIA_EXPECT(runner, "a user write of a missing page is normalized exactly",
                   DecodesAs(WriteBit | UserBit, FaultAccess::Write, FaultCause::MissingMapping, FaultPrivilege::User));
    ASTERIA_EXPECT(runner, "a user instruction fetch of a missing page is normalized exactly",
                   DecodesAs(InstructionFetchBit | UserBit, FaultAccess::Execute, FaultCause::MissingMapping,
                             FaultPrivilege::User));
    ASTERIA_EXPECT(runner, "a present-page write violation is normalized exactly",
                   DecodesAs(PresentBit | WriteBit, FaultAccess::Write, FaultCause::ProtectionViolation,
                             FaultPrivilege::Supervisor));
    ASTERIA_EXPECT(runner, "a reserved paging entry reports invalid mapping encoding",
                   DecodesAs(PresentBit | ReservedEncodingBit | UserBit, FaultAccess::Read,
                             FaultCause::InvalidMappingEncoding, FaultPrivilege::User));
    ASTERIA_EXPECT(runner, "a protection-key fault remains a protection violation",
                   DecodesAs(PresentBit | ProtectionKeyBit | UserBit, FaultAccess::Read,
                             FaultCause::ProtectionViolation, FaultPrivilege::User));
    ASTERIA_EXPECT(runner, "a missing shadow-stack page retains its write access",
                   DecodesAs(ShadowStackBit | WriteBit, FaultAccess::Write, FaultCause::MissingMapping,
                             FaultPrivilege::Supervisor));
    ASTERIA_EXPECT(runner, "a present shadow-stack fault is a protection violation",
                   DecodesAs(PresentBit | ShadowStackBit | WriteBit, FaultAccess::Write,
                             FaultCause::ProtectionViolation, FaultPrivilege::Supervisor));
    ASTERIA_EXPECT(runner, "an RMP fault remains a protection violation",
                   DecodesAs(PresentBit | ReverseMapViolationBit | UserBit, FaultAccess::Read,
                             FaultCause::ProtectionViolation, FaultPrivilege::User));
}

void TestRejectedSyndromes(TestContext& runner) noexcept
{
    constexpr Foundation::UInt64 ReservedSyndromes[]{
        Foundation::UInt64{1U} << 7U,
        Foundation::UInt64{1U} << 30U,
        Foundation::UInt64{1U} << 63U,
    };
    for (const Foundation::UInt64 errorCode : ReservedSyndromes)
    {
        const FaultRecord sentinel{Sentinel()};
        FaultRecord output{sentinel};
        ASTERIA_EXPECT(runner, "reserved error-code bits are rejected without output writes",
                       Amd64PageFaultDecoder::Decode(Amd64PageFaultState{0x1000U, 0x2000U, errorCode}, output) ==
                           Amd64PageFaultDecodeError::UnsupportedSyndrome,
                       RecordsEqual(output, sentinel));
    }

    constexpr Foundation::UInt64 ContradictorySyndromes[]{
        InstructionFetchBit | WriteBit,
        InstructionFetchBit | ProtectionKeyBit,
        InstructionFetchBit | ShadowStackBit,
    };
    for (const Foundation::UInt64 errorCode : ContradictorySyndromes)
    {
        const FaultRecord sentinel{Sentinel()};
        FaultRecord output{sentinel};
        ASTERIA_EXPECT(runner, "instruction fetches reject contradictory data-access bits",
                       Amd64PageFaultDecoder::Decode(Amd64PageFaultState{0x3000U, 0x4000U, errorCode}, output) ==
                           Amd64PageFaultDecodeError::InvalidAccessEncoding,
                       RecordsEqual(output, sentinel));
    }

    constexpr Foundation::UInt64 NonPresentProtectionCauses[]{
        ReservedEncodingBit,
        ProtectionKeyBit | UserBit,
        ReverseMapViolationBit,
    };
    for (const Foundation::UInt64 errorCode : NonPresentProtectionCauses)
    {
        const FaultRecord sentinel{Sentinel()};
        FaultRecord output{sentinel};
        ASTERIA_EXPECT(runner, "non-present faults reject contradictory protection causes",
                       Amd64PageFaultDecoder::Decode(Amd64PageFaultState{0x5000U, 0x6000U, errorCode}, output) ==
                           Amd64PageFaultDecodeError::InvalidAccessEncoding,
                       RecordsEqual(output, sentinel));
    }
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"Amd64PageFaultDecoderTests"};
    Asteria::Test::TestArchitectedFaults(runner);
    Asteria::Test::TestRejectedSyndromes(runner);
    return runner.Finish();
}
