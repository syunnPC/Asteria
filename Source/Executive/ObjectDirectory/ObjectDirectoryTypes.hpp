// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_EXECUTIVE_OBJECT_DIRECTORY_OBJECT_DIRECTORY_TYPES_HPP_INCLUDED
#define ASTERIA_EXECUTIVE_OBJECT_DIRECTORY_OBJECT_DIRECTORY_TYPES_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"

namespace Asteria::Executive::ObjectDirectory
{
struct DirectoryNameView
{
    const Foundation::UInt8* Data;
    Foundation::Size SizeBytes;
};

enum class DirectoryNameError : Foundation::UInt8
{
    None,
    NullData,
    InvalidSize,
    InvalidCharacter,
};

/// Canonical technical name for one Object Directory node.
///
/// Names contain one to 64 lowercase ASCII bytes. The first and last bytes are
/// alphanumeric; interior bytes may additionally contain '.', '-', or '_'.
/// The class owns its fixed storage and performs no allocation.
class DirectoryName final
{
public:
    static constexpr Foundation::Size MaximumSizeBytes{64U};

    constexpr DirectoryName() noexcept = default;

    [[nodiscard]] static constexpr Foundation::Result<DirectoryName, DirectoryNameError>
    Create(const DirectoryNameView view) noexcept
    {
        using ResultType = Foundation::Result<DirectoryName, DirectoryNameError>;

        if (view.Data == nullptr)
        {
            return ResultType::Failure(DirectoryNameError::NullData);
        }
        if ((view.SizeBytes == 0U) || (view.SizeBytes > MaximumSizeBytes))
        {
            return ResultType::Failure(DirectoryNameError::InvalidSize);
        }

        for (Foundation::Size index{0U}; index < view.SizeBytes; ++index)
        {
            const Foundation::UInt8 value = view.Data[index];
            const bool isDigit =
                (value >= static_cast<Foundation::UInt8>('0')) && (value <= static_cast<Foundation::UInt8>('9'));
            const bool isLowercase =
                (value >= static_cast<Foundation::UInt8>('a')) && (value <= static_cast<Foundation::UInt8>('z'));
            const bool isInteriorPunctuation = (value == static_cast<Foundation::UInt8>('.')) ||
                                               (value == static_cast<Foundation::UInt8>('-')) ||
                                               (value == static_cast<Foundation::UInt8>('_'));
            const bool isBoundary = (index == 0U) || ((index + 1U) == view.SizeBytes);
            if ((!isDigit && !isLowercase) && (isBoundary || !isInteriorPunctuation))
            {
                return ResultType::Failure(DirectoryNameError::InvalidCharacter);
            }
        }

        DirectoryName name{};
        for (Foundation::Size index{0U}; index < view.SizeBytes; ++index)
        {
            name.m_bytes[index] = view.Data[index];
        }
        name.m_sizeBytes = static_cast<Foundation::UInt8>(view.SizeBytes);
        return ResultType::Success(name);
    }

    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        return m_sizeBytes != 0U;
    }

    [[nodiscard]] constexpr const Foundation::UInt8* GetData() const noexcept
    {
        return m_bytes;
    }

    [[nodiscard]] constexpr Foundation::Size GetSizeBytes() const noexcept
    {
        return m_sizeBytes;
    }

    [[nodiscard]] constexpr bool IsEqualTo(const DirectoryName& other) const noexcept
    {
        if (m_sizeBytes != other.m_sizeBytes)
        {
            return false;
        }
        for (Foundation::Size index{0U}; index < m_sizeBytes; ++index)
        {
            if (m_bytes[index] != other.m_bytes[index])
            {
                return false;
            }
        }
        return true;
    }

private:
    Foundation::UInt8 m_bytes[MaximumSizeBytes]{};
    Foundation::UInt8 m_sizeBytes{0U};
};

struct BindingVersion
{
    Foundation::UInt64 DirectoryGeneration{0U};
    Foundation::UInt64 Revision{0U};
};

struct InterfaceVersionRange
{
    Foundation::UInt16 Major;
    Foundation::UInt16 MinimumMinor;
    Foundation::UInt16 MaximumMinor;
    Foundation::UInt16 Reserved;
};

enum class DirectoryStatus : Foundation::UInt16
{
    Success = 0U,
    InvalidRequest = 1U,
    NotFound = 2U,
    AlreadyBound = 3U,
    BindingChanged = 4U,
    PermissionDenied = 5U,
    NoCompatibleVersion = 6U,
    RightsUnavailable = 7U,
    LeaseUnavailable = 8U,
    ResourceExhausted = 9U,
    InternalFailure = 10U,
};

static_assert(sizeof(DirectoryNameView) == 16U);
static_assert(alignof(DirectoryNameView) == 8U);
static_assert(sizeof(DirectoryNameError) == 1U);
static_assert(sizeof(BindingVersion) == 16U);
static_assert(alignof(BindingVersion) == 8U);
static_assert(sizeof(InterfaceVersionRange) == 8U);
static_assert(sizeof(DirectoryStatus) == 2U);
} // namespace Asteria::Executive::ObjectDirectory

#endif // ASTERIA_EXECUTIVE_OBJECT_DIRECTORY_OBJECT_DIRECTORY_TYPES_HPP_INCLUDED
