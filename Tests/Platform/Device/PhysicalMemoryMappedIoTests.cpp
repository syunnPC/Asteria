// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "Kernel/Object/ObjectRegistry.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Kernel/Resource/ResourceDomain.hpp"
#include "Platform/Amd64/Device/Amd64PhysicalMemoryAccess.hpp"
#include "Platform/Device/PhysicalMemoryMappedIo.hpp"
#include "Platform/Ia64/Device/Ia64PhysicalMemoryAccess.hpp"

#include "Support/ObjectRegistryFixture.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Foundation::UInt32;
using Foundation::UInt64;
using Hal::Device::IMemoryMappedIo;
using Hal::Device::IMemoryMappedIoWriter;
using Hal::Device::MemoryMappedIoReadError;
using Hal::Device::MemoryMappedIoWidth;
using Hal::Device::MemoryMappedIoWriteError;
using Kernel::Device::DeviceAccessWidth;
using Kernel::Device::DeviceAccessWidths;
using Kernel::Device::DeviceRegion;
using Kernel::Device::DeviceRegionDescriptor;
using Kernel::Device::DeviceRegionKind;
using Kernel::Device::DeviceRegionPermission;
using Kernel::Device::DeviceRegionPermissions;
using Kernel::Object::ObjectRegistry;
using Kernel::Object::ResourceDomainId;
using Kernel::Object::RootCapabilityIssuer;
using Kernel::Resource::ResourceDomain;
using Kernel::Resource::ResourceDomainError;
using Kernel::Resource::ResourceDomainLedger;
using Kernel::Resource::ResourceDomainLimits;
using Kernel::Resource::ResourceDomainObjectId;
using Platform::Device::PhysicalMemoryMappedIo;
using Platform::Device::PhysicalMemoryMappedIoInitializationError;
using Platform::Device::PhysicalMemoryMappedIoRegionView;
using Platform::Device::PhysicalMemoryMappedIoState;
using Platform::Synchronization::SpinLock;

/// One ISA's physical access and the widest physical address it names; each
/// argv mode runs every scenario with one profile.
struct AccessProfile
{
    const Platform::Device::IPhysicalMemoryAccess& Access;
    Foundation::UInt8 PhysicalAddressBits;
    UInt64 LastImplementedDoubleWord;
};

class RegionFixture final
{
public:
    RegionFixture() noexcept : m_registry{m_synchronization}, m_issuer{m_registry}, m_domain{m_ledger}
    {
        const ResourceDomainLimits limits{
            .CommittedMemoryPages = 16U,
            .ResidentMemoryPages = 16U,
            .KernelMetadataBytes = 1024U * 1024U,
            .CapabilityHandles = 64U,
            .IpcQueueBytes = 1024U,
            .Threads = 16U,
            .ChildDomains = 16U,
        };
        m_ready = m_domain.InitializeRoot(ResourceDomainId{91U}, limits).Error == ResourceDomainError::None;
        const auto registered = m_issuer.CreateObject(m_domain);
        if (registered.HasValue())
        {
            m_domainIdentity = ResourceDomainObjectId{m_registry, registered.GetValue()};
        }
        else
        {
            m_ready = false;
        }
    }

    [[nodiscard]] bool InitializeRegion(DeviceRegion& region, const UInt64 baseAddress, const UInt64 lengthBytes,
                                        const DeviceRegionPermissions permissions,
                                        const DeviceAccessWidths widths) noexcept
    {
        return m_ready && (region
                               .Initialize(m_registry, m_domainIdentity, m_domain,
                                           DeviceRegionDescriptor{
                                               .BaseAddress = baseAddress,
                                               .LengthBytes = lengthBytes,
                                               .Kind = DeviceRegionKind::MemoryMappedIo,
                                               .Permissions = permissions,
                                               .Widths = widths,
                                               .CachePolicy = Kernel::Memory::MemoryCachePolicy::Uncached,
                                               .Reserved = {},
                                           })
                               .Error == Kernel::Device::DeviceRegionError::None);
    }

private:
    SpinLock m_synchronization;
    Asteria::TestSupport::TestObjectRegistry m_registry;
    RootCapabilityIssuer m_issuer;
    ResourceDomainLedger m_ledger;
    ResourceDomain m_domain;
    ResourceDomainObjectId m_domainIdentity{};
    bool m_ready{true};
};

[[nodiscard]] DeviceRegionPermissions ReadWritePermissions() noexcept
{
    return DeviceRegionPermissions{DeviceRegionPermission::Read}.With(DeviceRegionPermission::Write);
}

