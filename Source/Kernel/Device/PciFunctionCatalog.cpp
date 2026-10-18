// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Device/PciFunctionCatalog.hpp"

namespace Asteria::Kernel::Device
{
namespace
{
constexpr Foundation::UInt16 IdentifierRegisterOffset{0x00U};
constexpr Foundation::UInt16 ClassRegisterOffset{0x08U};
constexpr Foundation::UInt16 HeaderRegisterOffset{0x0CU};
constexpr Foundation::UInt16 MissingVendorId{0xFFFFU};
constexpr Foundation::UInt8 MultifunctionMask{0x80U};
constexpr Foundation::UInt8 HeaderLayoutMask{0x7FU};
} // namespace

PciFunctionCatalogFailure
PciFunctionCatalog::MakeFailure(const PciFunctionCatalogError error, const PciFunctionAddress address,
                                const Foundation::UInt16 registerOffset,
                                const Hal::Device::PciConfigurationReadError configuration) noexcept
{
    return PciFunctionCatalogFailure{
        .Address = address,
        .RegisterOffset = registerOffset,
        .Configuration = configuration,
        .Error = error,
        .Reserved = {},
    };
}

bool PciFunctionCatalog::RangeReservedIsZero(const PciBusRange& range) noexcept
{
    for (const Foundation::UInt8 value : range.Reserved)
    {
        if (value != 0U)
        {
            return false;
        }
    }
    return true;
}

bool PciFunctionCatalog::RangesOverlap(const PciBusRange& left, const PciBusRange& right) noexcept
{
    return (left.Segment == right.Segment) && (left.FirstBus <= right.LastBus) && (right.FirstBus <= left.LastBus);
}

bool PciFunctionCatalog::RangePrecedes(const PciBusRange& left, const PciBusRange& right) noexcept
{
    return (left.Segment < right.Segment) ||
           ((left.Segment == right.Segment) && ((left.FirstBus < right.FirstBus) ||
                                                ((left.FirstBus == right.FirstBus) && (left.LastBus < right.LastBus))));
}

PciFunctionCatalogFailure PciFunctionCatalog::ValidateAndSortRanges(const PciBusRangeView input,
                                                                    PciBusRange (&sorted)[MaximumRanges],
                                                                    Foundation::Size& busCount) noexcept
{
    if ((input.Data == nullptr) || (input.Count == 0U) || (input.Count > MaximumRanges))
    {
        return MakeFailure(PciFunctionCatalogError::InvalidRangeView);
    }

    Foundation::Size totalBuses{0U};
    for (Foundation::Size index{0U}; index < input.Count; ++index)
    {
        const PciBusRange& range = input.Data[index];
        if ((range.FirstBus > range.LastBus) || !RangeReservedIsZero(range))
        {
            return MakeFailure(PciFunctionCatalogError::InvalidBusRange);
        }
        const Foundation::Size rangeBusCount =
            static_cast<Foundation::Size>(range.LastBus) - static_cast<Foundation::Size>(range.FirstBus) + 1U;
        totalBuses += rangeBusCount;
        sorted[index] = range;
    }

    for (Foundation::Size index{1U}; index < input.Count; ++index)
    {
        const PciBusRange value = sorted[index];
        Foundation::Size destination{index};
        while ((destination != 0U) && RangePrecedes(value, sorted[destination - 1U]))
        {
            sorted[destination] = sorted[destination - 1U];
            --destination;
        }
        sorted[destination] = value;
    }

    for (Foundation::Size index{1U}; index < input.Count; ++index)
    {
        if (RangesOverlap(sorted[index - 1U], sorted[index]))
        {
            return MakeFailure(PciFunctionCatalogError::OverlappingBusRange);
        }
    }

    busCount = totalBuses;
    return MakeFailure(PciFunctionCatalogError::None);
}

PciFunctionCatalogFailure PciFunctionCatalog::Read32Locked(const PciFunctionAddress address,
                                                           const Foundation::UInt16 registerOffset,
                                                           Foundation::UInt32& output) noexcept
{
    Foundation::UInt32 value{0U};
    ++m_configurationReadCount;
    const Hal::Device::PciConfigurationReadError readError = m_configuration.Read32(address, registerOffset, value);
    if (readError != Hal::Device::PciConfigurationReadError::None)
    {
        return MakeFailure(PciFunctionCatalogError::ConfigurationReadFailed, address, registerOffset, readError);
    }
    output = value;
    return MakeFailure(PciFunctionCatalogError::None);
}

PciFunctionCatalogFailure PciFunctionCatalog::AppendFunctionLocked(const PciFunctionAddress address,
                                                                   const Foundation::UInt32 identifier,
                                                                   bool& multifunction) noexcept
{
    if (m_functionCount >= MaximumFunctions)
    {
        return MakeFailure(PciFunctionCatalogError::FunctionCapacityExceeded, address, IdentifierRegisterOffset);
    }

    Foundation::UInt32 classRegister{0U};
    PciFunctionCatalogFailure failure = Read32Locked(address, ClassRegisterOffset, classRegister);
    if (failure.Error != PciFunctionCatalogError::None)
    {
        return failure;
    }

    Foundation::UInt32 headerRegister{0U};
    failure = Read32Locked(address, HeaderRegisterOffset, headerRegister);
    if (failure.Error != PciFunctionCatalogError::None)
    {
        return failure;
    }

    const Foundation::UInt8 headerType = static_cast<Foundation::UInt8>((headerRegister >> 16U) & 0xFFU);
    multifunction = (headerType & MultifunctionMask) != 0U;
    m_functions[m_functionCount] = PciFunctionDescriptor{
        .Address = address,
        .VendorId = static_cast<Foundation::UInt16>(identifier & 0xFFFFU),
        .DeviceId = static_cast<Foundation::UInt16>(identifier >> 16U),
        .RevisionId = static_cast<Foundation::UInt8>(classRegister & 0xFFU),
        .ProgrammingInterface = static_cast<Foundation::UInt8>((classRegister >> 8U) & 0xFFU),
        .Subclass = static_cast<Foundation::UInt8>((classRegister >> 16U) & 0xFFU),
        .ClassCode = static_cast<Foundation::UInt8>((classRegister >> 24U) & 0xFFU),
        .HeaderLayout = static_cast<Foundation::UInt8>(headerType & HeaderLayoutMask),
        .Multifunction = multifunction,
    };
    ++m_functionCount;
    return MakeFailure(PciFunctionCatalogError::None);
}

PciFunctionCatalogFailure PciFunctionCatalog::Discover(const PciBusRangeView ranges) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == PciFunctionCatalogState::Ready)
    {
        return MakeFailure(PciFunctionCatalogError::AlreadyDiscovered);
    }

    PciBusRange sortedRanges[MaximumRanges]{};
    Foundation::Size busCount{0U};
    PciFunctionCatalogFailure failure = ValidateAndSortRanges(ranges, sortedRanges, busCount);
    if (failure.Error != PciFunctionCatalogError::None)
    {
        ResetStorageLocked();
        return failure;
    }

    ResetStorageLocked();
    for (Foundation::Size rangeIndex{0U}; rangeIndex < ranges.Count; ++rangeIndex)
    {
        const PciBusRange& range = sortedRanges[rangeIndex];
        const Foundation::UInt16 lastBus = range.LastBus;
        for (Foundation::UInt16 bus = range.FirstBus; bus <= lastBus; ++bus)
        {
            for (Foundation::UInt8 device{0U}; device <= PciFunctionAddress::MaximumDevice; ++device)
            {
                const PciFunctionAddress functionZero{range.Segment, static_cast<Foundation::UInt8>(bus), device, 0U};
                Foundation::UInt32 identifier{0U};
                failure = Read32Locked(functionZero, IdentifierRegisterOffset, identifier);
                if (failure.Error != PciFunctionCatalogError::None)
                {
                    ResetStorageLocked();
                    return failure;
                }
                if (static_cast<Foundation::UInt16>(identifier & 0xFFFFU) == MissingVendorId)
                {
                    continue;
                }

                bool multifunction{false};
                failure = AppendFunctionLocked(functionZero, identifier, multifunction);
                if (failure.Error != PciFunctionCatalogError::None)
                {
                    ResetStorageLocked();
                    return failure;
                }
                if (!multifunction)
                {
                    continue;
                }

                for (Foundation::UInt8 function{1U}; function <= PciFunctionAddress::MaximumFunction; ++function)
                {
                    const PciFunctionAddress address{range.Segment, static_cast<Foundation::UInt8>(bus), device,
                                                     function};
                    identifier = 0U;
                    failure = Read32Locked(address, IdentifierRegisterOffset, identifier);
                    if (failure.Error != PciFunctionCatalogError::None)
                    {
                        ResetStorageLocked();
                        return failure;
                    }
                    if (static_cast<Foundation::UInt16>(identifier & 0xFFFFU) == MissingVendorId)
                    {
                        continue;
                    }
                    bool ignoredMultifunction{false};
                    failure = AppendFunctionLocked(address, identifier, ignoredMultifunction);
                    if (failure.Error != PciFunctionCatalogError::None)
                    {
                        ResetStorageLocked();
                        return failure;
                    }
                }
            }
        }
    }

    m_busCount = busCount;
    m_rangeCount = ranges.Count;
    m_state = PciFunctionCatalogState::Ready;
    return MakeFailure(PciFunctionCatalogError::None);
}

