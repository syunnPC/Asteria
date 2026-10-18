// SPDX-License-Identifier: Apache-2.0
// Capability-space validation keys derived from the boot seed
// (CapabilityModel.md): the documented HKDF-SHA256 construction (known
// answers from an independent implementation), one key per purpose, and the
// rejection of an unpublished or degenerate entropy record.

#include "Kernel/Object/CapabilityKeyDerivation.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Boot::BootEntropy;
using Boot::BootEntropySource;
using Boot::GetBootEntropySourceMask;
using Foundation::Size;
using Foundation::UInt32;
using Foundation::UInt64;
using Foundation::UInt8;
using Kernel::Object::CapabilityKeyError;
using Kernel::Object::CapabilityKeyPurpose;
using Kernel::Object::CapabilityValidationKey;
using Kernel::Object::DeriveCapabilityValidationKey;

constexpr UInt32 FirmwareRng{GetBootEntropySourceMask(BootEntropySource::FirmwareRng)};
constexpr UInt32 ProcessorRng{GetBootEntropySourceMask(BootEntropySource::ProcessorRng)};
constexpr UInt32 Jitter{GetBootEntropySourceMask(BootEntropySource::CycleCounterJitter)};
constexpr UInt32 FirmwareState{GetBootEntropySourceMask(BootEntropySource::FirmwareState)};

// Seed byte i is i.
[[nodiscard]] BootEntropy CountingSeed(const UInt32 sources) noexcept
{
    BootEntropy entropy{};
    for (Size index{0U}; index < Boot::BootEntropySeedBytes; ++index)
    {
        entropy.Seed[index] = static_cast<UInt8>(index);
    }
    entropy.Sources = sources;
    return entropy;
}

[[nodiscard]] BootEntropy UniformSeed(const UInt8 value, const UInt32 sources) noexcept
{
    BootEntropy entropy{};
    for (UInt8& byte : entropy.Seed)
    {
        byte = value;
    }
    entropy.Sources = sources;
    return entropy;
}

[[nodiscard]] bool KeyIs(const BootEntropy& entropy, const CapabilityKeyPurpose purpose, const UInt64 word0,
                         const UInt64 word1) noexcept
{
    const auto key = DeriveCapabilityValidationKey(entropy, purpose);
    return key.HasValue() && (key.GetValue().Word0 == word0) && (key.GetValue().Word1 == word1);
}

[[nodiscard]] CapabilityKeyError ErrorOf(const BootEntropy& entropy, const CapabilityKeyPurpose purpose) noexcept
{
    const auto key = DeriveCapabilityValidationKey(entropy, purpose);
    return key.HasValue() ? CapabilityKeyError::None : key.GetError();
}

[[nodiscard]] bool KeysDiffer(const CapabilityValidationKey& first, const CapabilityValidationKey& second) noexcept
{
    return (first.Word0 != second.Word0) || (first.Word1 != second.Word1);
}

void TestDocumentedConstruction(TestContext& runner) noexcept
{
    // HMAC-SHA256(seed, "Asteria capability validation key v1" || LE32(purpose) || 0x01)[0..16).
    const BootEntropy counting = CountingSeed(FirmwareRng);
    ASTERIA_EXPECT(
        runner, "the bootstrap space key is the first HKDF-Expand block of its purpose",
        KeyIs(counting, CapabilityKeyPurpose::BootstrapSpace, 0x942F'BA65'20A6'F982ULL, 0x6EAD'EA58'DAAF'EFDFULL));
    ASTERIA_EXPECT(
        runner, "the device space key is derived with its own purpose",
        KeyIs(counting, CapabilityKeyPurpose::DeviceSpace, 0x8D80'ADA5'F732'71ECULL, 0x9829'489C'D687'615BULL));
    ASTERIA_EXPECT(
        runner, "the pager and fault-token space keys are derived with their own purposes",
        KeyIs(counting, CapabilityKeyPurpose::PagerSpace, 0xA8CF'3AA2'0F2B'0D39ULL, 0x5E52'71E5'1A54'D28DULL),
        KeyIs(counting, CapabilityKeyPurpose::FaultTokenSpace, 0x57C7'3715'5784'9660ULL, 0x55E5'31AC'C676'8B48ULL));
    ASTERIA_EXPECT(
        runner, "the spinner space key is derived with its own purpose",
        KeyIs(counting, CapabilityKeyPurpose::SpinnerSpace, 0x05F2'0BD4'E7F4'36FEULL, 0x5BD7'F84E'652E'1E64ULL));
    ASTERIA_EXPECT(
        runner, "each created-space slot's key is derived with its own purpose",
        KeyIs(counting, CapabilityKeyPurpose::RuntimeSpace0, 0xD3B6'8729'A1CF'F0BCULL, 0xAAA1'F53E'83ED'85AFULL),
        KeyIs(counting, CapabilityKeyPurpose::RuntimeSpace1, 0x4991'E224'C017'BB43ULL, 0xEEA8'C546'B11E'9E53ULL),
        KeyIs(counting, CapabilityKeyPurpose::RuntimeSpace4, 0xC5D3'241E'63F8'2EB1ULL, 0x6AF2'94D0'A3FC'86BBULL));

    // Only the last seed byte differs from a uniform seed: still a usable
    // seed, and a different key.
    BootEntropy nearlyUniform = UniformSeed(0x5AU, Jitter);
    nearlyUniform.Seed[Boot::BootEntropySeedBytes - 1U] = 0x5BU;
    ASTERIA_EXPECT(
        runner, "a seed differing only in its last byte is accepted and changes every key",
        KeyIs(nearlyUniform, CapabilityKeyPurpose::BootstrapSpace, 0xD920'6D11'8C18'5D9DULL, 0x1125'C86C'78CF'23A7ULL),
        KeyIs(nearlyUniform, CapabilityKeyPurpose::DeviceSpace, 0xBD15'6644'DB87'381BULL, 0x2CC2'221D'7507'A921ULL));
}

