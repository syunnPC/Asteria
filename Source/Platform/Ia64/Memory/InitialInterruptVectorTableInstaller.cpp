// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Memory/InitialInterruptVectorTableInstaller.hpp"

#include "Platform/Ia64/Memory/InitialAddressSpacePreparation.hpp"

extern "C" Asteria::Foundation::UInt64
AsteriaIa64InstallInitialInterruptVectorTable(Asteria::Foundation::UInt64 physicalStart) noexcept;

extern "C" Asteria::Platform::Ia64::Memory::InitialInterruptVectorTableApInstallStatus
AsteriaIa64InstallApplicationProcessorInitialInterruptVectorTable(Asteria::Foundation::UInt64 physicalStart) noexcept;

// The target entry points are in InitialInterruptVectorTableInstaller.S.

namespace Asteria::Platform::Ia64::Memory
{
namespace
{
constexpr Foundation::Size NoByteOffset{~Foundation::Size{0U}};
constexpr Foundation::UInt64 RegionZeroSizeBytes{Foundation::UInt64{1U} << 61U};

using InstallResult =
    Foundation::Result<InitialInterruptVectorTableInstallSummary, InitialInterruptVectorTableInstallFailure>;

[[nodiscard]] InstallResult Fail(const InitialInterruptVectorTableInstallError error,
                                 const InitialInterruptVectorTablePreparationError preparationError =
                                     InitialInterruptVectorTablePreparationError::None,
                                 const Foundation::Size byteOffset = NoByteOffset) noexcept
{
    return InstallResult::Failure(InitialInterruptVectorTableInstallFailure{
        .Error = error,
        .PreparationError = preparationError,
        .ByteOffset = byteOffset,
    });
}

[[nodiscard]] constexpr bool
IsApplicationProcessorSummaryValid(const InitialInterruptVectorTableInstallSummary& summary) noexcept
{
    constexpr Foundation::UInt64 tableSizeBytes{InitialInterruptVectorTableSizeBytes};
    constexpr Foundation::UInt32 expectedSizeBytes{
        static_cast<Foundation::UInt32>(InitialInterruptVectorTableSizeBytes)};
    constexpr Foundation::UInt16 expectedBundleCount{
        static_cast<Foundation::UInt16>(InitialInterruptVectorTableSizeBytes / InitialInterruptVectorBundleSizeBytes)};

    return (summary.PhysicalStart != 0U) && (summary.PhysicalStart <= (RegionZeroSizeBytes - tableSizeBytes)) &&
           ((summary.PhysicalStart & (tableSizeBytes - 1U)) == 0U) && (summary.SizeBytes == expectedSizeBytes) &&
           (summary.BundleCount == expectedBundleCount) && (summary.Reserved == 0U);
}
} // namespace

InstallResult InitialInterruptVectorTableInstaller::Validate(const InitialAddressSpacePreparation& preparation,
                                                             const WritableInterruptVectorTableView& table) noexcept
{
    const auto mappingPlan = preparation.TryGetMappingPlan();
    const auto summary = preparation.TryGetSummary();
    const auto translationPlan = preparation.TryGetTranslationResourcePlan();
    if (!preparation.IsPrepared() || !mappingPlan.HasValue() || !summary.HasValue() || !translationPlan.HasValue() ||
        !translationPlan.GetValue()->IsBuilt())
    {
        return Fail(InitialInterruptVectorTableInstallError::PreparationNotReady);
    }

    const InitialInterruptVectorTablePreparationError preparationError =
        InitialInterruptVectorTablePreparer::Validate(*mappingPlan.GetValue(), summary.GetValue()->Mapping, table);
    if (preparationError != InitialInterruptVectorTablePreparationError::None)
    {
        return Fail(InitialInterruptVectorTableInstallError::PreparedTableRejected, preparationError);
    }

    for (Foundation::Size offset{0U}; offset < InitialInterruptVectorTableSizeBytes; ++offset)
    {
        const Foundation::Size bundleByte = offset % InitialInterruptVectorBundleSizeBytes;
        Foundation::UInt8 expected{0U};
        if (bundleByte == 0U)
        {
            expected = 0x11U;
        }
        else if (bundleByte == 4U)
        {
            expected = 0x01U;
        }
        else if (bundleByte == 9U)
        {
            expected = 0x02U;
        }
        else if (bundleByte == 15U)
        {
            expected = 0x40U;
        }
        if (table.Data[offset] != expected)
        {
            return Fail(InitialInterruptVectorTableInstallError::InvalidTableContents,
                        InitialInterruptVectorTablePreparationError::None, offset);
        }
    }

    return InstallResult::Success(InitialInterruptVectorTableInstallSummary{
        .PhysicalStart = table.PhysicalStart,
        .SizeBytes = static_cast<Foundation::UInt32>(InitialInterruptVectorTableSizeBytes),
        .BundleCount = static_cast<Foundation::UInt16>(InitialInterruptVectorTableSizeBytes /
                                                       InitialInterruptVectorBundleSizeBytes),
        .Reserved = 0U,
    });
}

InstallResult InitialInterruptVectorTableInstaller::Install(const InitialAddressSpacePreparation& preparation,
                                                            const WritableInterruptVectorTableView& table) noexcept
{
    const InstallResult validation = Validate(preparation, table);
    if (!validation.HasValue())
    {
        return validation;
    }

    if (AsteriaIa64InstallInitialInterruptVectorTable(validation.GetValue().PhysicalStart) != 0U)
    {
        return Fail(InitialInterruptVectorTableInstallError::UnsupportedProcessorState);
    }
    return validation;
}

InitialInterruptVectorTableApInstallStatus InitialInterruptVectorTableInstaller::InstallForApplicationProcessor(
    const InitialInterruptVectorTableInstallSummary summary) noexcept
{
    if (!IsApplicationProcessorSummaryValid(summary))
    {
        return InitialInterruptVectorTableApInstallStatus::InvalidSummary;
    }

    const InitialInterruptVectorTableApInstallStatus adapterStatus =
        AsteriaIa64InstallApplicationProcessorInitialInterruptVectorTable(summary.PhysicalStart);
    if (adapterStatus == InitialInterruptVectorTableApInstallStatus::Installed)
    {
        return InitialInterruptVectorTableApInstallStatus::Installed;
    }
    if (adapterStatus == InitialInterruptVectorTableApInstallStatus::UnsupportedProcessorState)
    {
        return InitialInterruptVectorTableApInstallStatus::UnsupportedProcessorState;
    }
    if (adapterStatus == InitialInterruptVectorTableApInstallStatus::CommittedReadbackMismatch)
    {
        return InitialInterruptVectorTableApInstallStatus::CommittedReadbackMismatch;
    }

    // The native adapter has exactly three exits. Conservatively treat an
    // impossible ABI value as post-commit because normal continuation would
    // be unsafe when commit disposition cannot be established.
    return InitialInterruptVectorTableApInstallStatus::CommittedReadbackMismatch;
}
} // namespace Asteria::Platform::Ia64::Memory
