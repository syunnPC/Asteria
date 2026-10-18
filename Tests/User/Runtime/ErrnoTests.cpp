// SPDX-License-Identifier: Apache-2.0
#include "User/Posix/Errno.hpp"
#include "User/Runtime/GenericStatus.hpp"
#include "User/Runtime/KernelStatus.hpp"
#include "User/Runtime/MemoryStatus.hpp"
#include "User/Runtime/TimeStatus.hpp"
#include "User/Runtime/ConsoleStatus.hpp"
#include "User/Runtime/FileStatus.hpp"
#include "User/Runtime/DatagramStatus.hpp"
#include "User/Runtime/IpcStatus.hpp"
#include "User/Runtime/ProcessStatus.hpp"

#include "Support/TestContext.hpp"
#include <cstring>

namespace Asteria::Test
{
namespace
{
struct Expected
{
    User::Runtime::Status Outcome;
    int Number;
};
constexpr Expected Mappings[]{
    {.Outcome = User::Runtime::Status{}, .Number = 0},
    {.Outcome = User::Runtime::GenericStatus::Restarted, .Number = 4},
    {.Outcome = User::Runtime::GenericStatus::Pending, .Number = 11},
    {.Outcome = User::Runtime::GenericStatus::RefusedForCredit, .Number = 11},
    {.Outcome = User::Runtime::GenericStatus::StaleObject, .Number = 116},
    {.Outcome = User::Runtime::GenericStatus::StateLost, .Number = 5},
    {.Outcome = User::Runtime::GenericStatus::RebindRequired, .Number = 107},
    {.Outcome = User::Runtime::GenericStatus::CommitStatusUnknown, .Number = 5},
    {.Outcome = User::Runtime::GenericStatus::TooLate, .Number = 114},
    {.Outcome = User::Runtime::GenericStatus::NotSupported, .Number = 95},
    {.Outcome = User::Runtime::GenericStatus::InvalidArgument, .Number = 22},
    {.Outcome = User::Runtime::GenericStatus::OutOfResources, .Number = 12},
    {.Outcome = User::Runtime::GenericStatus::AccessDenied, .Number = 13},
    {.Outcome = User::Runtime::KernelStatus::UnknownSystemCall, .Number = 38},
    {.Outcome = User::Runtime::KernelStatus::InvalidArguments, .Number = 22},
    {.Outcome = User::Runtime::KernelStatus::InvalidConfiguration, .Number = 22},
    {.Outcome = User::Runtime::KernelStatus::InvalidHandle, .Number = 9},
    {.Outcome = User::Runtime::KernelStatus::ObjectNotFound, .Number = 2},
    {.Outcome = User::Runtime::KernelStatus::ResourceExhausted, .Number = 12},
    {.Outcome = User::Runtime::KernelStatus::SpaceExhausted, .Number = 28},
    {.Outcome = User::Runtime::KernelStatus::RightsDenied, .Number = 13},
    {.Outcome = User::Runtime::KernelStatus::RightsAmplification, .Number = 13},
    {.Outcome = User::Runtime::KernelStatus::NoAttenuation, .Number = 22},
    {.Outcome = User::Runtime::KernelStatus::TypeMismatch, .Number = 22},
    {.Outcome = User::Runtime::KernelStatus::Revoked, .Number = 116},
    {.Outcome = User::Runtime::KernelStatus::Expired, .Number = 116},
    {.Outcome = User::Runtime::KernelStatus::LeaseExtension, .Number = 22},
    {.Outcome = User::Runtime::KernelStatus::RevocationDomainExhausted, .Number = 28},
    {.Outcome = User::Runtime::KernelStatus::WouldBlock, .Number = 11},
    {.Outcome = User::Runtime::KernelStatus::UserMemoryFault, .Number = 14},
    {.Outcome = User::Runtime::KernelStatus::TimedOut, .Number = 110},
    {.Outcome = User::Runtime::KernelStatus::WriteValueDenied, .Number = 13},
    {.Outcome = User::Runtime::KernelStatus::ReservedMemory, .Number = 13},
    {.Outcome = User::Runtime::KernelStatus::DisplayReserved, .Number = 13},
    {.Outcome = User::Runtime::KernelStatus::BridgeReserved, .Number = 13},

    {.Outcome = User::Runtime::ConsoleStatus::Refused, .Number = 22},
    {.Outcome = User::Runtime::FileStatus::NotFound, .Number = 2},
    {.Outcome = User::Runtime::FileStatus::Corrupt, .Number = 117},
    {.Outcome = User::Runtime::FileStatus::IoFailed, .Number = 5},
    {.Outcome = User::Runtime::FileStatus::Cancelled, .Number = 125},
    {.Outcome = User::Runtime::DatagramStatus::NoRule, .Number = 13},
    {.Outcome = User::Runtime::DatagramStatus::NotBound, .Number = 107},
    {.Outcome = User::Runtime::DatagramStatus::IdentityRefused, .Number = 13},
    {.Outcome = User::Runtime::DatagramStatus::Malformed, .Number = 22},
    {.Outcome = User::Runtime::DatagramStatus::IoFailed, .Number = 5},
};
void TestMaps(TestSupport::TestContext& context) noexcept
{
    for (const Expected& expected : Mappings)
    {
        ASTERIA_EXPECT(context, "each facility maps its known outcomes to the Asteria libc errno ABI",
                       User::Posix::ToErrno(expected.Outcome) == expected.Number);
    }
    {
        const auto unknown = User::Runtime::Status::FromRaw(0x8000000DU);
        ASTERIA_EXPECT(context, "Generic refuses future codes before enum conversion", unknown.HasValue(),
                       !unknown.GetValue().TryAs<User::Runtime::GenericStatus>().HasValue(),
                       User::Posix::ToErrno(unknown.GetValue()) == 5,
                       User::Posix::ErrnoGeneric(unknown.GetValue()) == 5,
                       User::Posix::ErrnoGeneric(User::Runtime::Status{}) == 5);
    }
    {
        const auto unknown = User::Runtime::Status::FromRaw(0x81000017U);
        ASTERIA_EXPECT(context, "Kernel refuses future codes before enum conversion", unknown.HasValue(),
                       !unknown.GetValue().TryAs<User::Runtime::KernelStatus>().HasValue(),
                       User::Posix::ToErrno(unknown.GetValue()) == 5, User::Posix::ErrnoKernel(unknown.GetValue()) == 5,
                       User::Posix::ErrnoKernel(User::Runtime::Status{}) == 5);
    }
    {
        const auto unknown = User::Runtime::Status::FromRaw(0x82000001U);
        ASTERIA_EXPECT(context, "Memory refuses future codes before enum conversion", unknown.HasValue(),
                       !unknown.GetValue().TryAs<User::Runtime::MemoryStatus>().HasValue(),
                       User::Posix::ToErrno(unknown.GetValue()) == 5, User::Posix::ErrnoMemory(unknown.GetValue()) == 5,
                       User::Posix::ErrnoMemory(User::Runtime::Status{}) == 5);
    }
    {
        const auto unknown = User::Runtime::Status::FromRaw(0x83000001U);
        ASTERIA_EXPECT(context, "Time refuses future codes before enum conversion", unknown.HasValue(),
                       !unknown.GetValue().TryAs<User::Runtime::TimeStatus>().HasValue(),
                       User::Posix::ToErrno(unknown.GetValue()) == 5, User::Posix::ErrnoTime(unknown.GetValue()) == 5,
                       User::Posix::ErrnoTime(User::Runtime::Status{}) == 5);
    }
    {
        const auto unknown = User::Runtime::Status::FromRaw(0x84000002U);
        ASTERIA_EXPECT(context, "Console refuses future codes before enum conversion", unknown.HasValue(),
                       !unknown.GetValue().TryAs<User::Runtime::ConsoleStatus>().HasValue(),
                       User::Posix::ToErrno(unknown.GetValue()) == 5,
                       User::Posix::ErrnoConsole(unknown.GetValue()) == 5,
                       User::Posix::ErrnoConsole(User::Runtime::Status{}) == 5);
    }
    {
        const auto unknown = User::Runtime::Status::FromRaw(0x85000005U);
        ASTERIA_EXPECT(context, "File refuses future codes before enum conversion", unknown.HasValue(),
                       !unknown.GetValue().TryAs<User::Runtime::FileStatus>().HasValue(),
                       User::Posix::ToErrno(unknown.GetValue()) == 5, User::Posix::ErrnoFile(unknown.GetValue()) == 5,
                       User::Posix::ErrnoFile(User::Runtime::Status{}) == 5);
    }
    {
        const auto unknown = User::Runtime::Status::FromRaw(0x86000006U);
        ASTERIA_EXPECT(context, "Datagram refuses future codes before enum conversion", unknown.HasValue(),
                       !unknown.GetValue().TryAs<User::Runtime::DatagramStatus>().HasValue(),
                       User::Posix::ToErrno(unknown.GetValue()) == 5,
                       User::Posix::ErrnoDatagram(unknown.GetValue()) == 5,
                       User::Posix::ErrnoDatagram(User::Runtime::Status{}) == 5);
    }
    {
        const auto unknown = User::Runtime::Status::FromRaw(0x87000001U);
        ASTERIA_EXPECT(context, "Ipc refuses future codes before enum conversion", unknown.HasValue(),
                       !unknown.GetValue().TryAs<User::Runtime::IpcStatus>().HasValue(),
                       User::Posix::ToErrno(unknown.GetValue()) == 5, User::Posix::ErrnoIpc(unknown.GetValue()) == 5,
                       User::Posix::ErrnoIpc(User::Runtime::Status{}) == 5);
    }
    {
        const auto unknown = User::Runtime::Status::FromRaw(0x88000001U);
        ASTERIA_EXPECT(context, "Process refuses future codes before enum conversion", unknown.HasValue(),
                       !unknown.GetValue().TryAs<User::Runtime::ProcessStatus>().HasValue(),
                       User::Posix::ToErrno(unknown.GetValue()) == 5,
                       User::Posix::ErrnoProcess(unknown.GetValue()) == 5,
                       User::Posix::ErrnoProcess(User::Runtime::Status{}) == 5);
    }
}
void TestNames(TestSupport::TestContext& context) noexcept
{
    ASTERIA_EXPECT(context, "Generic owns the Restarted name",
                   std::strcmp(User::Runtime::Name(User::Runtime::GenericStatus::Restarted), "Restarted") == 0);
    ASTERIA_EXPECT(context, "Generic owns the Pending name",
                   std::strcmp(User::Runtime::Name(User::Runtime::GenericStatus::Pending), "Pending") == 0);
    ASTERIA_EXPECT(
        context, "Generic owns the RefusedForCredit name",
        std::strcmp(User::Runtime::Name(User::Runtime::GenericStatus::RefusedForCredit), "RefusedForCredit") == 0);
    ASTERIA_EXPECT(context, "Generic owns the StaleObject name",
                   std::strcmp(User::Runtime::Name(User::Runtime::GenericStatus::StaleObject), "StaleObject") == 0);
    ASTERIA_EXPECT(context, "Generic owns the StateLost name",
                   std::strcmp(User::Runtime::Name(User::Runtime::GenericStatus::StateLost), "StateLost") == 0);
    ASTERIA_EXPECT(context, "Generic owns the RebindRequired name",
                   std::strcmp(User::Runtime::Name(User::Runtime::GenericStatus::RebindRequired), "RebindRequired") ==
                       0);
    ASTERIA_EXPECT(context, "Generic owns the CommitStatusUnknown name",
                   std::strcmp(User::Runtime::Name(User::Runtime::GenericStatus::CommitStatusUnknown),
                               "CommitStatusUnknown") == 0);
    ASTERIA_EXPECT(context, "Generic owns the TooLate name",
                   std::strcmp(User::Runtime::Name(User::Runtime::GenericStatus::TooLate), "TooLate") == 0);
    ASTERIA_EXPECT(context, "Generic owns the NotSupported name",
                   std::strcmp(User::Runtime::Name(User::Runtime::GenericStatus::NotSupported), "NotSupported") == 0);
    ASTERIA_EXPECT(context, "Generic owns the InvalidArgument name",
                   std::strcmp(User::Runtime::Name(User::Runtime::GenericStatus::InvalidArgument), "InvalidArgument") ==
                       0);
    ASTERIA_EXPECT(context, "Generic owns the OutOfResources name",
                   std::strcmp(User::Runtime::Name(User::Runtime::GenericStatus::OutOfResources), "OutOfResources") ==
                       0);
    ASTERIA_EXPECT(context, "Generic owns the AccessDenied name",
                   std::strcmp(User::Runtime::Name(User::Runtime::GenericStatus::AccessDenied), "AccessDenied") == 0);
    ASTERIA_EXPECT(
        context, "Kernel owns the UnknownSystemCall name",
        std::strcmp(User::Runtime::Name(User::Runtime::KernelStatus::UnknownSystemCall), "UnknownSystemCall") == 0);
    ASTERIA_EXPECT(
        context, "Kernel owns the InvalidArguments name",
        std::strcmp(User::Runtime::Name(User::Runtime::KernelStatus::InvalidArguments), "InvalidArguments") == 0);
    ASTERIA_EXPECT(context, "Kernel owns the InvalidConfiguration name",
                   std::strcmp(User::Runtime::Name(User::Runtime::KernelStatus::InvalidConfiguration),
                               "InvalidConfiguration") == 0);
    ASTERIA_EXPECT(context, "Kernel owns the InvalidHandle name",
                   std::strcmp(User::Runtime::Name(User::Runtime::KernelStatus::InvalidHandle), "InvalidHandle") == 0);
    ASTERIA_EXPECT(context, "Kernel owns the ObjectNotFound name",
                   std::strcmp(User::Runtime::Name(User::Runtime::KernelStatus::ObjectNotFound), "ObjectNotFound") ==
                       0);
    ASTERIA_EXPECT(
        context, "Kernel owns the ResourceExhausted name",
        std::strcmp(User::Runtime::Name(User::Runtime::KernelStatus::ResourceExhausted), "ResourceExhausted") == 0);
    ASTERIA_EXPECT(context, "Kernel owns the SpaceExhausted name",
                   std::strcmp(User::Runtime::Name(User::Runtime::KernelStatus::SpaceExhausted), "SpaceExhausted") ==
                       0);
    ASTERIA_EXPECT(context, "Kernel owns the RightsDenied name",
                   std::strcmp(User::Runtime::Name(User::Runtime::KernelStatus::RightsDenied), "RightsDenied") == 0);
    ASTERIA_EXPECT(
        context, "Kernel owns the RightsAmplification name",
        std::strcmp(User::Runtime::Name(User::Runtime::KernelStatus::RightsAmplification), "RightsAmplification") == 0);
    ASTERIA_EXPECT(context, "Kernel owns the NoAttenuation name",
                   std::strcmp(User::Runtime::Name(User::Runtime::KernelStatus::NoAttenuation), "NoAttenuation") == 0);
    ASTERIA_EXPECT(context, "Kernel owns the TypeMismatch name",
                   std::strcmp(User::Runtime::Name(User::Runtime::KernelStatus::TypeMismatch), "TypeMismatch") == 0);
    ASTERIA_EXPECT(context, "Kernel owns the Revoked name",
                   std::strcmp(User::Runtime::Name(User::Runtime::KernelStatus::Revoked), "Revoked") == 0);
    ASTERIA_EXPECT(context, "Kernel owns the Expired name",
                   std::strcmp(User::Runtime::Name(User::Runtime::KernelStatus::Expired), "Expired") == 0);
    ASTERIA_EXPECT(context, "Kernel owns the LeaseExtension name",
                   std::strcmp(User::Runtime::Name(User::Runtime::KernelStatus::LeaseExtension), "LeaseExtension") ==
                       0);
    ASTERIA_EXPECT(context, "Kernel owns the RevocationDomainExhausted name",
                   std::strcmp(User::Runtime::Name(User::Runtime::KernelStatus::RevocationDomainExhausted),
                               "RevocationDomainExhausted") == 0);
    ASTERIA_EXPECT(context, "Kernel owns the WouldBlock name",
                   std::strcmp(User::Runtime::Name(User::Runtime::KernelStatus::WouldBlock), "WouldBlock") == 0);
    ASTERIA_EXPECT(context, "Kernel owns the UserMemoryFault name",
                   std::strcmp(User::Runtime::Name(User::Runtime::KernelStatus::UserMemoryFault), "UserMemoryFault") ==
                       0);
    ASTERIA_EXPECT(context, "Kernel owns the TimedOut name",
                   std::strcmp(User::Runtime::Name(User::Runtime::KernelStatus::TimedOut), "TimedOut") == 0);
    ASTERIA_EXPECT(
        context, "Kernel owns the WriteValueDenied name",
        std::strcmp(User::Runtime::Name(User::Runtime::KernelStatus::WriteValueDenied), "WriteValueDenied") == 0);
    ASTERIA_EXPECT(context, "Kernel owns the ReservedMemory name",
                   std::strcmp(User::Runtime::Name(User::Runtime::KernelStatus::ReservedMemory), "ReservedMemory") ==
                       0);
    ASTERIA_EXPECT(context, "Kernel owns the DisplayReserved name",
                   std::strcmp(User::Runtime::Name(User::Runtime::KernelStatus::DisplayReserved), "DisplayReserved") ==
                       0);
    ASTERIA_EXPECT(context, "Kernel owns the BridgeReserved name",
                   std::strcmp(User::Runtime::Name(User::Runtime::KernelStatus::BridgeReserved), "BridgeReserved") ==
                       0);
    ASTERIA_EXPECT(context, "Console owns the Refused name",
                   std::strcmp(User::Runtime::Name(User::Runtime::ConsoleStatus::Refused), "Refused") == 0);
    ASTERIA_EXPECT(context, "File owns the NotFound name",
                   std::strcmp(User::Runtime::Name(User::Runtime::FileStatus::NotFound), "NotFound") == 0);
    ASTERIA_EXPECT(context, "File owns the Corrupt name",
                   std::strcmp(User::Runtime::Name(User::Runtime::FileStatus::Corrupt), "Corrupt") == 0);
    ASTERIA_EXPECT(context, "File owns the IoFailed name",
                   std::strcmp(User::Runtime::Name(User::Runtime::FileStatus::IoFailed), "IoFailed") == 0);
    ASTERIA_EXPECT(context, "File owns the Cancelled name",
                   std::strcmp(User::Runtime::Name(User::Runtime::FileStatus::Cancelled), "Cancelled") == 0);
    ASTERIA_EXPECT(context, "Datagram owns the NoRule name",
                   std::strcmp(User::Runtime::Name(User::Runtime::DatagramStatus::NoRule), "NoRule") == 0);
    ASTERIA_EXPECT(context, "Datagram owns the NotBound name",
                   std::strcmp(User::Runtime::Name(User::Runtime::DatagramStatus::NotBound), "NotBound") == 0);
    ASTERIA_EXPECT(
        context, "Datagram owns the IdentityRefused name",
        std::strcmp(User::Runtime::Name(User::Runtime::DatagramStatus::IdentityRefused), "IdentityRefused") == 0);
    ASTERIA_EXPECT(context, "Datagram owns the Malformed name",
                   std::strcmp(User::Runtime::Name(User::Runtime::DatagramStatus::Malformed), "Malformed") == 0);
    ASTERIA_EXPECT(context, "Datagram owns the IoFailed name",
                   std::strcmp(User::Runtime::Name(User::Runtime::DatagramStatus::IoFailed), "IoFailed") == 0);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    Asteria::TestSupport::TestContext context{"ErrnoTests"};
    Asteria::Test::TestMaps(context);
    Asteria::Test::TestNames(context);
    return context.Finish();
}
