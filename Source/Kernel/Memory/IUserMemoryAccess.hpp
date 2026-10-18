// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_MEMORY_I_USER_MEMORY_ACCESS_HPP_INCLUDED
#define ASTERIA_KERNEL_MEMORY_I_USER_MEMORY_ACCESS_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Memory/FaultToken.hpp"

namespace Asteria::Kernel::Memory
{
enum class UserMemoryRangeError : Foundation::UInt8
{
    None,
    NullAddress,
    NonCanonicalEmptyRange,
    RangeTooLarge,
    AddressOverflow,
};

/// One bounded 64-bit user virtual-address interval. The sole empty form is
/// {address 0, size 0}; every nonempty form has a nonzero address and a
/// representable exclusive end.
class UserMemoryRange final
{
public:
    static constexpr Foundation::Size MaximumSizeBytes{4'096U};

    constexpr UserMemoryRange() noexcept = default;

    [[nodiscard]] static constexpr Foundation::Result<UserMemoryRange, UserMemoryRangeError>
    Create(const Foundation::UInt64 startAddress, const Foundation::Size sizeBytes) noexcept
    {
        using ResultType = Foundation::Result<UserMemoryRange, UserMemoryRangeError>;

        if (sizeBytes == 0U)
        {
            return startAddress == 0U ? ResultType::Success(UserMemoryRange{})
                                      : ResultType::Failure(UserMemoryRangeError::NonCanonicalEmptyRange);
        }
        if (startAddress == 0U)
        {
            return ResultType::Failure(UserMemoryRangeError::NullAddress);
        }
        if (sizeBytes > MaximumSizeBytes)
        {
            return ResultType::Failure(UserMemoryRangeError::RangeTooLarge);
        }

        const Foundation::UInt64 size = static_cast<Foundation::UInt64>(sizeBytes);
        const Foundation::UInt64 maximum{~Foundation::UInt64{0U}};
        if (startAddress > (maximum - size))
        {
            return ResultType::Failure(UserMemoryRangeError::AddressOverflow);
        }
        return ResultType::Success(UserMemoryRange{startAddress, sizeBytes});
    }

    [[nodiscard]] constexpr Foundation::UInt64 GetStartAddress() const noexcept
    {
        return m_startAddress;
    }

    [[nodiscard]] constexpr Foundation::UInt64 GetEndAddress() const noexcept
    {
        return m_startAddress + static_cast<Foundation::UInt64>(m_sizeBytes);
    }

    [[nodiscard]] constexpr Foundation::Size GetSizeBytes() const noexcept
    {
        return m_sizeBytes;
    }

    [[nodiscard]] constexpr bool IsEmpty() const noexcept
    {
        return m_sizeBytes == 0U;
    }

    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        if (m_sizeBytes == 0U)
        {
            return m_startAddress == 0U;
        }
        if ((m_startAddress == 0U) || (m_sizeBytes > MaximumSizeBytes))
        {
            return false;
        }
        const Foundation::UInt64 maximum{~Foundation::UInt64{0U}};
        return m_startAddress <= (maximum - static_cast<Foundation::UInt64>(m_sizeBytes));
    }

    [[nodiscard]] constexpr bool IsEqualTo(const UserMemoryRange other) const noexcept
    {
        return (m_startAddress == other.m_startAddress) && (m_sizeBytes == other.m_sizeBytes);
    }

private:
    constexpr UserMemoryRange(const Foundation::UInt64 startAddress, const Foundation::Size sizeBytes) noexcept
        : m_startAddress{startAddress}, m_sizeBytes{sizeBytes}
    {
    }

