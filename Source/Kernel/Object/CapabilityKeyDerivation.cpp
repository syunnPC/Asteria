// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Object/CapabilityKeyDerivation.hpp"

#include "Kernel/Security/Sha256.hpp"

namespace Asteria::Kernel::Object
{
namespace
{
using Foundation::Size;
using Foundation::UInt32;
using Foundation::UInt64;
using Foundation::UInt8;

// HKDF-Expand info prefix: the ASCII text "Asteria capability validation key v1".
constexpr UInt8 ExpandInfo[]{
    'A', 's', 't', 'e', 'r', 'i', 'a', ' ', 'c', 'a', 'p', 'a', 'b', 'i', 'l', 'i', 't', 'y',
    ' ', 'v', 'a', 'l', 'i', 'd', 'a', 't', 'i', 'o', 'n', ' ', 'k', 'e', 'y', ' ', 'v', '1',
};
// The one-byte counter of the first HKDF-Expand block.
constexpr UInt8 FirstBlockCounter{0x01U};

[[nodiscard]] bool IsKnownPurpose(const CapabilityKeyPurpose purpose) noexcept
{
    switch (purpose)
    {
    case CapabilityKeyPurpose::BootstrapSpace:
    case CapabilityKeyPurpose::DeviceSpace:
    case CapabilityKeyPurpose::PagerSpace:
    case CapabilityKeyPurpose::FaultTokenSpace:
    case CapabilityKeyPurpose::SpinnerSpace:
    case CapabilityKeyPurpose::RuntimeSpace0:
    case CapabilityKeyPurpose::RuntimeSpace1:
    case CapabilityKeyPurpose::RuntimeSpace2:
    case CapabilityKeyPurpose::RuntimeSpace3:
    case CapabilityKeyPurpose::RuntimeSpace4:
    case CapabilityKeyPurpose::RuntimeSpace5:
    case CapabilityKeyPurpose::RuntimeSpace6:
    case CapabilityKeyPurpose::RuntimeSpace7:
    case CapabilityKeyPurpose::RuntimeSpace8:
    case CapabilityKeyPurpose::RuntimeSpace9:
    case CapabilityKeyPurpose::RuntimeSpace10:
    case CapabilityKeyPurpose::RuntimeSpace11:
    case CapabilityKeyPurpose::RuntimeSpace12:
    case CapabilityKeyPurpose::RuntimeSpace13:
    case CapabilityKeyPurpose::RuntimeSpace14:
    case CapabilityKeyPurpose::RuntimeSpace15:
        return true;
    }
    return false;
}

[[nodiscard]] UInt64 LoadLittleEndian64(const UInt8* const bytes) noexcept
{
    UInt64 value{0U};
    for (Size index{0U}; index < 8U; ++index)
    {
        value |= static_cast<UInt64>(bytes[index]) << (index * 8U);
    }
    return value;
}
} // namespace

Foundation::Result<CapabilityValidationKey, CapabilityKeyError>
DeriveCapabilityValidationKey(const Boot::BootEntropy& entropy, const CapabilityKeyPurpose purpose) noexcept
{
    using ResultType = Foundation::Result<CapabilityValidationKey, CapabilityKeyError>;
    if (!IsKnownPurpose(purpose))
    {
        return ResultType::Failure(CapabilityKeyError::InvalidPurpose);
    }
    switch (Boot::GetBootEntropyState(entropy))
    {
    case Boot::BootEntropyState::Published:
        break;
    case Boot::BootEntropyState::Unpublished:
        return ResultType::Failure(CapabilityKeyError::MissingEntropy);
    case Boot::BootEntropyState::DegenerateSeed:
        return ResultType::Failure(CapabilityKeyError::DegenerateSeed);
    }

    const UInt32 purposeValue = static_cast<UInt32>(purpose);
    const UInt8 purposeBytes[4U]{
        static_cast<UInt8>(purposeValue),
        static_cast<UInt8>(purposeValue >> 8U),
        static_cast<UInt8>(purposeValue >> 16U),
        static_cast<UInt8>(purposeValue >> 24U),
    };
    Security::HmacSha256 expand{entropy.Seed, Boot::BootEntropySeedBytes};
    expand.Update(ExpandInfo, sizeof(ExpandInfo));
    expand.Update(purposeBytes, sizeof(purposeBytes));
    expand.Update(&FirstBlockCounter, 1U);
    UInt8 block[Security::Sha256DigestBytes]{};
    expand.Finish(block);
    return ResultType::Success(CapabilityValidationKey{
        .Word0 = LoadLittleEndian64(block),
        .Word1 = LoadLittleEndian64(block + 8U),
    });
}
} // namespace Asteria::Kernel::Object