[[nodiscard]] DeviceAccessWidths PciWidths() noexcept
{
    return DeviceAccessWidths{DeviceAccessWidth::Byte}
        .With(DeviceAccessWidth::Word)
        .With(DeviceAccessWidth::DoubleWord);
}

void CheckRejectedWrite(TestContext& runner, IMemoryMappedIoWriter& writer, const UInt64 physicalAddress,
                        const MemoryMappedIoWidth width, const UInt64 value,
                        const MemoryMappedIoWriteError expectedError, const volatile UInt32* const registers,
                        const UInt32* const expectedRegisters, const Foundation::Size registerCount,
                        const char* const description) noexcept
{
    const MemoryMappedIoWriteError error = writer.Write(physicalAddress, width, value);
    bool unchanged{true};
    for (Foundation::Size index{0U}; index < registerCount; ++index)
    {
        unchanged = unchanged && (registers[index] == expectedRegisters[index]);
    }
    ASTERIA_EXPECT(runner, description, error == expectedError, unchanged);
}

void TestPhysicalRangeValidation(TestContext& runner, const AccessProfile& profile) noexcept
{
    const Platform::Device::IPhysicalMemoryAccess& access = profile.Access;
    const Foundation::UInt8 widest = profile.PhysicalAddressBits;
    ASTERIA_EXPECT(runner, "physical widths, overflow, and the top of the physical range are checked",
                   access.GetMaximumPhysicalAddressBits() == widest,
                   access.IsPhysicalRangeImplemented(0xFFFF'F000ULL, 0x1000U, 32U),
                   !access.IsPhysicalRangeImplemented(0xFFFF'F000ULL, 0x1001U, 32U),
                   !access.IsPhysicalRangeImplemented(0xFFFF'F000ULL, 0x1000U, 31U),
                   !access.IsPhysicalRangeImplemented(0x1000U, 4U, static_cast<Foundation::UInt8>(widest + 1U)),
                   !access.IsPhysicalRangeImplemented(0x8000'0000'0000'0000ULL, 4U, widest),
                   access.IsPhysicalRangeImplemented(profile.LastImplementedDoubleWord, 4U, widest),
                   !access.IsPhysicalRangeImplemented(profile.LastImplementedDoubleWord, 8U, widest));
}

void TestReadAndBounds(TestContext& runner, const AccessProfile& profile) noexcept
{
    alignas(8) volatile UInt32 registers[4U]{
        0x1234'8086U,
        0xAABB'CCDDU,
        0U,
        0U,
    };
    const UInt64 baseAddress = reinterpret_cast<UInt64>(&registers[0U]);
    RegionFixture fixture;
    DeviceRegion region;
    const bool regionReady =
        fixture.InitializeRegion(region, baseAddress, sizeof(registers), ReadWritePermissions(), PciWidths());
    DeviceRegion* regions[]{&region};
    PhysicalMemoryMappedIo memoryMappedIo{profile.Access};
    const auto initialized = memoryMappedIo.Initialize(
        PhysicalMemoryMappedIoRegionView{
            .Data = regions,
            .Count = 1U,
        },
        profile.PhysicalAddressBits);
    UInt64 output{0xDEAD'BEEF'DEAD'BEEFULL};
    const auto read = memoryMappedIo.Read(baseAddress + 4U, MemoryMappedIoWidth::DoubleWord, output);
    ASTERIA_EXPECT(runner, "a superset PCI policy authorizes one exact volatile DWORD read", regionReady,
                   initialized == PhysicalMemoryMappedIoInitializationError::None,
                   memoryMappedIo.GetSnapshot().State == PhysicalMemoryMappedIoState::Ready,
                   read == MemoryMappedIoReadError::None, output == 0xAABB'CCDDU);

    output = 0x5566'7788U;
    const auto width = memoryMappedIo.Read(baseAddress, MemoryMappedIoWidth::Word, output);
    const auto alignment = memoryMappedIo.Read(baseAddress + 2U, MemoryMappedIoWidth::DoubleWord, output);
    const auto lowerBound = memoryMappedIo.Read(baseAddress - 4U, MemoryMappedIoWidth::DoubleWord, output);
    const auto upperBound =
        memoryMappedIo.Read(baseAddress + sizeof(registers), MemoryMappedIoWidth::DoubleWord, output);
    ASTERIA_EXPECT(runner, "width, alignment, and region bounds fail without changing output",
                   width == MemoryMappedIoReadError::UnsupportedWidth,
                   alignment == MemoryMappedIoReadError::InvalidAddress,
                   lowerBound == MemoryMappedIoReadError::InvalidAddress,
                   upperBound == MemoryMappedIoReadError::InvalidAddress, output == 0x5566'7788U);
    ASTERIA_EXPECT(runner, "the successful direct region is quiesced before fixture destruction",
                   region.Quiesce().Error == Kernel::Device::DeviceRegionError::None);
}

void TestInitializationAndLifecycleFailures(TestContext& runner, const AccessProfile& profile) noexcept
{
    alignas(8) UInt32 registers[4U]{};
    const UInt64 baseAddress = reinterpret_cast<UInt64>(&registers[0U]);
    RegionFixture fixture;
    DeviceRegion region;
    const bool regionReady = fixture.InitializeRegion(region, baseAddress, sizeof(registers),
                                                      DeviceRegionPermissions{DeviceRegionPermission::Read},
                                                      DeviceAccessWidths{DeviceAccessWidth::DoubleWord});
    DeviceRegion* regions[]{&region};
    PhysicalMemoryMappedIo memoryMappedIo{profile.Access};
    const auto badWidth = memoryMappedIo.Initialize(
        PhysicalMemoryMappedIoRegionView{
            .Data = regions,
            .Count = 1U,
        },
        31U);
    const auto tooWide = memoryMappedIo.Initialize(
        PhysicalMemoryMappedIoRegionView{
            .Data = regions,
            .Count = 1U,
        },
        static_cast<Foundation::UInt8>(profile.PhysicalAddressBits + 1U));
    const auto retry = memoryMappedIo.Initialize(
        PhysicalMemoryMappedIoRegionView{
            .Data = regions,
            .Count = 1U,
        },
        profile.PhysicalAddressBits);
    const bool quiesced = region.Quiesce().Error == Kernel::Device::DeviceRegionError::None;
    UInt64 output{0x1020'3040U};
    const auto afterQuiesce = memoryMappedIo.Read(baseAddress, MemoryMappedIoWidth::DoubleWord, output);
    ASTERIA_EXPECT(runner, "initialization failure is atomic and lifecycle failure preserves output", regionReady,
                   badWidth == PhysicalMemoryMappedIoInitializationError::InvalidPhysicalAddressWidth,
                   tooWide == PhysicalMemoryMappedIoInitializationError::InvalidPhysicalAddressWidth,
                   retry == PhysicalMemoryMappedIoInitializationError::None, quiesced,
                   afterQuiesce == MemoryMappedIoReadError::NotReady, output == 0x1020'3040U);
}

void TestWriteAndCapabilitySeparation(TestContext& runner, const AccessProfile& profile) noexcept
{
    alignas(8) volatile UInt32 readOnlyRegisters[2U]{
        0xB1B2'B3B4U,
        0xC1C2'C3C4U,
    };
    alignas(8) volatile UInt32 registers[4U]{
        0x0102'0304U,
        0x1112'1314U,
        0x2122'2324U,
        0x3132'3334U,
    };
    const UInt64 readOnlyBaseAddress = reinterpret_cast<UInt64>(&readOnlyRegisters[0U]);
    const UInt64 baseAddress = reinterpret_cast<UInt64>(&registers[0U]);
    RegionFixture fixture;
    DeviceRegion readOnlyRegion;
    DeviceRegion region;
    const bool readOnlyRegionReady = fixture.InitializeRegion(
        readOnlyRegion, readOnlyBaseAddress, sizeof(readOnlyRegisters),
        DeviceRegionPermissions{DeviceRegionPermission::Read}, DeviceAccessWidths{DeviceAccessWidth::DoubleWord});
    const bool regionReady =
        fixture.InitializeRegion(region, baseAddress, sizeof(registers), ReadWritePermissions(), PciWidths());
    DeviceRegion* regions[]{&readOnlyRegion, &region};
    PhysicalMemoryMappedIo memoryMappedIo{profile.Access};
    const auto initialized = memoryMappedIo.Initialize(
        PhysicalMemoryMappedIoRegionView{
            .Data = regions,
            .Count = 2U,
        },
        profile.PhysicalAddressBits);
    IMemoryMappedIo& reader = memoryMappedIo;
    IMemoryMappedIoWriter& writer = memoryMappedIo;
    const MemoryMappedIoWriteError write =
        writer.Write(baseAddress + 4U, MemoryMappedIoWidth::DoubleWord, 0xA5C3'7E19U);
    UInt64 output{0U};
    const MemoryMappedIoReadError read = reader.Read(baseAddress + 4U, MemoryMappedIoWidth::DoubleWord, output);
    const auto regionSnapshot = region.GetSnapshot();
    ASTERIA_EXPECT(runner, "separate capabilities select and release the exact writable DWORD region",
                   readOnlyRegionReady, regionReady, initialized == PhysicalMemoryMappedIoInitializationError::None,
                   write == MemoryMappedIoWriteError::None, readOnlyRegisters[0U] == 0xB1B2'B3B4U,
                   readOnlyRegisters[1U] == 0xC1C2'C3C4U, registers[1U] == 0xA5C3'7E19U,
                   read == MemoryMappedIoReadError::None, output == 0xA5C3'7E19U,
                   regionSnapshot.ActiveRegisterAccesses == 0U);
    const bool readOnlyQuiesced = readOnlyRegion.Quiesce().Error == Kernel::Device::DeviceRegionError::None;
    const bool writableQuiesced = region.Quiesce().Error == Kernel::Device::DeviceRegionError::None;
    ASTERIA_EXPECT(runner, "the successful backend regions are quiesced before fixture destruction", readOnlyQuiesced,
                   writableQuiesced);
}

void TestRejectedWritesPreserveMemory(TestContext& runner, const AccessProfile& profile) noexcept
{
    alignas(8) volatile UInt32 registers[4U]{
        0x1020'3040U,
        0x5060'7080U,
        0x90A0'B0C0U,
        0xD0E0'F000U,
    };
    const UInt32 expectedRegisters[4U]{
        0x1020'3040U,
        0x5060'7080U,
        0x90A0'B0C0U,
        0xD0E0'F000U,
    };
    const UInt64 baseAddress = reinterpret_cast<UInt64>(&registers[0U]);

    PhysicalMemoryMappedIo uninitialized{profile.Access};
    IMemoryMappedIoWriter& uninitializedWriter = uninitialized;
    CheckRejectedWrite(runner, uninitializedWriter, baseAddress, MemoryMappedIoWidth::DoubleWord, 0xAAAA'5555U,
                       MemoryMappedIoWriteError::NotReady, registers, expectedRegisters, 4U,
                       "an uninitialized writer rejects access without changing memory");

    RegionFixture fixture;
    DeviceRegion region;
    const bool regionReady =
        fixture.InitializeRegion(region, baseAddress, sizeof(registers), ReadWritePermissions(), PciWidths());
    DeviceRegion* regions[]{&region};
    PhysicalMemoryMappedIo memoryMappedIo{profile.Access};
    const auto initialized = memoryMappedIo.Initialize(
        PhysicalMemoryMappedIoRegionView{
            .Data = regions,
            .Count = 1U,
        },
        profile.PhysicalAddressBits);
    IMemoryMappedIoWriter& writer = memoryMappedIo;
    ASTERIA_EXPECT(runner, "the write-rejection fixture initializes", regionReady,
                   initialized == PhysicalMemoryMappedIoInitializationError::None);
    CheckRejectedWrite(runner, writer, 0U, MemoryMappedIoWidth::DoubleWord, 0xAAAA'5555U,
                       MemoryMappedIoWriteError::InvalidAddress, registers, expectedRegisters, 4U,
                       "a null physical address is rejected without changing memory");
    CheckRejectedWrite(runner, writer, baseAddress, MemoryMappedIoWidth::Word, 0xAAAAU,
                       MemoryMappedIoWriteError::UnsupportedWidth, registers, expectedRegisters, 4U,
                       "a non-DWORD write is rejected without changing memory");
    CheckRejectedWrite(runner, writer, baseAddress, MemoryMappedIoWidth::DoubleWord, 0x1'0000'0000ULL,
                       MemoryMappedIoWriteError::ValueOutOfRange, registers, expectedRegisters, 4U,
                       "a value wider than 32 bits is rejected without changing memory");
    CheckRejectedWrite(runner, writer, baseAddress + 2U, MemoryMappedIoWidth::DoubleWord, 0xAAAA'5555U,
                       MemoryMappedIoWriteError::InvalidAddress, registers, expectedRegisters, 4U,
                       "a misaligned DWORD write is rejected without changing memory");
    CheckRejectedWrite(runner, writer, baseAddress - 4U, MemoryMappedIoWidth::DoubleWord, 0xAAAA'5555U,
                       MemoryMappedIoWriteError::InvalidAddress, registers, expectedRegisters, 4U,
                       "an address below the region is rejected without changing memory");
    CheckRejectedWrite(runner, writer, baseAddress + sizeof(registers), MemoryMappedIoWidth::DoubleWord, 0xAAAA'5555U,
                       MemoryMappedIoWriteError::InvalidAddress, registers, expectedRegisters, 4U,
                       "an address above the region is rejected without changing memory");
    CheckRejectedWrite(runner, writer, 0x8000'0000'0000'0000ULL, MemoryMappedIoWidth::DoubleWord, 0xAAAA'5555U,
                       MemoryMappedIoWriteError::InvalidAddress, registers, expectedRegisters, 4U,
                       "an unimplemented physical alias is rejected without changing memory");
    const bool quiesced = region.Quiesce().Error == Kernel::Device::DeviceRegionError::None;
    ASTERIA_EXPECT(runner, "the rejection fixture region quiesces", quiesced);
    CheckRejectedWrite(runner, writer, baseAddress, MemoryMappedIoWidth::DoubleWord, 0xAAAA'5555U,
                       MemoryMappedIoWriteError::NotReady, registers, expectedRegisters, 4U,
                       "a quiesced region rejects writes without changing memory");
}

void TestReadOnlyRegionRejectsWrite(TestContext& runner, const AccessProfile& profile) noexcept
{
    alignas(8) volatile UInt32 registers[2U]{
        0x1357'9BDFU,
        0x2468'ACE0U,
    };
    const UInt32 expectedRegisters[2U]{
        0x1357'9BDFU,
        0x2468'ACE0U,
    };
    const UInt64 baseAddress = reinterpret_cast<UInt64>(&registers[0U]);
    RegionFixture fixture;
    DeviceRegion region;
    const bool regionReady = fixture.InitializeRegion(region, baseAddress, sizeof(registers),
                                                      DeviceRegionPermissions{DeviceRegionPermission::Read},
                                                      DeviceAccessWidths{DeviceAccessWidth::DoubleWord});
    DeviceRegion* regions[]{&region};
    PhysicalMemoryMappedIo memoryMappedIo{profile.Access};
    const auto initialized = memoryMappedIo.Initialize(
        PhysicalMemoryMappedIoRegionView{
            .Data = regions,
            .Count = 1U,
        },
        profile.PhysicalAddressBits);
    IMemoryMappedIoWriter& writer = memoryMappedIo;
    ASTERIA_EXPECT(runner, "a read-only region remains a valid reader backend region", regionReady,
                   initialized == PhysicalMemoryMappedIoInitializationError::None);
    CheckRejectedWrite(runner, writer, baseAddress, MemoryMappedIoWidth::DoubleWord, 0xAAAA'5555U,
                       MemoryMappedIoWriteError::AccessDenied, registers, expectedRegisters, 2U,
                       "a read-only region denies writes without changing memory");
    const auto snapshot = region.GetSnapshot();
    ASTERIA_EXPECT(runner, "a denied write retains no region reference", snapshot.ActiveRegisterAccesses == 0U,
                   region.Quiesce().Error == Kernel::Device::DeviceRegionError::None);
}
} // namespace
} // namespace Asteria::Test

int main(const int argumentCount, char** const arguments)
{
    constexpr Asteria::Platform::Ia64::Device::Ia64PhysicalMemoryAccess ia64Access{};
    constexpr Asteria::Platform::Amd64::Device::Amd64PhysicalMemoryAccess amd64Access{};
    const char* const mode = (argumentCount == 2) ? arguments[1] : "";
    const Asteria::Test::AccessProfile* profile{nullptr};
    const Asteria::Test::AccessProfile ia64{ia64Access, 63U, 0x7FFF'FFFF'FFFF'FFFCULL};
    const Asteria::Test::AccessProfile amd64{amd64Access, 52U, 0x000F'FFFF'FFFF'FFFCULL};
    if (std::strcmp(mode, "ia64") == 0)
    {
        profile = &ia64;
    }
    else if (std::strcmp(mode, "amd64") == 0)
    {
        profile = &amd64;
    }
    else
    {
        static_cast<void>(std::fprintf(stderr, "usage: PhysicalMemoryMappedIoTests ia64|amd64\n"));
        return 2;
    }
    TestContext runner{"PhysicalMemoryMappedIoTests"};
    Asteria::Test::TestPhysicalRangeValidation(runner, *profile);
    Asteria::Test::TestReadAndBounds(runner, *profile);
    Asteria::Test::TestInitializationAndLifecycleFailures(runner, *profile);
    Asteria::Test::TestWriteAndCapabilitySeparation(runner, *profile);
    Asteria::Test::TestRejectedWritesPreserveMemory(runner, *profile);
    Asteria::Test::TestReadOnlyRegionRejectsWrite(runner, *profile);
    return runner.Finish(mode);
}