Foundation::Result<PciFunctionDescriptor, PciFunctionCatalogFailure>
PciFunctionCatalog::GetFunction(const Foundation::Size index) const noexcept
{
    using ResultType = Foundation::Result<PciFunctionDescriptor, PciFunctionCatalogFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state != PciFunctionCatalogState::Ready)
    {
        return ResultType::Failure(MakeFailure(PciFunctionCatalogError::NotReady));
    }
    if (index >= m_functionCount)
    {
        return ResultType::Failure(MakeFailure(PciFunctionCatalogError::IndexOutOfRange));
    }
    return ResultType::Success(m_functions[index]);
}

Foundation::Result<PciFunctionDescriptor, PciFunctionCatalogFailure>
PciFunctionCatalog::FindFunction(const PciFunctionAddress address) const noexcept
{
    using ResultType = Foundation::Result<PciFunctionDescriptor, PciFunctionCatalogFailure>;

    if (!address.IsValid())
    {
        return ResultType::Failure(MakeFailure(PciFunctionCatalogError::InvalidAddress, address));
    }

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state != PciFunctionCatalogState::Ready)
    {
        return ResultType::Failure(MakeFailure(PciFunctionCatalogError::NotReady, address));
    }
    for (Foundation::Size index{0U}; index < m_functionCount; ++index)
    {
        if (m_functions[index].Address.IsEqualTo(address))
        {
            return ResultType::Success(m_functions[index]);
        }
    }
    return ResultType::Failure(MakeFailure(PciFunctionCatalogError::FunctionNotFound, address));
}

PciFunctionCatalogSnapshot PciFunctionCatalog::GetSnapshot() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return PciFunctionCatalogSnapshot{
        .FunctionCount = m_functionCount,
        .BusCount = m_busCount,
        .RangeCount = m_rangeCount,
        .ConfigurationReadCount = m_configurationReadCount,
        .State = m_state,
        .Reserved = {},
    };
}

void PciFunctionCatalog::ResetStorageLocked() noexcept
{
    for (PciFunctionDescriptor& function : m_functions)
    {
        function = PciFunctionDescriptor{};
    }
    m_functionCount = 0U;
    m_busCount = 0U;
    m_rangeCount = 0U;
    m_configurationReadCount = 0U;
    m_state = PciFunctionCatalogState::Empty;
}
} // namespace Asteria::Kernel::Device
