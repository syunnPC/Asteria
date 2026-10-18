// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Firmware/SalSystemTableCapture.hpp"

#include "Runtime/Memory.hpp"

namespace Asteria::Platform::Ia64::Firmware
{
namespace
{
using CaptureResult = Foundation::Result<SalSystemTableInformation, SalSystemTableCaptureError>;

[[nodiscard]] CaptureResult Fail(const SalSystemTableCaptureErrorKind kind,
                                 const SalSystemTableLocateError locationError = SalSystemTableLocateError::None,
                                 const SalSystemTableParseError parseError = SalSystemTableParseError::None) noexcept
{
    return CaptureResult::Failure(SalSystemTableCaptureError{
        .Kind = kind,
        .LocationError = locationError,
        .ParseError = parseError,
    });
}

[[nodiscard]] bool ScrubSnapshot(Foundation::UInt8* const snapshotBuffer,
                                 const Foundation::Size snapshotCapacity) noexcept
{
    Foundation::Size scrubBytes = snapshotCapacity;
    if (scrubBytes > MaximumSalSystemTableSnapshotBytes)
    {
        scrubBytes = MaximumSalSystemTableSnapshotBytes;
    }
    static_cast<void>(memset(snapshotBuffer, 0, scrubBytes));
    const volatile Foundation::UInt8* const verifiedBytes = snapshotBuffer;
    for (Foundation::Size index{0U}; index < scrubBytes; ++index)
    {
        if (verifiedBytes[index] != 0U)
        {
            return false;
        }
    }
    return true;
}
} // namespace

CaptureResult SalSystemTableCapture::Capture(const Boot::Uefi::ConfigurationTable* const configurationTables,
                                             const Foundation::Size configurationTableCount,
                                             const void* const memoryMap, const Foundation::Size memoryMapSize,
                                             const Foundation::Size memoryDescriptorSize,
                                             const Foundation::UInt32 memoryDescriptorVersion,
                                             Foundation::UInt8* const snapshotBuffer,
                                             const Foundation::Size snapshotCapacity) noexcept
{
    if (snapshotBuffer == nullptr)
    {
        return Fail(SalSystemTableCaptureErrorKind::InvalidSnapshotBuffer);
    }
    if (snapshotCapacity < SalSystemTableHeaderSize)
    {
        return Fail(SalSystemTableCaptureErrorKind::SnapshotBufferTooSmall);
    }

    const auto locationResult =
        SalSystemTableLocator::Locate(configurationTables, configurationTableCount, memoryMap, memoryMapSize,
                                      memoryDescriptorSize, memoryDescriptorVersion);
    if (!locationResult.HasValue())
    {
        return Fail(SalSystemTableCaptureErrorKind::LocationFailed, locationResult.GetError());
    }

    const SalSystemTableLocation location = locationResult.GetValue();
    Foundation::Size copyBytes = location.AvailableBytes;
    if (copyBytes > snapshotCapacity)
    {
        copyBytes = snapshotCapacity;
    }
    if (copyBytes > MaximumSalSystemTableSnapshotBytes)
    {
        copyBytes = MaximumSalSystemTableSnapshotBytes;
    }
    static_cast<void>(memmove(snapshotBuffer, location.Bytes, copyBytes));

    const auto parseResult = SalSystemTableParser::Parse(snapshotBuffer, copyBytes);
    if (!parseResult.HasValue())
    {
        return Fail(SalSystemTableCaptureErrorKind::ParseFailed, SalSystemTableLocateError::None,
                    parseResult.GetError());
    }

    return CaptureResult::Success(parseResult.GetValue());
}

CaptureResult SalSystemTableCapture::CaptureFromBootServices(
    Boot::Uefi::BootServices& bootServices, const Boot::Uefi::ConfigurationTable* const configurationTables,
    const Foundation::Size configurationTableCount, void* const memoryMapBuffer,
    const Foundation::Size memoryMapCapacity, Foundation::UInt8* const snapshotBuffer,
    const Foundation::Size snapshotCapacity) noexcept
{
    if (snapshotBuffer == nullptr)
    {
        return Fail(SalSystemTableCaptureErrorKind::InvalidSnapshotBuffer);
    }
    if (snapshotCapacity < SalSystemTableHeaderSize)
    {
        if (!ScrubSnapshot(snapshotBuffer, snapshotCapacity))
        {
            return Fail(SalSystemTableCaptureErrorKind::SnapshotScrubFailed);
        }
        return Fail(SalSystemTableCaptureErrorKind::SnapshotBufferTooSmall);
    }

    if ((memoryMapBuffer == nullptr) || (memoryMapCapacity == 0U))
    {
        if (!ScrubSnapshot(snapshotBuffer, snapshotCapacity))
        {
            return Fail(SalSystemTableCaptureErrorKind::SnapshotScrubFailed);
        }
        return Fail(SalSystemTableCaptureErrorKind::InvalidMemoryMapBuffer);
    }
    if (bootServices.GetMemoryMapFunction == nullptr)
    {
        if (!ScrubSnapshot(snapshotBuffer, snapshotCapacity))
        {
            return Fail(SalSystemTableCaptureErrorKind::SnapshotScrubFailed);
        }
        return Fail(SalSystemTableCaptureErrorKind::MissingGetMemoryMap);
    }

    Foundation::Size memoryMapSize{memoryMapCapacity};
    Foundation::Size memoryMapKey{0U};
    Foundation::Size memoryDescriptorSize{0U};
    Foundation::UInt32 memoryDescriptorVersion{0U};
    const Boot::Uefi::Status memoryMapStatus = bootServices.GetMemoryMapFunction(
        &memoryMapSize, memoryMapBuffer, &memoryMapKey, &memoryDescriptorSize, &memoryDescriptorVersion);

    CaptureResult result = Fail(SalSystemTableCaptureErrorKind::MemoryMapFailed);
    if (memoryMapStatus == Boot::Uefi::Success)
    {
        if (memoryMapSize > memoryMapCapacity)
        {
            result = Fail(SalSystemTableCaptureErrorKind::MemoryMapTooLarge);
        }
        else
        {
            result = Capture(configurationTables, configurationTableCount, memoryMapBuffer, memoryMapSize,
                             memoryDescriptorSize, memoryDescriptorVersion, snapshotBuffer, snapshotCapacity);
        }
    }

    if (!ScrubSnapshot(snapshotBuffer, snapshotCapacity))
    {
        return Fail(SalSystemTableCaptureErrorKind::SnapshotScrubFailed);
    }
    return result;
}
} // namespace Asteria::Platform::Ia64::Firmware
