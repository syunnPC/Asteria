// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Device/AhciPciBootstrapBinding.hpp"

namespace Asteria::Kernel::Device
{
namespace
{
constexpr Foundation::UInt8 MassStorageClass{0x01U};
constexpr Foundation::UInt8 SerialAtaSubclass{0x06U};
constexpr Foundation::UInt8 AhciProgrammingInterface{0x01U};
constexpr Foundation::UInt8 EndpointHeaderLayout{0x00U};
constexpr Foundation::UInt8 AbarIndex{5U};
} // namespace

AhciPciBootstrapBindingFailure AhciPciBootstrapBinding::MakeFailure(const AhciPciBootstrapBindingError error,
                                                                    const Foundation::Size matchingFunctionCount,
                                                                    const PciFunctionCatalogFailure catalog,
                                                                    const PciEndpointBarDecodeFailure abar,
                                                                    const AhciHbaSnapshotFailure snapshot) noexcept
{
    return AhciPciBootstrapBindingFailure{
        .Catalog = catalog,
        .Abar = abar,
        .Snapshot = snapshot,
        .MatchingFunctionCount = matchingFunctionCount,
        .Error = error,
        .Reserved = {},
    };
}

bool AhciPciBootstrapBinding::IsAhciEndpoint(const PciFunctionDescriptor& descriptor) noexcept
{
    return (descriptor.ClassCode == MassStorageClass) && (descriptor.Subclass == SerialAtaSubclass) &&
           (descriptor.ProgrammingInterface == AhciProgrammingInterface) &&
           (descriptor.HeaderLayout == EndpointHeaderLayout);
}

AhciPciBootstrapBindingError
AhciPciBootstrapBinding::SelectAhciEndpoint(PciFunctionDescriptor& output, Foundation::Size& matchingFunctionCount,
                                            PciFunctionCatalogFailure& catalogFailure) const noexcept
{
    const PciFunctionCatalogSnapshot catalogSnapshot = m_catalog.GetSnapshot();
    if (catalogSnapshot.State != PciFunctionCatalogState::Ready)
    {
        catalogFailure.Error = PciFunctionCatalogError::NotReady;
        return AhciPciBootstrapBindingError::CatalogNotReady;
    }

    PciFunctionDescriptor selected{};
    Foundation::Size matchCount{0U};
    for (Foundation::Size index{0U}; index < catalogSnapshot.FunctionCount; ++index)
    {
        const auto function = m_catalog.GetFunction(index);
        if (!function.HasValue())
        {
            catalogFailure = function.GetError();
            matchingFunctionCount = matchCount;
            return AhciPciBootstrapBindingError::CatalogQueryRejected;
        }
        if (IsAhciEndpoint(function.GetValue()))
        {
            selected = function.GetValue();
            ++matchCount;
        }
    }

    matchingFunctionCount = matchCount;
    if (matchCount == 0U)
    {
        return AhciPciBootstrapBindingError::NoAhciController;
    }
    if (matchCount != 1U)
    {
        return AhciPciBootstrapBindingError::AmbiguousAhciController;
    }

    output = selected;
    return AhciPciBootstrapBindingError::None;
}

AhciPciBootstrapBindingFailure AhciPciBootstrapBinding::Prepare(const AhciPciBootstrapBindingRequest request) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == AhciPciBootstrapBindingState::Ready)
    {
        return MakeFailure(AhciPciBootstrapBindingError::AlreadyPrepared, 1U);
    }

    AhciPciBootstrapBindingResult candidate{};
    Foundation::Size matchingFunctionCount{0U};
    PciFunctionCatalogFailure catalogFailure{};
    const AhciPciBootstrapBindingError selectionError =
        SelectAhciEndpoint(candidate.Function, matchingFunctionCount, catalogFailure);
    if (selectionError != AhciPciBootstrapBindingError::None)
    {
        return MakeFailure(selectionError, matchingFunctionCount, catalogFailure);
    }

    const PciEndpointBarDecodeFailure abarFailure = m_barDecoder.Decode(
        PciEndpointBarDecodeRequest{
            .MaximumPhysicalAddress = request.MaximumPhysicalAddress,
            .ExtentBytes = request.AbarExtentBytes,
            .Address = candidate.Function.Address,
            .ExtentState = PciEndpointBarExtentState::Authoritative,
            .BarIndex = AbarIndex,
        },
        candidate.Abar);
    if (abarFailure.Error != PciEndpointBarDecodeError::None)
    {
        return MakeFailure(AhciPciBootstrapBindingError::AbarDecodeRejected, matchingFunctionCount, {}, abarFailure);
    }
    if ((candidate.Abar.Kind != PciEndpointBarKind::Memory32) || (candidate.Abar.RegisterCount != 1U))
    {
        return MakeFailure(AhciPciBootstrapBindingError::AbarKindRejected, matchingFunctionCount);
    }
    if (candidate.Abar.Prefetchable)
    {
        return MakeFailure(AhciPciBootstrapBindingError::AbarPrefetchable, matchingFunctionCount);
    }

    const AhciHbaSnapshotFailure snapshotFailure = m_snapshotReader.Read(
        AhciHbaRegisterRange{
            .BaseAddress = candidate.Abar.BaseAddress,
            .LengthBytes = candidate.Abar.ExtentBytes,
        },
        candidate.Snapshot);
    if (snapshotFailure.Error != AhciHbaSnapshotError::None)
    {
        return MakeFailure(AhciPciBootstrapBindingError::SnapshotReadRejected, matchingFunctionCount, {}, {},
                           snapshotFailure);
    }

    m_result = candidate;
    m_state = AhciPciBootstrapBindingState::Ready;
    return MakeFailure(AhciPciBootstrapBindingError::None, matchingFunctionCount);
}

AhciPciBootstrapBindingQueryError
AhciPciBootstrapBinding::GetResult(AhciPciBootstrapBindingResult& output) const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state != AhciPciBootstrapBindingState::Ready)
    {
        return AhciPciBootstrapBindingQueryError::NotReady;
    }
    output = m_result;
    return AhciPciBootstrapBindingQueryError::None;
}

AhciPciBootstrapBindingState AhciPciBootstrapBinding::GetState() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return m_state;
}
} // namespace Asteria::Kernel::Device