    Foundation::UInt64 m_startAddress{0U};
    Foundation::Size m_sizeBytes{0U};
};

static_assert(sizeof(UserMemoryRange) == 16U);
static_assert(alignof(UserMemoryRange) == 8U);

struct ReadOnlyKernelBuffer
{
    const Foundation::UInt8* Data;
    Foundation::Size SizeBytes;
};

struct WritableKernelBuffer
{
    Foundation::UInt8* Data;
    Foundation::Size SizeBytes;
};

static_assert(sizeof(ReadOnlyKernelBuffer) == 16U);
static_assert(sizeof(WritableKernelBuffer) == 16U);

enum class UserMemoryAccessError : Foundation::UInt8
{
    None,
    InvalidRange,
    NullKernelBuffer,
    NonCanonicalEmptyBuffer,
    KernelBufferOverflow,
    SizeMismatch,
    NotReady,
    AccessDenied,
    PinFailed,
    Fault,
    ReservationExhausted,
    ForeignReservation,
    StaleReservation,
    BackendProtocolViolation,
};

class IUserMemoryAccess;

/// Names one backend-owned, fully validated and pinned user write interval.
/// The originating IUserMemoryAccess outlives every copy and use of the token.
class UserMemoryWriteReservation final
{
public:
    constexpr UserMemoryWriteReservation() noexcept = default;

    [[nodiscard]] constexpr UserMemoryRange GetRange() const noexcept
    {
        return m_range;
    }

    [[nodiscard]] constexpr Foundation::UInt64 GetGeneration() const noexcept
    {
        return m_generation;
    }

    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        return (m_owner != nullptr) && m_range.IsValid() && (m_generation != 0U);
    }

    [[nodiscard]] constexpr bool IsFor(const IUserMemoryAccess& owner) const noexcept
    {
        return m_owner == &owner;
    }

    [[nodiscard]] constexpr bool IsEqualTo(const UserMemoryWriteReservation other) const noexcept
    {
        return (m_owner == other.m_owner) && m_range.IsEqualTo(other.m_range) && (m_generation == other.m_generation);
    }

private:
    friend class IUserMemoryAccess;

    constexpr UserMemoryWriteReservation(const IUserMemoryAccess& owner, const UserMemoryRange range,
                                         const Foundation::UInt64 generation) noexcept
        : m_owner{&owner}, m_range{range}, m_generation{generation}
    {
    }

    const IUserMemoryAccess* m_owner{nullptr};
    UserMemoryRange m_range{};
    Foundation::UInt64 m_generation{0U};
};

static_assert(sizeof(UserMemoryWriteReservation) == 32U);
static_assert(alignof(UserMemoryWriteReservation) == 8U);

/// Fault-contained access to one exact user AddressSpace generation.
///
/// Implementations may pin mappings and contain native faults during each
/// fallible operation, but may not allocate. CopyFromUser must not modify the
/// kernel destination on failure. PrepareCopyToUser must make every access and
/// pinning failure observable before returning a reservation. Cancel publishes
/// no byte. For a live owner-matched reservation and exact stable source,
/// CommitCopyToUser is a guaranteed publication: the backend must return None
/// and may not report a fault, access, or pinning failure after writing begins.
class IUserMemoryAccess
{
public:
    IUserMemoryAccess(const IUserMemoryAccess&) = delete;
    IUserMemoryAccess& operator=(const IUserMemoryAccess&) = delete;
    IUserMemoryAccess(IUserMemoryAccess&&) = delete;
    IUserMemoryAccess& operator=(IUserMemoryAccess&&) = delete;

    [[nodiscard]] virtual AddressSpaceId GetAddressSpaceId() const noexcept = 0;
    [[nodiscard]] virtual bool IsReady() const noexcept = 0;

    [[nodiscard]] UserMemoryAccessError CopyFromUser(const UserMemoryRange source,
                                                     const WritableKernelBuffer destination) noexcept
    {
        const UserMemoryAccessError shapeError = ValidateBuffer(source, destination.Data, destination.SizeBytes);
        if (shapeError != UserMemoryAccessError::None)
        {
            return shapeError;
        }
        if (source.IsEmpty())
        {
            return UserMemoryAccessError::None;
        }
        if (!IsReady())
        {
            return UserMemoryAccessError::NotReady;
        }
        return CopyFromUserValidated(source, destination);
    }