void TestKeysAreDistinct(TestContext& runner) noexcept
{
    const auto bootstrap = DeriveCapabilityValidationKey(CountingSeed(Jitter), CapabilityKeyPurpose::BootstrapSpace);
    const auto device = DeriveCapabilityValidationKey(CountingSeed(Jitter), CapabilityKeyPurpose::DeviceSpace);
    BootEntropy otherBoot = CountingSeed(Jitter);
    otherBoot.Seed[0U] ^= 0x80U;
    const auto otherBootstrap = DeriveCapabilityValidationKey(otherBoot, CapabilityKeyPurpose::BootstrapSpace);
    ASTERIA_EXPECT(runner, "the spaces of one boot get distinct keys and another seed changes them",
                   bootstrap.HasValue(), device.HasValue(), otherBootstrap.HasValue(),
                   KeysDiffer(bootstrap.GetValue(), device.GetValue()),
                   KeysDiffer(bootstrap.GetValue(), otherBootstrap.GetValue()));

    // The source mask qualifies the record; it is not key material.
    const auto withState = DeriveCapabilityValidationKey(CountingSeed(Jitter | ProcessorRng | FirmwareState),
                                                         CapabilityKeyPurpose::BootstrapSpace);
    ASTERIA_EXPECT(runner, "every credited source, with firmware state mixed in, qualifies the same seed",
                   withState.HasValue(), !KeysDiffer(bootstrap.GetValue(), withState.GetValue()),
                   ErrorOf(CountingSeed(ProcessorRng), CapabilityKeyPurpose::DeviceSpace) == CapabilityKeyError::None);
}

void TestDegenerateRecordsAreRejected(TestContext& runner) noexcept
{
    ASTERIA_EXPECT(runner, "an all-zero record, as a reset BootInfo holds, is rejected",
                   ErrorOf(BootEntropy{}, CapabilityKeyPurpose::BootstrapSpace) == CapabilityKeyError::MissingEntropy);
    ASTERIA_EXPECT(runner, "firmware state alone is never credited",
                   ErrorOf(CountingSeed(FirmwareState), CapabilityKeyPurpose::BootstrapSpace) ==
                       CapabilityKeyError::MissingEntropy);
    ASTERIA_EXPECT(runner, "an unknown source bit marks a record the boot application did not publish",
                   ErrorOf(CountingSeed(FirmwareRng | (FirmwareState << 1U)), CapabilityKeyPurpose::BootstrapSpace) ==
                       CapabilityKeyError::MissingEntropy);
    BootEntropy reserved = CountingSeed(FirmwareRng);
    reserved.Reserved = 1U;
    ASTERIA_EXPECT(runner, "a nonzero reserved field is rejected",
                   ErrorOf(reserved, CapabilityKeyPurpose::BootstrapSpace) == CapabilityKeyError::MissingEntropy);
    ASTERIA_EXPECT(runner, "an all-zero seed yields no key even with a credited source",
                   ErrorOf(UniformSeed(0x00U, FirmwareRng), CapabilityKeyPurpose::BootstrapSpace) ==
                       CapabilityKeyError::DegenerateSeed);
    ASTERIA_EXPECT(runner, "a seed of one repeated byte yields no key",
                   ErrorOf(UniformSeed(0xFFU, Jitter), CapabilityKeyPurpose::DeviceSpace) ==
                       CapabilityKeyError::DegenerateSeed);
    ASTERIA_EXPECT(runner, "an unknown purpose yields no key",
                   ErrorOf(CountingSeed(FirmwareRng), static_cast<CapabilityKeyPurpose>(0U)) ==
                       CapabilityKeyError::InvalidPurpose,
                   ErrorOf(CountingSeed(FirmwareRng), static_cast<CapabilityKeyPurpose>(22U)) ==
                       CapabilityKeyError::InvalidPurpose);
}
} // Namespace
} // Namespace Asteria::Test

int main()
{
    TestContext runner{"CapabilityKeyDerivationTests"};
    Asteria::Test::TestDocumentedConstruction(runner);
    Asteria::Test::TestKeysAreDistinct(runner);
    Asteria::Test::TestDegenerateRecordsAreRejected(runner);
    return runner.Finish();
}
