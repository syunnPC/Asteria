// SPDX-License-Identifier: Apache-2.0

#include "Boot/Uefi/BootEntropyGathering.hpp"

#include "Boot/Uefi/TableValidation.hpp"
#include "Platform/Firmware/BootEntropySources.hpp"

namespace Asteria::Boot::Uefi
{
namespace
{
using Entropy::BootEntropyCollector;
using Foundation::Size;
using Foundation::UInt64;
using Foundation::UInt8;

constexpr Guid RngProtocolGuid{
    .Data1 = 0x3152'BCA5U,
    .Data2 = 0xEADEU,
    .Data3 = 0x433DU,
    .Data4 = {0x86U, 0x2EU, 0xC0U, 0x1CU, 0xDCU, 0x29U, 0x1FU, 0x44U},
};
constexpr Size GetTimeEndBytes{__builtin_offsetof(RuntimeServices, GetTimeFunction) + sizeof(GetTime)};

void StoreLittleEndian64(UInt8* const bytes, const UInt64 value) noexcept
{
    for (Size index{0U}; index < 8U; ++index)
    {
        bytes[index] = static_cast<UInt8>(value >> (index * 8U));
    }
}

[[gnu::noinline]] void MixFirmwareRng(BootServices& bootServices, BootEntropyCollector& collector) noexcept
{
    if (bootServices.LocateProtocolFunction == nullptr)
    {
        return;
    }
    Guid protocolGuid = RngProtocolGuid;
    void* interface{nullptr};
    if ((bootServices.LocateProtocolFunction(&protocolGuid, nullptr, &interface) != Success) || (interface == nullptr))
    {
        return;
    }
    auto* const rng = static_cast<RngProtocol*>(interface);
    if (rng->GetRng == nullptr)
    {
        return;
    }
    for (Size block{0U}; block < FirmwareRngBlockCount; ++block)
    {
        UInt8 bytes[FirmwareRngBlockBytes]{};
        if (rng->GetRng(rng, nullptr, sizeof(bytes), bytes) == Success)
        {
            collector.AddGeneratorBlock(BootEntropySource::FirmwareRng, bytes, sizeof(bytes));
        }
    }
}

[[gnu::noinline]] void MixProcessorRandom(BootEntropyCollector& collector) noexcept
{
    for (Size value{0U}; value < ProcessorRandomValueCount; ++value)
    {
        UInt64 random{0U};
        if (!Platform::Firmware::TryReadProcessorRandom(random))
        {
            return;
        }
        UInt8 bytes[8U]{};
        StoreLittleEndian64(bytes, random);
        collector.AddGeneratorBlock(BootEntropySource::ProcessorRng, bytes, sizeof(bytes));
    }
}

[[gnu::noinline]] void MixFirmwareState(SystemTable& systemTable, BootEntropyCollector& collector) noexcept
{
    const auto* const runtimeServices = static_cast<const RuntimeServices*>(systemTable.RuntimeServices);
    if ((runtimeServices != nullptr) &&
        IsTableHeaderValid(runtimeServices->Header, RuntimeServicesSignature, GetTimeEndBytes) &&
        (runtimeServices->GetTimeFunction != nullptr))
    {
        Time time{};
        if (runtimeServices->GetTimeFunction(&time, nullptr) == Success)
        {
            UInt8 timeBytes[sizeof(Time)]{};
            StoreLittleEndian64(
                timeBytes, static_cast<UInt64>(time.Year) | (static_cast<UInt64>(time.Month) << 16U) |
                               (static_cast<UInt64>(time.Day) << 24U) | (static_cast<UInt64>(time.Hour) << 32U) |
                               (static_cast<UInt64>(time.Minute) << 40U) | (static_cast<UInt64>(time.Second) << 48U));
            StoreLittleEndian64(timeBytes + 8U, static_cast<UInt64>(time.Nanosecond));
            collector.AddFirmwareState(timeBytes, sizeof(timeBytes));
        }
    }

    BootServices* const bootServices = systemTable.BootServicesTable;
    UInt64 count{0U};
    if ((bootServices != nullptr) && (bootServices->GetNextMonotonicCountFunction != nullptr) &&
        (bootServices->GetNextMonotonicCountFunction(&count) == Success))
    {
        UInt8 bytes[8U]{};
        StoreLittleEndian64(bytes, count);
        collector.AddFirmwareState(bytes, sizeof(bytes));
    }
}

[[gnu::noinline]] void MixCycleCounterJitter(BootEntropyCollector& collector) noexcept
{
    // Each reading follows the pool update of the previous one, so the
    // deltas time that work.
    for (Size reading{0U}; reading < CycleCounterReadingCount; ++reading)
    {
        collector.AddCycleCounterSample(Platform::Firmware::ReadCycleCounter());
    }
}
} // namespace

Foundation::Result<BootEntropy, Entropy::BootEntropyError> GatherBootEntropy(SystemTable& systemTable) noexcept
{
    BootEntropyCollector collector{};
    if (systemTable.BootServicesTable != nullptr)
    {
        MixFirmwareRng(*systemTable.BootServicesTable, collector);
    }
    MixProcessorRandom(collector);
    MixFirmwareState(systemTable, collector);
    MixCycleCounterJitter(collector);
    return collector.Finish();
}
} // namespace Asteria::Boot::Uefi
