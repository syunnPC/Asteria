// SPDX-License-Identifier: Apache-2.0
#include "Foundation/Types.hpp"
#include "Kernel/Memory/FaultToken.hpp"
#include "Platform/Amd64/Interrupt/Amd64UserException.hpp"

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
using Platform::Amd64::Interrupt::DescribeUserException;
using Platform::Amd64::Interrupt::IsSupervisedUserExceptionVector;

void TestSupervisedVectors(TestContext& runner) noexcept
{
    bool exceptions{true};
    for (Foundation::UInt64 vector{0U}; vector < 32U; ++vector)
    {
        const bool terminal = (vector == 2U) || (vector == 8U) || (vector == 18U);
        exceptions = exceptions && (IsSupervisedUserExceptionVector(vector) == (!terminal && (vector != 14U)));
    }
    ASTERIA_EXPECT(runner,
                   "every exception vector but the page fault, NMI, double fault and machine check is "
                   "supervised",
                   exceptions, IsSupervisedUserExceptionVector(6U), IsSupervisedUserExceptionVector(13U),
                   !IsSupervisedUserExceptionVector(14U));
    ASTERIA_EXPECT(runner, "interrupts, the timer and the system-call frame are not exceptions",
                   !IsSupervisedUserExceptionVector(32U), !IsSupervisedUserExceptionVector(0xF0U),
                   !IsSupervisedUserExceptionVector(255U), !IsSupervisedUserExceptionVector(256U));
}

void TestRecords(TestContext& runner) noexcept
{
    constexpr FaultRecord invalidOpcode{DescribeUserException(6U, 0U, 0x0040'1234U)};
    constexpr FaultRecord protection{DescribeUserException(13U, 0xFFFF'FFFF'0000'001AULL, 0x7000'0000U)};
    ASTERIA_EXPECT(runner, "an exception names its instruction, vector and error code",
                   invalidOpcode.VirtualAddress == 0x0040'1234U, invalidOpcode.InstructionPointer == 0x0040'1234U,
                   invalidOpcode.ArchitectureSyndrome == (Foundation::UInt64{6U} << 32U),
                   invalidOpcode.Access == FaultAccess::Execute, invalidOpcode.Cause == FaultCause::Exception,
                   invalidOpcode.Privilege == FaultPrivilege::User,
                   protection.ArchitectureSyndrome == ((Foundation::UInt64{13U} << 32U) | 0x1AU),
                   protection.Reserved[0U] == 0U, protection.Reserved[4U] == 0U);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"Amd64UserExceptionTests"};
    Asteria::Test::TestSupervisedVectors(runner);
    Asteria::Test::TestRecords(runner);
    return runner.Finish();
}