    [[nodiscard]] Foundation::Result<UserMemoryWriteReservation, UserMemoryAccessError>
    PrepareCopyToUser(const UserMemoryRange destination) noexcept
    {
        using ResultType = Foundation::Result<UserMemoryWriteReservation, UserMemoryAccessError>;

        if (!destination.IsValid())
        {
            return ResultType::Failure(UserMemoryAccessError::InvalidRange);
        }
        if (!IsReady())
        {
            return ResultType::Failure(UserMemoryAccessError::NotReady);
        }

        const auto result = PrepareCopyToUserValidated(destination);
        if (!result.HasValue())
        {
            if (result.GetError() == UserMemoryAccessError::None)
            {
                return ResultType::Failure(UserMemoryAccessError::BackendProtocolViolation);
            }
            return result;
        }
        const UserMemoryWriteReservation& reservation = result.GetValue();
        if (!reservation.IsValid() || !reservation.IsFor(*this) || !reservation.GetRange().IsEqualTo(destination))
        {
            return ResultType::Failure(UserMemoryAccessError::BackendProtocolViolation);
        }
        return result;
    }

    [[nodiscard]] UserMemoryAccessError CancelCopyToUser(const UserMemoryWriteReservation reservation) noexcept
    {
        if (!reservation.IsValid() || !reservation.IsFor(*this))
        {
            return UserMemoryAccessError::ForeignReservation;
        }
        return CancelCopyToUserValidated(reservation);
    }

    [[nodiscard]] UserMemoryAccessError CommitCopyToUser(const UserMemoryWriteReservation reservation,
                                                         const ReadOnlyKernelBuffer source) noexcept
    {
        if (!reservation.IsValid() || !reservation.IsFor(*this))
        {
            return UserMemoryAccessError::ForeignReservation;
        }
        const UserMemoryAccessError shapeError = ValidateBuffer(reservation.GetRange(), source.Data, source.SizeBytes);
        if (shapeError != UserMemoryAccessError::None)
        {
            return shapeError;
        }
        return CommitCopyToUserValidated(reservation, source);
    }

protected:
    /// Backend storage is externally owned and is never deleted through this
    /// interface.
    constexpr IUserMemoryAccess() noexcept = default;
    ~IUserMemoryAccess() noexcept = default;

    [[nodiscard]] constexpr UserMemoryWriteReservation
    MakeWriteReservation(const UserMemoryRange range, const Foundation::UInt64 generation) const noexcept
    {
        return UserMemoryWriteReservation{*this, range, generation};
    }

    [[nodiscard]] virtual UserMemoryAccessError CopyFromUserValidated(UserMemoryRange source,
                                                                      WritableKernelBuffer destination) noexcept = 0;
    [[nodiscard]] virtual Foundation::Result<UserMemoryWriteReservation, UserMemoryAccessError>
    PrepareCopyToUserValidated(UserMemoryRange destination) noexcept = 0;
    [[nodiscard]] virtual UserMemoryAccessError
    CancelCopyToUserValidated(UserMemoryWriteReservation reservation) noexcept = 0;
    [[nodiscard]] virtual UserMemoryAccessError CommitCopyToUserValidated(UserMemoryWriteReservation reservation,
                                                                          ReadOnlyKernelBuffer source) noexcept = 0;

private:
    [[nodiscard]] static UserMemoryAccessError ValidateBuffer(const UserMemoryRange range, const void* const buffer,
                                                              const Foundation::Size sizeBytes) noexcept
    {
        if (!range.IsValid())
        {
            return UserMemoryAccessError::InvalidRange;
        }
        if (sizeBytes != range.GetSizeBytes())
        {
            return UserMemoryAccessError::SizeMismatch;
        }
        if (sizeBytes == 0U)
        {
            return buffer == nullptr ? UserMemoryAccessError::None : UserMemoryAccessError::NonCanonicalEmptyBuffer;
        }
        if (buffer == nullptr)
        {
            return UserMemoryAccessError::NullKernelBuffer;
        }

        const Foundation::UIntPointer start = reinterpret_cast<Foundation::UIntPointer>(buffer);
        const Foundation::UIntPointer maximum{~Foundation::UIntPointer{0U}};
        const Foundation::UIntPointer size = static_cast<Foundation::UIntPointer>(sizeBytes);
        return start <= (maximum - size) ? UserMemoryAccessError::None : UserMemoryAccessError::KernelBufferOverflow;
    }
};
} // namespace Asteria::Kernel::Memory

#endif // ASTERIA_KERNEL_MEMORY_I_USER_MEMORY_ACCESS_HPP_INCLUDED
