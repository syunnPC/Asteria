// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Firmware/PalBootstrapCapabilityCapture.hpp"

namespace Asteria::Platform::Ia64::Firmware
{
namespace
{
using CaptureError = PalBootstrapCapabilityCaptureError;
using CapturePhase = PalBootstrapCapabilityCapturePhase;
using VirtualMemoryResult = Foundation::Result<Memory::PalVirtualMemoryCapabilities, CaptureError>;
using TranslationCachePurgeResult = Foundation::Result<Memory::PalTranslationCachePurgePlan, CaptureError>;

[[nodiscard]] constexpr CaptureError AdapterFailure(const CapturePhase phase,
                                                    const PalStaticCallAdapterError error) noexcept
{
    return CaptureError{
        .Phase = phase,
        .AdapterError = error,
        .VirtualMemoryError = Memory::PalVirtualMemoryDecodeError::None,
        .TranslationCachePurgeError = Memory::PalTranslationCachePurgeDecodeError::None,
    };
}

[[nodiscard]] constexpr CaptureError VirtualMemoryFailure(const Memory::PalVirtualMemoryDecodeError error) noexcept
{
    return CaptureError{
        .Phase = CapturePhase::VirtualMemoryDecoding,
        .AdapterError = PalStaticCallAdapterError::None,
        .VirtualMemoryError = error,
        .TranslationCachePurgeError = Memory::PalTranslationCachePurgeDecodeError::None,
    };
}

[[nodiscard]] constexpr bool IsSupportedRseInformation(const Memory::PalStaticCallResult& information) noexcept
{
    return (information.Status == 0) && (information.Result1 == Ia64PhysicalStackedRegisterCount) &&
           ((information.Result2 & ~PalRseInformationHintMask) == 0U) && (information.Result3 == 0U);
}

[[nodiscard]] VirtualMemoryResult CaptureVirtualMemory(const SalEntrypointBinding& binding) noexcept
{
    const auto summary = PalStaticCallAdapter::QueryVirtualMemorySummary(binding);
    if (!summary.HasValue())
    {
        return VirtualMemoryResult::Failure(
            AdapterFailure(CapturePhase::VirtualMemorySummaryInvocation, summary.GetError()));
    }
    if (summary.GetValue().Status != 0)
    {
        return VirtualMemoryResult::Failure(
            VirtualMemoryFailure(Memory::PalVirtualMemoryDecodeError::SummaryCallFailed));
    }

    const auto pageSizes = PalStaticCallAdapter::QueryVirtualMemoryPageSizes(binding);
    if (!pageSizes.HasValue())
    {
        return VirtualMemoryResult::Failure(
            AdapterFailure(CapturePhase::VirtualMemoryPageSizeInvocation, pageSizes.GetError()));
    }
    if (pageSizes.GetValue().Status != 0)
    {
        return VirtualMemoryResult::Failure(
            VirtualMemoryFailure(Memory::PalVirtualMemoryDecodeError::PageSizeCallFailed));
    }

    const auto memoryAttributes = PalStaticCallAdapter::QueryMemoryAttributes(binding);
    if (!memoryAttributes.HasValue())
    {
        return VirtualMemoryResult::Failure(
            AdapterFailure(CapturePhase::MemoryAttributeInvocation, memoryAttributes.GetError()));
    }
    if (memoryAttributes.GetValue().Status != 0)
    {
        return VirtualMemoryResult::Failure(
            VirtualMemoryFailure(Memory::PalVirtualMemoryDecodeError::MemoryAttributeCallFailed));
    }

    const auto decoded =
        Memory::PalVirtualMemoryDecoder::Decode(summary.GetValue(), pageSizes.GetValue(), memoryAttributes.GetValue());
    if (!decoded.HasValue())
    {
        return VirtualMemoryResult::Failure(VirtualMemoryFailure(decoded.GetError()));
    }
    return VirtualMemoryResult::Success(decoded.GetValue());
}

[[nodiscard]] TranslationCachePurgeResult CaptureTranslationCachePurge(const SalEntrypointBinding& binding) noexcept
{
    const auto information = PalStaticCallAdapter::QueryTranslationCachePurgeInformation(binding);
    if (!information.HasValue())
    {
        return TranslationCachePurgeResult::Failure(
            AdapterFailure(CapturePhase::TranslationCachePurgeInvocation, information.GetError()));
    }

    const auto decoded = Memory::PalTranslationCachePurgeDecoder::Decode(information.GetValue());
    if (!decoded.HasValue())
    {
        return TranslationCachePurgeResult::Failure(CaptureError{
            .Phase = CapturePhase::TranslationCachePurgeDecoding,
            .AdapterError = PalStaticCallAdapterError::None,
            .VirtualMemoryError = Memory::PalVirtualMemoryDecodeError::None,
            .TranslationCachePurgeError = decoded.GetError(),
        });
    }
    return TranslationCachePurgeResult::Success(decoded.GetValue());
}
} // namespace

Foundation::Result<PalBootstrapCapabilities, PalBootstrapCapabilityCaptureError>
PalBootstrapCapabilityCapture::Capture(const SalEntrypointBinding& binding) noexcept
{
    const VirtualMemoryResult virtualMemory = CaptureVirtualMemory(binding);
    if (!virtualMemory.HasValue())
    {
        return Foundation::Result<PalBootstrapCapabilities, PalBootstrapCapabilityCaptureError>::Failure(
            virtualMemory.GetError());
    }

    const TranslationCachePurgeResult translationCachePurge = CaptureTranslationCachePurge(binding);
    if (!translationCachePurge.HasValue())
    {
        return Foundation::Result<PalBootstrapCapabilities, PalBootstrapCapabilityCaptureError>::Failure(
            translationCachePurge.GetError());
    }

    const auto rseInformation = PalStaticCallAdapter::QueryRseInformation(binding);
    if (!rseInformation.HasValue())
    {
        return Foundation::Result<PalBootstrapCapabilities, PalBootstrapCapabilityCaptureError>::Failure(
            AdapterFailure(CapturePhase::RseInformationInvocation, rseInformation.GetError()));
    }
    if (!IsSupportedRseInformation(rseInformation.GetValue()))
    {
        return Foundation::Result<PalBootstrapCapabilities, PalBootstrapCapabilityCaptureError>::Failure(CaptureError{
            .Phase = CapturePhase::RseInformationDecoding,
            .AdapterError = PalStaticCallAdapterError::None,
            .VirtualMemoryError = Memory::PalVirtualMemoryDecodeError::None,
            .TranslationCachePurgeError = Memory::PalTranslationCachePurgeDecodeError::None,
        });
    }

    return Foundation::Result<PalBootstrapCapabilities, PalBootstrapCapabilityCaptureError>::Success(
        PalBootstrapCapabilities{
            .VirtualMemory = virtualMemory.GetValue(),
            .TranslationCachePurge = translationCachePurge.GetValue(),
        });
}
} // namespace Asteria::Platform::Ia64::Firmware
