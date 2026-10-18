// SPDX-License-Identifier: Apache-2.0
#include "User/Runtime/Status.hpp"
#include "User/Runtime/FileStatus.hpp"
#include "User/Runtime/DatagramStatus.hpp"
#include "User/Runtime/GenericStatus.hpp"
#include "User/Runtime/KernelStatus.hpp"
#include "User/Posix/Errno.hpp"

#include "Support/TestContext.hpp"

namespace Asteria::Test
{
namespace
{
using Foundation::UInt32;
using Foundation::UInt64;
using User::Runtime::Status;
using User::Runtime::Facility;
using User::Runtime::FileStatus;
using User::Runtime::DatagramStatus;
using User::Runtime::GenericStatus;
using User::Runtime::KernelStatus;

template <typename TLeft, typename TRight>
concept EqualityComparable = requires(TLeft left, TRight right) { left == right; };
template <typename TValue>
concept BooleanConvertible = requires(TValue value) { static_cast<bool>(value); };
static_assert(EqualityComparable<Status, FileStatus>);
static_assert(EqualityComparable<FileStatus, Status>);
static_assert(!EqualityComparable<FileStatus, DatagramStatus>);
static_assert(!EqualityComparable<FileStatus, GenericStatus>);
static_assert(!BooleanConvertible<Status>);
static_assert(!EqualityComparable<Status, UInt32>);
static_assert(Status{}.IsSuccess());
static_assert(!Status{}.IsFailure());
static_assert(Status{FileStatus::NotFound}.Raw() == 0x8500'0001U);
static_assert(Status{FileStatus::NotFound} == FileStatus::NotFound);
static_assert(FileStatus::NotFound == Status{FileStatus::NotFound});
static_assert(Status{FileStatus::NotFound} != Status{DatagramStatus::NoRule});
static_assert(Status{FileStatus::NotFound}.As<FileStatus>() == FileStatus::NotFound);
static_assert(!Status{}.TryAs<FileStatus>().HasValue());
static_assert(!Status{FileStatus::NotFound}.TryAs<DatagramStatus>().HasValue());

// An independently owned, sparse vendor facility proves the central header needs no edit.
enum class VendorStatus : UInt32
{
    First = 1U,
    Third = 3U,
};
[[nodiscard]] constexpr Facility FacilityOf(VendorStatus) noexcept
{
    return Facility{127U};
}
[[nodiscard]] constexpr bool IsKnownStatusCode(VendorStatus, const UInt32 code) noexcept
{
    return (code == 1U) || (code == 3U);
}
static_assert(Status{VendorStatus::Third}.Raw() == 0xFF00'0003U);
static_assert(Status{VendorStatus::Third}.TryAs<VendorStatus>().HasValue());

void TestRaw(TestSupport::TestContext& context) noexcept
{
    const UInt32 rejected[]{1U, 0x7FFF'FFFFU, 0x0100'0001U, 0x8000'0000U, 0xFF00'0000U};
    for (const UInt32 raw : rejected)
    {
        const auto result = Status::FromRaw(raw);
        ASTERIA_EXPECT(context, "reserved encodings are rejected with a native validation status", !result.HasValue(),
                       result.GetError() == GenericStatus::InvalidArgument);
    }
    const UInt32 accepted[]{0U, 0x8000'0001U, 0xBF00'0001U, 0xC000'0001U, 0xDF00'0001U, 0xE000'0001U, 0xFFFF'FFFFU};
    for (const UInt32 raw : accepted)
    {
        const auto result = Status::FromRaw(raw);
        ASTERIA_EXPECT(context, "all facility allocations and unknown nonzero codes round trip", result.HasValue(),
                       result.GetValue().Raw() == raw, result.GetValue().IsSuccess() == (raw == 0U),
                       result.GetValue().IsFailure() == (raw != 0U),
                       result.GetValue().GetFacility().Number() == ((raw >> 24U) & 0x7FU),
                       result.GetValue().Code() == (raw & 0x00FF'FFFFU));
    }
    const Status hole = Status::FromRaw(0xFF00'0002U).GetValue();
    const auto failed = hole.TryAs<VendorStatus>();
    ASTERIA_EXPECT(context, "a sparse facility rejects holes and preserves the original status on failure",
                   !failed.HasValue(), failed.GetError() == hole, User::Posix::ToErrno(hole) == 5);
    const Status future = Status::FromRaw(0x8500'0005U).GetValue();
    ASTERIA_EXPECT(context, "a future file code is preserved but never cast to an enum",
                   !future.TryAs<FileStatus>().HasValue(), User::Posix::ToErrno(future) == 5);
    ASTERIA_EXPECT(context, "a known native status converts only to its facility",
                   Status{FileStatus::Corrupt}.TryAs<FileStatus>().HasValue(),
                   Status{FileStatus::Corrupt}.As<FileStatus>() == FileStatus::Corrupt,
                   !Status{FileStatus::Corrupt}.TryAs<KernelStatus>().HasValue());
}

void TestKernel(TestSupport::TestContext& context) noexcept
{
    ASTERIA_EXPECT(context, "kernel None is explicitly success and all known errors retain ABI identity",
                   User::Runtime::FromKernelError(0U).HasValue(),
                   User::Runtime::FromKernelError(0U).GetValue().IsSuccess(),
                   User::Runtime::FromKernelError(Abi::SystemCallError::None).GetValue().IsSuccess(),
                   User::Runtime::FromKernelError(Abi::SystemCallError::TimedOut).GetValue() == KernelStatus::TimedOut);
    for (UInt64 code{1U}; code <= static_cast<UInt64>(Abi::SystemCallError::Last); ++code)
    {
        const auto result = User::Runtime::FromKernelError(code);
        ASTERIA_EXPECT(context, "kernel errors validate before narrowing and preserve every ABI code",
                       result.HasValue(), result.GetValue().Code() == code,
                       result.GetValue().GetFacility() == User::Runtime::KernelFacility,
                       result.GetValue().TryAs<KernelStatus>().HasValue(),
                       static_cast<UInt32>(result.GetValue().As<KernelStatus>()) == code);
    }
    const auto future = User::Runtime::FromKernelError(static_cast<UInt64>(Abi::SystemCallError::Last) + 1U);
    ASTERIA_EXPECT(context, "a newer kernel code survives decoding and maps through the unknown EIO path",
                   future.HasValue(), future.GetValue().Raw() == 0x8100'0017U,
                   !future.GetValue().TryAs<KernelStatus>().HasValue(), User::Posix::ToErrno(future.GetValue()) == 5);
    const UInt64 invalid[]{0x0100'0000ULL, 0x1'0000'0001ULL, 0xFFFF'FFFF'FFFF'FFFFULL};
    for (const UInt64 raw : invalid)
    {
        ASTERIA_EXPECT(context, "ABI words outside the 24-bit code field are rejected before narrowing",
                       !User::Runtime::FromKernelError(raw).HasValue());
    }
}
} // namespace
} // namespace Asteria::Test

int main()
{
    Asteria::TestSupport::TestContext context{"StatusTests"};
    Asteria::Test::TestRaw(context);
    Asteria::Test::TestKernel(context);
    return context.Finish();
}
