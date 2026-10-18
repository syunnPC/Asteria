// SPDX-License-Identifier: Apache-2.0

#include "Platform/Device/PciEcamRuntime.hpp"

#include "Kernel/Device/PciBarProbe.hpp"

namespace Asteria::Platform::Device
{
PciEcamRuntime::PciEcamRuntime(Kernel::Object::ObjectRegistry& registry,
                               Platform::Synchronization::SpinLock& registrySynchronization,
                               Kernel::Object::RootCapabilityIssuer& issuer,
                               const Kernel::Object::CapabilityValidationKey& capabilityValidationKey,
                               const IPhysicalMemoryAccess& physicalMemoryAccess) noexcept
    : m_registry{registry}, m_physicalMemoryAccess{physicalMemoryAccess}, m_revocations{registrySynchronization},
      m_capabilitySpace{registry, m_revocations, m_clock, capabilityValidationKey.Word0, capabilityValidationKey.Word1},
      m_regionCapabilitySpace{registry, m_revocations, m_clock, capabilityValidationKey.Word0,
                              capabilityValidationKey.Word1},
      m_deviceManager{registry, issuer}, m_ecamMemoryMappedIo{physicalMemoryAccess},
      m_configurationSpace{m_ecamMemoryMappedIo, m_ecamMemoryMappedIo}, m_catalog{m_configurationSpace},
      m_abarMemoryMappedIo{physicalMemoryAccess}, m_barDecoder{m_configurationSpace},
      m_ahciSnapshotReader{m_abarMemoryMappedIo}, m_ahciBinding{m_catalog, m_barDecoder, m_ahciSnapshotReader}
{
}

PciEcamRuntime::PreparationResult PciEcamRuntime::PublishUnavailable(const PciEcamAvailability availability) noexcept
{
    const auto initialized = m_configurationSpace.Initialize(Kernel::Device::PciEcamRegionView{
        .Data = nullptr,
        .Count = 0U,
    });
    if (initialized != Kernel::Device::PciEcamConfigurationSpaceInitializationError::None)
    {
        m_failure.ConfigurationSpace = initialized;
        return Fail(PciEcamRuntimeError::ConfigurationSpaceRejected);
    }
    m_state = PciEcamRuntimeState::Unavailable;
    PublishAhciFailure(AhciPciRuntimeError::EcamUnavailable, AhciPciAvailability::EcamUnavailable);
    m_summary = PciEcamRuntimeSummary{
        .RegionCount = 0U,
        .BusCount = 0U,
        .DiscoveredFunctionCount = 0U,
        .PublishedFunctionCount = 0U,
        .ConfigurationReadCount = 0U,
        .InitialVendorDevice = 0U,
        .InitialFunction = {},
        .AhciFunction = {},
        .Availability = availability,
        .InitialFunctionPresent = false,
        .AhciFunctionPresent = false,
        .Reserved = {},
    };
    return PreparationResult::Success(m_summary);
}

PciEcamRuntime::PreparationResult PciEcamRuntime::Fail(const PciEcamRuntimeError error) noexcept
{
    m_failure.Error = error;
    m_state = PciEcamRuntimeState::Failed;
    if (m_ahciAvailability == AhciPciAvailability::NotEvaluated)
    {
        PublishAhciFailure(AhciPciRuntimeError::EcamPreparationFailed);
    }
    return PreparationResult::Failure(error);
}

Foundation::UInt64 PciEcamRuntime::GetApertureLength(const Kernel::Device::PciEcamRegion& region) noexcept
{
    const Foundation::UInt64 busCount =
        static_cast<Foundation::UInt64>(region.LastBus) - static_cast<Foundation::UInt64>(region.FirstBus) + 1U;
    return busCount << 20U;
}

PciEcamRuntime::PreparationResult
PciEcamRuntime::DiscoverAndPublishFunctions(const Kernel::Object::RevocationDomainId revocationDomain,
                                            const Kernel::Resource::ResourceDomainObjectId resourceDomainIdentity,
                                            Kernel::Resource::ResourceDomain& resourceDomain) noexcept
{
    m_failure.Catalog = m_catalog.Discover(Kernel::Device::PciBusRangeView{
        .Data = m_topology.BusRanges,
        .Count = m_topology.RegionCount,
    });
    if (m_failure.Catalog.Error != Kernel::Device::PciFunctionCatalogError::None)
    {
        return Fail(PciEcamRuntimeError::CatalogDiscoveryRejected);
    }

    const Kernel::Device::PciFunctionCatalogSnapshot catalog = m_catalog.GetSnapshot();
    if (catalog.FunctionCount > Kernel::Device::DeviceManager::MaximumPciFunctions)
    {
        return Fail(PciEcamRuntimeError::FunctionCapacityExceeded);
    }

    m_failure.CatalogBinding = m_deviceManager.BindPciFunctionCatalog(m_catalog);
    if (m_failure.CatalogBinding != Kernel::Device::PciFunctionManagerError::None)
    {
        return Fail(PciEcamRuntimeError::CatalogBindRejected);
    }

    for (Foundation::Size index{0U}; index < catalog.FunctionCount; ++index)
    {
        const auto descriptor = m_catalog.GetFunction(index);
        if (!descriptor.HasValue())
        {
            m_failure.Catalog = descriptor.GetError();
            return Fail(PciEcamRuntimeError::CatalogDiscoveryRejected);
        }

        const auto creation = m_deviceManager.CreatePciFunction(Kernel::Device::PciFunctionCreateRequest{
            .Destination = &m_capabilitySpace,
            .ResourceDomainStorage = &resourceDomain,
            .ResourceDomainIdentity = resourceDomainIdentity,
            .RevocationDomain = revocationDomain,
            .LeaseDeadline = Kernel::Object::MonotonicTime::Infinite(),
            .Address = descriptor.GetValue().Address,
            .Reserved = {},
        });
        if (!creation.HasValue())
        {
            m_failure.PciFunctionManager = creation.GetError();
            m_failure.PublishedFunctionCount = m_functionCount;
            return Fail(PciEcamRuntimeError::FunctionPublicationRejected);
        }

        m_functionPublications[index] = PciFunctionPublication{
            .Descriptor = descriptor.GetValue(),
            .Function = creation.GetValue().Function,
            .Capability = creation.GetValue().Capability,
            .CapabilitySpace = m_capabilitySpace.GetIdentifier(),
        };
        m_functionCount = index + 1U;
        m_failure.PublishedFunctionCount = m_functionCount;
    }

    return PublishReadySummary(catalog);
}

bool PciEcamRuntime::IsAhciFunction(const Kernel::Device::PciFunctionDescriptor& descriptor) noexcept
{
    constexpr Foundation::UInt8 MassStorageClass{0x01U};
    constexpr Foundation::UInt8 SerialAtaSubclass{0x06U};
    constexpr Foundation::UInt8 AhciProgrammingInterface{0x01U};
    constexpr Foundation::UInt8 EndpointHeaderLayout{0x00U};
    return (descriptor.ClassCode == MassStorageClass) && (descriptor.Subclass == SerialAtaSubclass) &&
           (descriptor.ProgrammingInterface == AhciProgrammingInterface) &&
           (descriptor.HeaderLayout == EndpointHeaderLayout);
}

PciEcamRuntime::PreparationResult
PciEcamRuntime::PublishReadySummary(const Kernel::Device::PciFunctionCatalogSnapshot& catalog) noexcept
{
    Kernel::Device::PciFunctionDescriptor ahciFunction{};
    Foundation::Size ahciFunctionCount{0U};
    for (Foundation::Size index{0U}; index < m_functionCount; ++index)
    {
        const Kernel::Device::PciFunctionDescriptor& descriptor = m_functionPublications[index].Descriptor;
        if (IsAhciFunction(descriptor))
        {
            ahciFunction = descriptor;
            ++ahciFunctionCount;
        }
    }
    const bool ahciFunctionPresent = ahciFunctionCount == 1U;
    if (!ahciFunctionPresent)
    {
        ahciFunction = {};
    }

    Kernel::Device::PciFunctionAddress initialFunction{};
    Foundation::UInt32 initialVendorDevice{0xFFFF'FFFFU};
    const bool initialFunctionPresent = m_functionCount != 0U;
    if (initialFunctionPresent)
    {
        const Kernel::Device::PciFunctionDescriptor& initial = m_functionPublications[0U].Descriptor;
        initialFunction = initial.Address;
        initialVendorDevice = static_cast<Foundation::UInt32>(initial.VendorId) |
                              (static_cast<Foundation::UInt32>(initial.DeviceId) << 16U);
    }

    m_state = PciEcamRuntimeState::Ready;
    m_summary = PciEcamRuntimeSummary{
        .RegionCount = m_regionCount,
        .BusCount = catalog.BusCount,
        .DiscoveredFunctionCount = catalog.FunctionCount,
        .PublishedFunctionCount = m_functionCount,
        .ConfigurationReadCount = catalog.ConfigurationReadCount,
        .InitialVendorDevice = initialVendorDevice,
        .InitialFunction = initialFunction,
        .AhciFunction = ahciFunction,
        .Availability = initialFunctionPresent ? PciEcamAvailability::Ready : PciEcamAvailability::NoFunctionPresent,
        .InitialFunctionPresent = initialFunctionPresent,
        .AhciFunctionPresent = ahciFunctionPresent,
        .Reserved = {},
    };
    return PreparationResult::Success(m_summary);
}

PciEcamRuntime::PreparationResult
PciEcamRuntime::Prepare(const Boot::BootInfo& bootInfo, const Platform::Firmware::AhciPciResource& ahciResource,
                        const Foundation::UInt8 physicalAddressBits,
                        const Kernel::Resource::ResourceDomainObjectId resourceDomainIdentity,
                        Kernel::Resource::ResourceDomain& resourceDomain) noexcept
{
    if (m_state != PciEcamRuntimeState::Uninitialized)
    {
        return PreparationResult::Failure(PciEcamRuntimeError::AlreadyPrepared);
    }
    if (!resourceDomainIdentity.IsValid() || !resourceDomainIdentity.IsFromRegistry(m_registry) ||
        !resourceDomain.IsReady())
    {
        return Fail(PciEcamRuntimeError::InvalidConfiguration);
    }

    m_failure.Topology = Platform::Acpi::Device::AcpiPciEcamTopologyAdapter::Build(bootInfo, m_topology);
    if (m_failure.Topology == Platform::Acpi::Device::AcpiPciEcamTopologyError::InvalidBootInfo)
    {
        return Fail(PciEcamRuntimeError::TopologyRejected);
    }
    if (m_failure.Topology != Platform::Acpi::Device::AcpiPciEcamTopologyError::None)
    {
        return PublishUnavailable(PciEcamAvailability::InvalidOptionalTopology);
    }
    if (m_topology.RegionCount == 0U)
    {
        return PublishUnavailable(PciEcamAvailability::Absent);
    }

    for (Foundation::Size index{0U}; index < m_topology.RegionCount; ++index)
    {
        const Kernel::Device::PciEcamRegion& region = m_topology.Regions[index];
        if (!m_physicalMemoryAccess.IsPhysicalRangeImplemented(region.PhysicalStart, GetApertureLength(region),
                                                               physicalAddressBits))
        {
            return PublishUnavailable(PciEcamAvailability::UnsupportedPhysicalRange);
        }
    }

    const auto revocation = m_revocations.CreateRoot();
    if (!revocation.HasValue())
    {
        m_failure.Revocation = revocation.GetError();
        return Fail(PciEcamRuntimeError::RevocationRejected);
    }

    const Kernel::Device::DeviceRegionPermissions permissions =
        Kernel::Device::DeviceRegionPermissions{Kernel::Device::DeviceRegionPermission::Read}.With(
            Kernel::Device::DeviceRegionPermission::Write);
    const Kernel::Device::DeviceAccessWidths widths =
        Kernel::Device::DeviceAccessWidths{Kernel::Device::DeviceAccessWidth::Byte}
            .With(Kernel::Device::DeviceAccessWidth::Word)
            .With(Kernel::Device::DeviceAccessWidth::DoubleWord);
    for (Foundation::Size index{0U}; index < m_topology.RegionCount; ++index)
    {
        const Kernel::Device::PciEcamRegion& region = m_topology.Regions[index];
        const auto created = m_deviceManager.Create(Kernel::Device::DeviceRegionCreateRequest{
            .Destination = &m_regionCapabilitySpace,
            .ResourceDomainStorage = &resourceDomain,
            .ResourceDomainIdentity = resourceDomainIdentity,
            .Descriptor =
                Kernel::Device::DeviceRegionDescriptor{
                    .BaseAddress = region.PhysicalStart,
                    .LengthBytes = GetApertureLength(region),
                    .Kind = Kernel::Device::DeviceRegionKind::MemoryMappedIo,
                    .Permissions = permissions,
                    .Widths = widths,
                    .CachePolicy = Kernel::Memory::MemoryCachePolicy::Uncached,
                    .Reserved = {},
                },
            .RevocationDomain = revocation.GetValue(),
            .LeaseDeadline = Kernel::Object::MonotonicTime::Infinite(),
        });
        if (!created.HasValue())
        {
            m_failure.DeviceManager = created.GetError();
            return Fail(PciEcamRuntimeError::RegionRejected);
        }
        m_creations[index] = created.GetValue();
        m_regions[index] = m_creations[index].Storage;
        m_regionCount = index + 1U;
        m_failure.Capability = m_regionCapabilitySpace.Close(m_creations[index].Capability);
        if (m_failure.Capability != Kernel::Object::CapabilityError::Ok)
        {
            return Fail(PciEcamRuntimeError::CapabilityCloseRejected);
        }
    }

    m_failure.MemoryMappedIo = m_ecamMemoryMappedIo.Initialize(
        PhysicalMemoryMappedIoRegionView{
            .Data = m_regions,
            .Count = m_regionCount,
        },
        physicalAddressBits);
    if (m_failure.MemoryMappedIo != PhysicalMemoryMappedIoInitializationError::None)
    {
        return Fail(PciEcamRuntimeError::MemoryMappedIoRejected);
    }
    m_failure.ConfigurationSpace = m_configurationSpace.Initialize(Kernel::Device::PciEcamRegionView{
        .Data = m_topology.Regions,
        .Count = m_topology.RegionCount,
    });
    if (m_failure.ConfigurationSpace != Kernel::Device::PciEcamConfigurationSpaceInitializationError::None)
    {
        return Fail(PciEcamRuntimeError::ConfigurationSpaceRejected);
    }

    const PreparationResult discovery =
        DiscoverAndPublishFunctions(revocation.GetValue(), resourceDomainIdentity, resourceDomain);
    if (!discovery.HasValue())
    {
        return discovery;
    }
    PrepareAhci(ahciResource, physicalAddressBits, revocation.GetValue(), resourceDomainIdentity, resourceDomain);
    return discovery;
}

void PciEcamRuntime::PublishAhciFailure(const AhciPciRuntimeError error,
                                        const AhciPciAvailability availability) noexcept
{
    m_ahciFailure.Error = error;
    m_ahciAvailability = availability;
}

Foundation::Result<AssignableDevice, PciEcamRuntimeError>
PciEcamRuntime::PublishAssignableDevice(const Platform::Firmware::AhciPciResource& resource,
                                        const Foundation::UInt8 physicalAddressBits,
                                        const Kernel::Resource::ResourceDomainObjectId resourceDomainIdentity,
                                        Kernel::Resource::ResourceDomain& resourceDomain) noexcept
{
    using ResultType = Foundation::Result<AssignableDevice, PciEcamRuntimeError>;
    if (m_state != PciEcamRuntimeState::Ready)
    {
        return ResultType::Failure(PciEcamRuntimeError::InvalidConfiguration);
    }
    const PciFunctionPublication* const function = ValidateMemoryBar(resource, physicalAddressBits);
    if (function == nullptr)
    {
        return ResultType::Failure(PciEcamRuntimeError::DeviceRejected);
    }
    return CreateAssignableRegion(resource, function->Function.GetObjectId(), resourceDomainIdentity, resourceDomain);
}

const PciFunctionPublication* PciEcamRuntime::ValidateMemoryBar(const Platform::Firmware::AhciPciResource& resource,
                                                                const Foundation::UInt8 physicalAddressBits) noexcept
{
    const Kernel::Device::PciFunctionAddress address = GetAddress(resource);
    const PciFunctionPublication* function{nullptr};
    for (Foundation::Size index{0U}; index < m_functionCount; ++index)
    {
        function = m_functionPublications[index].Descriptor.Address.IsEqualTo(address) ? &m_functionPublications[index]
                                                                                       : function;
    }
    Foundation::UInt32 command{0U};
    constexpr Foundation::UInt32 MemorySpaceEnableMask{1U << 1U};
    if ((function == nullptr) || (resource.AbarLengthBytes == 0U) || (resource.BarIndex > 5U) ||
        (physicalAddressBits < 32U) || (physicalAddressBits > m_physicalMemoryAccess.GetMaximumPhysicalAddressBits()) ||
        !m_physicalMemoryAccess.IsPhysicalRangeImplemented(resource.AbarBaseAddress, resource.AbarLengthBytes,
                                                           physicalAddressBits) ||
        (m_configurationSpace.Read32(address, 0x04U, command) != Hal::Device::PciConfigurationReadError::None) ||
        ((command & MemorySpaceEnableMask) == 0U))
    {
        return nullptr;
    }
    Kernel::Device::PciEndpointBar bar{};
    const auto decoded = m_barDecoder.Decode(
        Kernel::Device::PciEndpointBarDecodeRequest{
            .MaximumPhysicalAddress = GetMaximumPhysicalAddress(physicalAddressBits),
            .ExtentBytes = resource.AbarLengthBytes,
            .Address = address,
            .ExtentState = Kernel::Device::PciEndpointBarExtentState::Authoritative,
            .BarIndex = resource.BarIndex,
        },
        bar);
    if ((decoded.Error != Kernel::Device::PciEndpointBarDecodeError::None) ||
        ((bar.Kind != Kernel::Device::PciEndpointBarKind::Memory32) &&
         (bar.Kind != Kernel::Device::PciEndpointBarKind::Memory64)) ||
        (bar.BaseAddress != resource.AbarBaseAddress) || (bar.ExtentBytes != resource.AbarLengthBytes))
    {
        return nullptr;
    }
    return function;
}

// A memory BAR starting before start cannot reach into it unless it covers
// the whole page, which would overlap the BAR the page holds: a BAR is
// aligned to its power-of-two size, so one of less than a page never crosses
// a page boundary. Starting in the range is therefore the only overlap.
bool PciEcamRuntime::OtherMemoryBarIn(const Kernel::Device::PciFunctionAddress address,
                                      const Foundation::UInt8 barIndex, const Foundation::UInt64 start,
                                      const Foundation::UInt64 bytes) noexcept
{
    for (Foundation::Size index{0U}; index < m_functionCount; ++index)
    {
        const Kernel::Device::PciFunctionDescriptor& descriptor = m_functionPublications[index].Descriptor;
        const Foundation::UInt8 layout = descriptor.HeaderLayout & 0x7FU;
        const Foundation::UInt8 bars = (layout == 0U) ? 6U : ((layout == 1U) ? 2U : 0U);
        for (Foundation::UInt8 bar{0U}; bar < bars; ++bar)
        {
            Foundation::UInt32 low{0U};
            if (m_configurationSpace.Read32(descriptor.Address, static_cast<Foundation::UInt16>(0x10U + bar * 4U),
                                            low) != Hal::Device::PciConfigurationReadError::None)
            {
                return true;
            }
            if ((low & 1U) != 0U)
            {
                continue;
            }
            Foundation::UInt64 base = low & ~Foundation::UInt64{0xFU};
            const bool wide = ((low >> 1U) & 3U) == 2U;
            if (wide && (bar + 1U < bars))
            {
                Foundation::UInt32 high{0U};
                if (m_configurationSpace.Read32(descriptor.Address, static_cast<Foundation::UInt16>(0x14U + bar * 4U),
                                                high) != Hal::Device::PciConfigurationReadError::None)
                {
                    return true;
                }
                base |= Foundation::UInt64{high} << 32U;
            }
            const bool own = descriptor.Address.IsEqualTo(address) && (bar == barIndex);
            if (!own && (base != 0U) && (base >= start) && (base - start < bytes))
            {
                return true;
            }
            bar = static_cast<Foundation::UInt8>(bar + (wide ? 1U : 0U));
        }
    }
    return false;
}

Foundation::Result<AssignableDevice, PciEcamRuntimeError> PciEcamRuntime::PublishUsbHostDevice(
    const Kernel::Device::PciFunctionAddress address, const Platform::Firmware::AhciPciResource* const firmware,
    const Foundation::UInt8 physicalAddressBits, const Kernel::Resource::ResourceDomainObjectId resourceDomainIdentity,
    Kernel::Resource::ResourceDomain& resourceDomain) noexcept
{
    using ResultType = Foundation::Result<AssignableDevice, PciEcamRuntimeError>;
    constexpr Foundation::UInt64 Page{4096U};
    constexpr Foundation::UInt32 IoSpaceEnableMask{1U};
    constexpr Foundation::UInt32 InterruptDisableMask{1U << 10U};
    constexpr Foundation::UInt64 UhciIoBytes{32U};
    if (m_state != PciEcamRuntimeState::Ready)
    {
        return ResultType::Failure(PciEcamRuntimeError::InvalidConfiguration);
    }
    const PciFunctionPublication* function{nullptr};
    for (Foundation::Size index{0U}; index < m_functionCount; ++index)
    {
        function = m_functionPublications[index].Descriptor.Address.IsEqualTo(address) ? &m_functionPublications[index]
                                                                                       : function;
    }
    if ((function == nullptr) || (function->Descriptor.ClassCode != 0x0CU) || (function->Descriptor.Subclass != 3U) ||
        ((function->Descriptor.ProgrammingInterface != 0x00U) && (function->Descriptor.ProgrammingInterface != 0x10U) &&
         (function->Descriptor.ProgrammingInterface != 0x20U)))
    {
        return ResultType::Failure(PciEcamRuntimeError::DeviceRejected);
    }
    if (function->Descriptor.ProgrammingInterface == 0x00U)
    {
        // UHCI: USBBASE at 0x20, I/O bits 15:5 (UHCI 1.1 section 2.1.3).
        Foundation::UInt32 command{0U};
        Foundation::UInt32 bar{0U};
        if ((m_configurationSpace.Read32(address, 0x04U, command) != Hal::Device::PciConfigurationReadError::None) ||
            (m_configurationSpace.Read32(address, 0x20U, bar) != Hal::Device::PciConfigurationReadError::None) ||
            ((command & IoSpaceEnableMask) == 0U) || ((bar & 1U) == 0U) || ((bar & 0xFFE0U) == 0U) ||
            ((bar & 0xFFFF'0000U) != 0U) ||
            (m_configurationSpace.Write32(address, 0x04U, (command & 0xFFFFU) | InterruptDisableMask) !=
             Hal::Device::PciConfigurationWriteError::None))
        {
            return ResultType::Failure(PciEcamRuntimeError::DeviceRejected);
        }
        const Platform::Firmware::AhciPciResource ports{.AbarBaseAddress = bar & 0xFFE0U,
                                                        .AbarLengthBytes = UhciIoBytes,
                                                        .Segment = address.GetSegment(),
                                                        .Bus = address.GetBus(),
                                                        .Device = address.GetDevice(),
                                                        .Function = address.GetFunction(),
                                                        .BarIndex = 4U,
                                                        .Reserved = {0U, 0U}};
        return CreateAssignableRegion(ports, function->Function.GetObjectId(), resourceDomainIdentity, resourceDomain,
                                      true);
    }
    const auto captured =
        (firmware != nullptr)
            ? Foundation::Result<Platform::Firmware::AhciPciResource, PciEcamRuntimeError>::Success(*firmware)
            : CaptureAssignableResource(address, 0U);
    if (!captured.HasValue())
    {
        return ResultType::Failure(captured.GetError());
    }
    Platform::Firmware::AhciPciResource resource = captured.GetValue();
    if ((ValidateMemoryBar(resource, physicalAddressBits) == nullptr) ||
        (resource.AbarBaseAddress > ~Foundation::UInt64{0U} - resource.AbarLengthBytes - Page))
    {
        return ResultType::Failure(PciEcamRuntimeError::DeviceRejected);
    }
    const Foundation::UInt64 start = resource.AbarBaseAddress & ~(Page - 1U);
    const Foundation::UInt64 end = (resource.AbarBaseAddress + resource.AbarLengthBytes + Page - 1U) & ~(Page - 1U);
    if (((start != resource.AbarBaseAddress) || (end - start != resource.AbarLengthBytes)) &&
        (!m_physicalMemoryAccess.IsPhysicalRangeImplemented(start, end - start, physicalAddressBits) ||
         OtherMemoryBarIn(address, 0U, start, end - start)))
    {
        return ResultType::Failure(PciEcamRuntimeError::DeviceRejected);
    }
    // Command's low half only (Status above it is write-one-to-clear).
    Foundation::UInt32 command{0U};
    if ((m_configurationSpace.Read32(address, 0x04U, command) != Hal::Device::PciConfigurationReadError::None) ||
        (m_configurationSpace.Write32(address, 0x04U, (command & 0xFFFFU) | InterruptDisableMask) !=
         Hal::Device::PciConfigurationWriteError::None))
    {
        return ResultType::Failure(PciEcamRuntimeError::DeviceRejected);
    }
    resource.AbarBaseAddress = start;
    resource.AbarLengthBytes = end - start;
    return CreateAssignableRegion(resource, function->Function.GetObjectId(), resourceDomainIdentity, resourceDomain);
}

Foundation::Result<Kernel::Object::ObjectId, PciEcamRuntimeError> PciEcamRuntime::PublishScanoutRegion(
    const Foundation::UInt64 base, const Foundation::UInt64 bytes, const Foundation::UInt8 physicalAddressBits,
    const Kernel::Resource::ResourceDomainObjectId resourceDomainIdentity,
    Kernel::Resource::ResourceDomain& resourceDomain, const Kernel::Memory::MemoryCachePolicy cachePolicy) noexcept
{
    using ResultType = Foundation::Result<Kernel::Object::ObjectId, PciEcamRuntimeError>;
    constexpr Foundation::UInt64 PageMask{0xFFFU};
    // The scanout is no PCI function: a machine without ECAM (Hyper-V's
    // generation 2, which has no PCI bus) still publishes its framebuffer.
    if (((cachePolicy != Kernel::Memory::MemoryCachePolicy::Uncached) &&
         (cachePolicy != Kernel::Memory::MemoryCachePolicy::WriteCombining)) ||
        ((m_state != PciEcamRuntimeState::Ready) && (m_state != PciEcamRuntimeState::Unavailable)) || (bytes == 0U) ||
        ((base & PageMask) != 0U) || ((bytes & PageMask) != 0U) || (physicalAddressBits < 32U) ||
        (physicalAddressBits > m_physicalMemoryAccess.GetMaximumPhysicalAddressBits()) ||
        !m_physicalMemoryAccess.IsPhysicalRangeImplemented(base, bytes, physicalAddressBits))
    {
        return ResultType::Failure(PciEcamRuntimeError::DeviceRejected);
    }
    const auto revocation = m_revocations.CreateRoot();
    if (!revocation.HasValue())
    {
        return ResultType::Failure(PciEcamRuntimeError::RevocationRejected);
    }
    const auto created = m_deviceManager.Create(Kernel::Device::DeviceRegionCreateRequest{
        .Destination = &m_regionCapabilitySpace,
        .ResourceDomainStorage = &resourceDomain,
        .ResourceDomainIdentity = resourceDomainIdentity,
        .Descriptor =
            Kernel::Device::DeviceRegionDescriptor{
                .BaseAddress = base,
                .LengthBytes = bytes,
                .Kind = Kernel::Device::DeviceRegionKind::MemoryMappedIo,
                .Permissions =
                    Kernel::Device::DeviceRegionPermissions{Kernel::Device::DeviceRegionPermission::Read}.With(
                        Kernel::Device::DeviceRegionPermission::Write),
                .Widths = Kernel::Device::DeviceAccessWidths{Kernel::Device::DeviceAccessWidth::DoubleWord}.With(
                    Kernel::Device::DeviceAccessWidth::QuadWord),
                .CachePolicy = cachePolicy,
                .Reserved = {},
            },
        .RevocationDomain = revocation.GetValue(),
        .LeaseDeadline = Kernel::Object::MonotonicTime::Infinite(),
    });
    if (!created.HasValue() ||
        (m_regionCapabilitySpace.Close(created.GetValue().Capability) != Kernel::Object::CapabilityError::Ok))
    {
        return ResultType::Failure(created.HasValue() ? PciEcamRuntimeError::RegionRejected
                                                      : PciEcamRuntimeError::DeviceRejected);
    }
    return ResultType::Success(created.GetValue().Region.GetObjectId());
}

Foundation::Result<AssignableDevice, PciEcamRuntimeError>
PciEcamRuntime::PublishAhciDevice(const Platform::Firmware::AhciPciResource& resource) const noexcept
{
    using ResultType = Foundation::Result<AssignableDevice, PciEcamRuntimeError>;
    if ((m_state != PciEcamRuntimeState::Ready) || (m_ahciAvailability != AhciPciAvailability::Ready))
    {
        return ResultType::Failure(PciEcamRuntimeError::DeviceRejected);
    }
    const Kernel::Device::PciFunctionAddress address = GetAddress(resource);
    for (Foundation::Size index{0U}; index < m_functionCount; ++index)
    {
        if (m_functionPublications[index].Descriptor.Address.IsEqualTo(address))
        {
            return ResultType::Success(AssignableDevice{
                .Function = m_functionPublications[index].Function.GetObjectId(),
                .Region = m_abarCreation.Region.GetObjectId(),
            });
        }
    }
    return ResultType::Failure(PciEcamRuntimeError::DeviceRejected);
}

Foundation::Result<Platform::Firmware::AhciPciResource, PciEcamRuntimeError>
PciEcamRuntime::CaptureAssignableResource(const Foundation::UInt32 vendorDevice,
                                          const Foundation::UInt8 barIndex) noexcept
{
    using ResultType = Foundation::Result<Platform::Firmware::AhciPciResource, PciEcamRuntimeError>;
    if (m_state != PciEcamRuntimeState::Ready)
    {
        return ResultType::Failure(PciEcamRuntimeError::InvalidConfiguration);
    }
    const PciFunctionPublication* function{nullptr};
    for (Foundation::Size index{0U}; (function == nullptr) && (index < m_functionCount); ++index)
    {
        const Kernel::Device::PciFunctionDescriptor& descriptor = m_functionPublications[index].Descriptor;
        const Foundation::UInt32 identity =
            descriptor.VendorId | (static_cast<Foundation::UInt32>(descriptor.DeviceId) << 16U);
        function = (identity == vendorDevice) ? &m_functionPublications[index] : nullptr;
    }
    if (function == nullptr)
    {
        return ResultType::Failure(PciEcamRuntimeError::DeviceRejected);
    }
    return CaptureAssignableResource(function->Descriptor.Address, barIndex);
}

Foundation::Result<Platform::Firmware::AhciPciResource, PciEcamRuntimeError>
PciEcamRuntime::CaptureAssignableResource(const Kernel::Device::PciFunctionAddress address,
                                          const Foundation::UInt8 barIndex) noexcept
{
    using ResultType = Foundation::Result<Platform::Firmware::AhciPciResource, PciEcamRuntimeError>;
    if (m_state != PciEcamRuntimeState::Ready)
    {
        return ResultType::Failure(PciEcamRuntimeError::InvalidConfiguration);
    }
    bool known{false};
    for (Foundation::Size index{0U}; index < m_functionCount; ++index)
    {
        known = known || m_functionPublications[index].Descriptor.Address.IsEqualTo(address);
    }
    if (!known)
    {
        return ResultType::Failure(PciEcamRuntimeError::DeviceRejected);
    }
    const auto probed =
        Kernel::Device::ProbePciMemoryBar(m_configurationSpace, m_configurationSpace, address, barIndex);
    // The Command register's low half only: Status, above it, clears what a
    // one is written to.
    constexpr Foundation::UInt32 InterruptDisableMask{1U << 10U};
    Foundation::UInt32 command{0U};
    if (!probed.HasValue() ||
        (m_configurationSpace.Read32(address, 0x04U, command) != Hal::Device::PciConfigurationReadError::None) ||
        (m_configurationSpace.Write32(address, 0x04U, (command & 0xFFFFU) | InterruptDisableMask) !=
         Hal::Device::PciConfigurationWriteError::None))
    {
        return ResultType::Failure(PciEcamRuntimeError::DeviceRejected);
    }
    return ResultType::Success(Platform::Firmware::AhciPciResource{
        .AbarBaseAddress = probed.GetValue().BaseAddress,
        .AbarLengthBytes = probed.GetValue().LengthBytes,
        .Segment = address.GetSegment(),
        .Bus = address.GetBus(),
        .Device = address.GetDevice(),
        .Function = address.GetFunction(),
        .BarIndex = barIndex,
        .Reserved = {0U, 0U},
    });
}

// Configuration dword 0x3C holds the interrupt line in byte 0 and the pin
// (1-4 for INTA-INTD, 0 for none) in byte 1.
Foundation::Result<Foundation::UInt8, PciEcamRuntimeError>
PciEcamRuntime::IsolateInterruptLine(const Kernel::Device::PciFunctionAddress address) noexcept
{
    using ResultType = Foundation::Result<Foundation::UInt8, PciEcamRuntimeError>;
    if (m_state != PciEcamRuntimeState::Ready)
    {
        return ResultType::Failure(PciEcamRuntimeError::InvalidConfiguration);
    }
    Foundation::UInt32 interrupt{0U};
    if ((m_configurationSpace.Read32(address, 0x3CU, interrupt) != Hal::Device::PciConfigurationReadError::None) ||
        (((interrupt >> 8U) & 0xFFU) == 0U))
    {
        return ResultType::Failure(PciEcamRuntimeError::DeviceRejected);
    }
    const Foundation::UInt8 line = static_cast<Foundation::UInt8>(interrupt & 0xFFU);
    constexpr Foundation::UInt32 InterruptDisableMask{1U << 10U};
    for (Foundation::Size index{0U}; index < m_functionCount; ++index)
    {
        const Kernel::Device::PciFunctionAddress other = m_functionPublications[index].Descriptor.Address;
        Foundation::UInt32 shared{0U};
        Foundation::UInt32 command{0U};
        if (other.IsEqualTo(address) ||
            (m_configurationSpace.Read32(other, 0x3CU, shared) != Hal::Device::PciConfigurationReadError::None) ||
            (((shared >> 8U) & 0xFFU) == 0U) || ((shared & 0xFFU) != line))
        {
            continue;
        }
        // The Command register's low half only: Status clears what a one is
        // written to.
        if ((m_configurationSpace.Read32(other, 0x04U, command) != Hal::Device::PciConfigurationReadError::None) ||
            (m_configurationSpace.Write32(other, 0x04U, (command & 0xFFFFU) | InterruptDisableMask) !=
             Hal::Device::PciConfigurationWriteError::None))
        {
            return ResultType::Failure(PciEcamRuntimeError::DeviceRejected);
        }
    }
    return ResultType::Success(line);
}

Foundation::Result<AssignableDevice, PciEcamRuntimeError>
PciEcamRuntime::CreateAssignableRegion(const Platform::Firmware::AhciPciResource& resource,
                                       const Kernel::Object::ObjectId function,
                                       const Kernel::Resource::ResourceDomainObjectId resourceDomainIdentity,
                                       Kernel::Resource::ResourceDomain& resourceDomain, const bool portIo) noexcept
{
    using ResultType = Foundation::Result<AssignableDevice, PciEcamRuntimeError>;
    const auto revocation = m_revocations.CreateRoot();
    if (!revocation.HasValue())
    {
        return ResultType::Failure(PciEcamRuntimeError::RevocationRejected);
    }
    const auto created =
        m_deviceManager.Create(
            Kernel::Device::DeviceRegionCreateRequest{
                .Destination = &m_regionCapabilitySpace,
                .ResourceDomainStorage = &resourceDomain,
                .ResourceDomainIdentity = resourceDomainIdentity,
                .Descriptor =
                    Kernel::Device::DeviceRegionDescriptor{
                        .BaseAddress = resource.AbarBaseAddress,
                        .LengthBytes = resource.AbarLengthBytes,
                        .Kind = portIo ? Kernel::Device::DeviceRegionKind::PortIo
                                       : Kernel::Device::DeviceRegionKind::MemoryMappedIo,
                        .Permissions =
                            Kernel::Device::DeviceRegionPermissions{Kernel::Device::DeviceRegionPermission::Read}.With(
                                Kernel::Device::DeviceRegionPermission::Write),
                        .Widths =
                            portIo ? Kernel::Device::DeviceAccessWidths{Kernel::Device::DeviceAccessWidth::Byte}
                                         .With(Kernel::Device::DeviceAccessWidth::Word)
                                         .With(Kernel::Device::DeviceAccessWidth::DoubleWord)
                                   : Kernel::Device::DeviceAccessWidths{Kernel::Device::DeviceAccessWidth::DoubleWord}
                                         .With(Kernel::Device::DeviceAccessWidth::QuadWord),
                        .CachePolicy = Kernel::Memory::MemoryCachePolicy::Uncached,
                        .Reserved = {},
                    },
                .RevocationDomain = revocation.GetValue(),
                .LeaseDeadline = Kernel::Object::MonotonicTime::Infinite(),
            });
    if (!created.HasValue() ||
        (m_regionCapabilitySpace.Close(created.GetValue().Capability) != Kernel::Object::CapabilityError::Ok))
    {
        return ResultType::Failure(PciEcamRuntimeError::RegionRejected);
    }
    return ResultType::Success(AssignableDevice{
        .Function = function,
        .Region = created.GetValue().Region.GetObjectId(),
    });
}

Kernel::Device::PciFunctionAddress
PciEcamRuntime::GetAddress(const Platform::Firmware::AhciPciResource& resource) noexcept
{
    return Kernel::Device::PciFunctionAddress{
        resource.Segment,
        resource.Bus,
        resource.Device,
        resource.Function,
    };
}

bool PciEcamRuntime::FirmwareResourceIsValid(const Platform::Firmware::AhciPciResource& resource) noexcept
{
    if (!GetAddress(resource).IsValid() || (resource.AbarLengthBytes == 0U))
    {
        return false;
    }
    for (const Foundation::UInt8 value : resource.Reserved)
    {
        if (value != 0U)
        {
            return false;
        }
    }
    return true;
}

Foundation::UInt64 PciEcamRuntime::GetMaximumPhysicalAddress(const Foundation::UInt8 physicalAddressBits) noexcept
{
    return (Foundation::UInt64{1U} << physicalAddressBits) - 1U;
}

bool PciEcamRuntime::SelectCatalogAhci(Kernel::Device::PciFunctionDescriptor& output) noexcept
{
    const Kernel::Device::PciFunctionCatalogSnapshot snapshot = m_catalog.GetSnapshot();
    if (snapshot.State != Kernel::Device::PciFunctionCatalogState::Ready)
    {
        m_ahciFailure.Catalog.Error = Kernel::Device::PciFunctionCatalogError::NotReady;
        PublishAhciFailure(AhciPciRuntimeError::CatalogNotReady);
        return false;
    }

    Kernel::Device::PciFunctionDescriptor selected{};
    Foundation::Size count{0U};
    for (Foundation::Size index{0U}; index < snapshot.FunctionCount; ++index)
    {
        const auto function = m_catalog.GetFunction(index);
        if (!function.HasValue())
        {
            m_ahciFailure.Catalog = function.GetError();
            m_ahciFailure.MatchingFunctionCount = count;
            PublishAhciFailure(AhciPciRuntimeError::CatalogQueryRejected);
            return false;
        }
        if (IsAhciFunction(function.GetValue()))
        {
            selected = function.GetValue();
            ++count;
        }
    }
    m_ahciFailure.MatchingFunctionCount = count;
    if (count == 0U)
    {
        PublishAhciFailure(AhciPciRuntimeError::NoCatalogAhciController, AhciPciAvailability::Absent);
        return false;
    }
    if (count != 1U)
    {
        PublishAhciFailure(AhciPciRuntimeError::AmbiguousCatalogAhciController);
        return false;
    }
    output = selected;
    return true;
}

bool PciEcamRuntime::BindingMatchesResource(const Kernel::Device::AhciPciBootstrapBindingResult& binding,
                                            const Platform::Firmware::AhciPciResource& resource) noexcept
{
    return binding.Function.Address.IsEqualTo(GetAddress(resource)) &&
           (binding.Abar.BaseAddress == resource.AbarBaseAddress) &&
           (binding.Abar.ExtentBytes == resource.AbarLengthBytes) &&
           (binding.Abar.Kind == Kernel::Device::PciEndpointBarKind::Memory32) &&
           (binding.Abar.ExtentState == Kernel::Device::PciEndpointBarExtentState::Authoritative) &&
           (binding.Abar.BarIndex == 5U) && (binding.Abar.RegisterCount == 1U) && !binding.Abar.Prefetchable;
}

void PciEcamRuntime::PrepareAhci(const Platform::Firmware::AhciPciResource& resource,
                                 const Foundation::UInt8 physicalAddressBits,
                                 const Kernel::Object::RevocationDomainId revocationDomain,
                                 const Kernel::Resource::ResourceDomainObjectId resourceDomainIdentity,
                                 Kernel::Resource::ResourceDomain& resourceDomain) noexcept
{
    Kernel::Device::PciFunctionDescriptor catalogAhci{};
    if (!SelectCatalogAhci(catalogAhci))
    {
        return;
    }
    if (!FirmwareResourceIsValid(resource))
    {
        PublishAhciFailure(AhciPciRuntimeError::InvalidFirmwareResource);
        return;
    }
    const Kernel::Device::PciFunctionAddress capturedAddress = GetAddress(resource);
    if (!catalogAhci.Address.IsEqualTo(capturedAddress))
    {
        PublishAhciFailure(AhciPciRuntimeError::FirmwareBdfMismatch);
        return;
    }

    if ((physicalAddressBits < 32U) || (physicalAddressBits > m_physicalMemoryAccess.GetMaximumPhysicalAddressBits()))
    {
        PublishAhciFailure(AhciPciRuntimeError::InvalidPhysicalAddressWidth);
        return;
    }
    if (!m_physicalMemoryAccess.IsPhysicalRangeImplemented(resource.AbarBaseAddress, resource.AbarLengthBytes,
                                                           physicalAddressBits))
    {
        PublishAhciFailure(AhciPciRuntimeError::PhysicalRangeRejected);
        return;
    }
    const Foundation::UInt64 maximumPhysicalAddress = GetMaximumPhysicalAddress(physicalAddressBits);

    m_ahciFailure.ConfigurationRead =
        m_configurationSpace.Read32(capturedAddress, 0x04U, m_ahciFailure.CommandRegister);
    if (m_ahciFailure.ConfigurationRead != Hal::Device::PciConfigurationReadError::None)
    {
        PublishAhciFailure(AhciPciRuntimeError::CommandReadRejected);
        return;
    }
    constexpr Foundation::UInt32 MemorySpaceEnableMask{1U << 1U};
    if ((m_ahciFailure.CommandRegister & MemorySpaceEnableMask) == 0U)
    {
        PublishAhciFailure(AhciPciRuntimeError::MemorySpaceDisabled);
        return;
    }
    constexpr Foundation::UInt32 BusMasterEnableMask{1U << 2U};
    if ((m_ahciFailure.CommandRegister & BusMasterEnableMask) == 0U)
    {
        PublishAhciFailure(AhciPciRuntimeError::BusMasterDisabled);
        return;
    }

    Kernel::Device::PciEndpointBar abar{};
    m_ahciFailure.Abar = m_barDecoder.Decode(
        Kernel::Device::PciEndpointBarDecodeRequest{
            .MaximumPhysicalAddress = maximumPhysicalAddress,
            .ExtentBytes = resource.AbarLengthBytes,
            .Address = capturedAddress,
            .ExtentState = Kernel::Device::PciEndpointBarExtentState::Authoritative,
            .BarIndex = 5U,
        },
        abar);
    if (m_ahciFailure.Abar.Error != Kernel::Device::PciEndpointBarDecodeError::None)
    {
        PublishAhciFailure(AhciPciRuntimeError::AbarPreflightRejected);
        return;
    }
    if ((abar.Kind != Kernel::Device::PciEndpointBarKind::Memory32) || (abar.RegisterCount != 1U))
    {
        PublishAhciFailure(AhciPciRuntimeError::AbarKindRejected);
        return;
    }
    if (abar.Prefetchable)
    {
        PublishAhciFailure(AhciPciRuntimeError::AbarPrefetchable);
        return;
    }
    if ((abar.BaseAddress != resource.AbarBaseAddress) || (abar.ExtentBytes != resource.AbarLengthBytes))
    {
        PublishAhciFailure(AhciPciRuntimeError::AbarBaseMismatch);
        return;
    }

    if (!CreateAhciRegion(resource, physicalAddressBits, revocationDomain, resourceDomainIdentity, resourceDomain))
    {
        return;
    }
    BindAhci(resource, maximumPhysicalAddress);
}

bool PciEcamRuntime::CreateAhciRegion(const Platform::Firmware::AhciPciResource& resource,
                                      const Foundation::UInt8 physicalAddressBits,
                                      const Kernel::Object::RevocationDomainId revocationDomain,
                                      const Kernel::Resource::ResourceDomainObjectId resourceDomainIdentity,
                                      Kernel::Resource::ResourceDomain& resourceDomain) noexcept
{
    const auto created = m_deviceManager.Create(Kernel::Device::DeviceRegionCreateRequest{
        .Destination = &m_regionCapabilitySpace,
        .ResourceDomainStorage = &resourceDomain,
        .ResourceDomainIdentity = resourceDomainIdentity,
        .Descriptor =
            Kernel::Device::DeviceRegionDescriptor{
                .BaseAddress = resource.AbarBaseAddress,
                .LengthBytes = resource.AbarLengthBytes,
                .Kind = Kernel::Device::DeviceRegionKind::MemoryMappedIo,
                .Permissions =
                    Kernel::Device::DeviceRegionPermissions{Kernel::Device::DeviceRegionPermission::Read}.With(
                        Kernel::Device::DeviceRegionPermission::Write),
                .Widths = Kernel::Device::DeviceAccessWidths{Kernel::Device::DeviceAccessWidth::DoubleWord},
                .CachePolicy = Kernel::Memory::MemoryCachePolicy::Uncached,
                .Reserved = {},
            },
        .RevocationDomain = revocationDomain,
        .LeaseDeadline = Kernel::Object::MonotonicTime::Infinite(),
    });
    if (!created.HasValue())
    {
        m_ahciFailure.DeviceManager = created.GetError();
        PublishAhciFailure(AhciPciRuntimeError::RegionRejected);
        return false;
    }
    m_abarCreation = created.GetValue();

    m_ahciFailure.Capability = m_regionCapabilitySpace.Close(m_abarCreation.Capability);
    if (m_ahciFailure.Capability != Kernel::Object::CapabilityError::Ok)
    {
        PublishAhciFailure(AhciPciRuntimeError::CapabilityCloseRejected);
        return false;
    }

    Kernel::Device::DeviceRegion* const abarRegion = m_abarCreation.Storage;
    m_ahciFailure.MemoryMappedIo = m_abarMemoryMappedIo.Initialize(
        PhysicalMemoryMappedIoRegionView{
            .Data = &abarRegion,
            .Count = 1U,
        },
        physicalAddressBits);
    if (m_ahciFailure.MemoryMappedIo != PhysicalMemoryMappedIoInitializationError::None)
    {
        PublishAhciFailure(AhciPciRuntimeError::MemoryMappedIoRejected);
        return false;
    }
    return true;
}

void PciEcamRuntime::BindAhci(const Platform::Firmware::AhciPciResource& resource,
                              const Foundation::UInt64 maximumPhysicalAddress) noexcept
{
    m_ahciFailure.Binding = m_ahciBinding.Prepare(Kernel::Device::AhciPciBootstrapBindingRequest{
        .MaximumPhysicalAddress = maximumPhysicalAddress,
        .AbarExtentBytes = resource.AbarLengthBytes,
    });
    if (m_ahciFailure.Binding.Error != Kernel::Device::AhciPciBootstrapBindingError::None)
    {
        PublishAhciFailure(AhciPciRuntimeError::BindingRejected);
        return;
    }

    Kernel::Device::AhciPciBootstrapBindingResult binding{};
    if (m_ahciBinding.GetResult(binding) != Kernel::Device::AhciPciBootstrapBindingQueryError::None)
    {
        PublishAhciFailure(AhciPciRuntimeError::BindingQueryRejected);
        return;
    }
    if (!BindingMatchesResource(binding, resource))
    {
        PublishAhciFailure(AhciPciRuntimeError::BindingResultMismatch);
        return;
    }

    m_ahciResult = AhciPciRuntimeResult{
        .Binding = binding,
        .Region = m_abarCreation.Storage->GetSnapshot().Descriptor,
    };
    m_ahciAvailability = AhciPciAvailability::Ready;
}

Hal::Device::PciConfigurationReadError PciEcamRuntime::Read32(const Kernel::Device::PciFunctionAddress address,
                                                              const Foundation::UInt16 registerOffset,
                                                              Foundation::UInt32& output) noexcept
{
    if ((m_state != PciEcamRuntimeState::Ready) && (m_state != PciEcamRuntimeState::Unavailable))
    {
        return Hal::Device::PciConfigurationReadError::NotReady;
    }
    return m_configurationSpace.Read32(address, registerOffset, output);
}

PciEcamFunctionQueryError PciEcamRuntime::GetSummary(PciEcamRuntimeSummary& output) const noexcept
{
    if ((m_state != PciEcamRuntimeState::Ready) && (m_state != PciEcamRuntimeState::Unavailable))
    {
        return PciEcamFunctionQueryError::NotReady;
    }
    output = m_summary;
    return PciEcamFunctionQueryError::None;
}

PciEcamFunctionQueryError PciEcamRuntime::GetPublishedFunction(const Foundation::Size index,
                                                               PciFunctionPublication& output) const noexcept
{
    if (m_state != PciEcamRuntimeState::Ready)
    {
        return PciEcamFunctionQueryError::NotReady;
    }
    if (index >= m_functionCount)
    {
        return PciEcamFunctionQueryError::IndexOutOfRange;
    }
    output = m_functionPublications[index];
    return PciEcamFunctionQueryError::None;
}

PciEcamRuntimeState PciEcamRuntime::GetState() const noexcept
{
    return m_state;
}

PciEcamRuntimeFailure PciEcamRuntime::GetFailure() const noexcept
{
    return m_failure;
}

AhciPciAvailability PciEcamRuntime::GetAhciAvailability() const noexcept
{
    return m_ahciAvailability;
}

AhciPciRuntimeFailure PciEcamRuntime::GetAhciFailure() const noexcept
{
    return m_ahciFailure;
}

AhciPciRuntimeQueryError PciEcamRuntime::GetAhciResult(AhciPciRuntimeResult& output) const noexcept
{
    switch (m_ahciAvailability)
    {
    case AhciPciAvailability::NotEvaluated:
        return AhciPciRuntimeQueryError::NotReady;
    case AhciPciAvailability::EcamUnavailable:
    case AhciPciAvailability::Absent:
        return AhciPciRuntimeQueryError::Unavailable;
    case AhciPciAvailability::Failed:
        return AhciPciRuntimeQueryError::Failed;
    case AhciPciAvailability::Ready:
        output = m_ahciResult;
        return AhciPciRuntimeQueryError::None;
    }
    return AhciPciRuntimeQueryError::Failed;
}

AhciIdentifyRuntimeFailure PciEcamRuntime::MakeAhciIdentifyFailure(const AhciIdentifyRuntimeError error) noexcept
{
    return AhciIdentifyRuntimeFailure{
        .Command = {},
        .Error = error,
        .Reserved = {},
    };
}

AhciIdentifyRuntimeFailure
PciEcamRuntime::MakeAhciIdentifyFailure(const AhciIdentifyRuntimeError error,
                                        const Kernel::Device::AhciIdentifyCommandFailure& command) noexcept
{
    return AhciIdentifyRuntimeFailure{
        .Command = command,
        .Error = error,
        .Reserved = {},
    };
}

bool PciEcamRuntime::HasWritableAhciRegion() const noexcept
{
    if (m_abarCreation.Storage == nullptr)
    {
        return false;
    }
    const Kernel::Device::DeviceRegionSnapshot snapshot = m_abarCreation.Storage->GetSnapshot();
    const Kernel::Device::DeviceRegionDescriptor& region = snapshot.Descriptor;
    const Kernel::Device::PciEndpointBar& abar = m_ahciResult.Binding.Abar;
    return (snapshot.State == Kernel::Device::DeviceRegionState::Ready) &&
           (region.BaseAddress == m_ahciResult.Region.BaseAddress) &&
           (region.LengthBytes == m_ahciResult.Region.LengthBytes) && (region.Kind == m_ahciResult.Region.Kind) &&
           (region.Permissions.GetBits() == m_ahciResult.Region.Permissions.GetBits()) &&
           (region.Widths.GetBits() == m_ahciResult.Region.Widths.GetBits()) &&
           (region.CachePolicy == m_ahciResult.Region.CachePolicy) && (region.BaseAddress == abar.BaseAddress) &&
           (region.LengthBytes == abar.ExtentBytes) &&
           (region.Kind == Kernel::Device::DeviceRegionKind::MemoryMappedIo) &&
           region.Permissions.Contains(Kernel::Device::DeviceRegionPermission::Read) &&
           region.Permissions.Contains(Kernel::Device::DeviceRegionPermission::Write) &&
           region.Widths.Contains(Kernel::Device::DeviceAccessWidth::DoubleWord) &&
           (region.CachePolicy == Kernel::Memory::MemoryCachePolicy::Uncached);
}

AhciIdentifyRuntimeFailure
PciEcamRuntime::ExecuteAhciIdentify(const Hal::Time::IMonotonicClock& clock,
                                    const Kernel::Device::AhciDmaPageWritableView dmaPage,
                                    const Kernel::Device::AhciIdentifyPollingPolicy polling) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_ahciCommandSynchronization};
    if (m_ahciIdentifyState != AhciIdentifyRuntimeState::NotAttempted)
    {
        return MakeAhciIdentifyFailure(AhciIdentifyRuntimeError::AlreadyAttempted, m_ahciIdentifyFailure.Command);
    }
    if (m_ahciAvailability != AhciPciAvailability::Ready)
    {
        m_ahciIdentifyFailure = MakeAhciIdentifyFailure(AhciIdentifyRuntimeError::AhciNotReady);
        m_ahciIdentifyState = AhciIdentifyRuntimeState::Failed;
        return m_ahciIdentifyFailure;
    }
    if (!HasWritableAhciRegion())
    {
        m_ahciIdentifyFailure = MakeAhciIdentifyFailure(AhciIdentifyRuntimeError::RegionAuthorityRejected);
        m_ahciIdentifyState = AhciIdentifyRuntimeState::Failed;
        return m_ahciIdentifyFailure;
    }

    Kernel::Device::AhciIdentifyCommandReceipt candidate{};
    Kernel::Device::AhciIdentifyCommandExecutor executor{m_abarMemoryMappedIo, m_abarMemoryMappedIo, clock};
    const Kernel::Device::PciEndpointBar& abar = m_ahciResult.Binding.Abar;
    m_ahciIdentifyFailure.Command = executor.Execute(
        Kernel::Device::AhciIdentifyCommandRequest{
            .Abar =
                Kernel::Device::AhciHbaRegisterRange{
                    .BaseAddress = abar.BaseAddress,
                    .LengthBytes = abar.ExtentBytes,
                },
            .DmaPage = dmaPage,
            .Polling = polling,
        },
        candidate);
    if (m_ahciIdentifyFailure.Command.Error != Kernel::Device::AhciIdentifyCommandError::None)
    {
        m_ahciIdentifyFailure.Error = AhciIdentifyRuntimeError::CommandRejected;
        m_ahciIdentifyState = AhciIdentifyRuntimeState::Failed;
        return m_ahciIdentifyFailure;
    }

    m_ahciIdentifyReceipt = candidate;
    m_ahciIdentifyFailure = MakeAhciIdentifyFailure(AhciIdentifyRuntimeError::None);
    m_ahciIdentifyState = AhciIdentifyRuntimeState::Succeeded;
    return m_ahciIdentifyFailure;
}

AhciIdentifyRuntimeQueryError
PciEcamRuntime::GetAhciIdentifyReceipt(Kernel::Device::AhciIdentifyCommandReceipt& output) const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_ahciCommandSynchronization};
    if (m_ahciIdentifyState == AhciIdentifyRuntimeState::NotAttempted)
    {
        return AhciIdentifyRuntimeQueryError::NotReady;
    }
    if (m_ahciIdentifyState == AhciIdentifyRuntimeState::Failed)
    {
        return AhciIdentifyRuntimeQueryError::Failed;
    }
    output = m_ahciIdentifyReceipt;
    return AhciIdentifyRuntimeQueryError::None;
}

AhciIdentifyRuntimeState PciEcamRuntime::GetAhciIdentifyState() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_ahciCommandSynchronization};
    return m_ahciIdentifyState;
}

AhciIdentifyRuntimeFailure PciEcamRuntime::GetAhciIdentifyFailure() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_ahciCommandSynchronization};
    return m_ahciIdentifyFailure;
}

AhciReadDmaExtRuntimeFailure PciEcamRuntime::MakeAhciReadDmaExtFailure(const AhciReadDmaExtRuntimeError error) noexcept
{
    return AhciReadDmaExtRuntimeFailure{
        .Command = {},
        .Error = error,
        .Reserved = {},
    };
}

AhciReadDmaExtRuntimeFailure
PciEcamRuntime::MakeAhciReadDmaExtFailure(const AhciReadDmaExtRuntimeError error,
                                          const Kernel::Device::AhciReadDmaExtExecutionFailure& command) noexcept
{
    return AhciReadDmaExtRuntimeFailure{
        .Command = command,
        .Error = error,
        .Reserved = {},
    };
}

AhciReadDmaExtRuntimeFailure PciEcamRuntime::ExecuteAhciReadDmaExt(
    const Hal::Time::IMonotonicClock& clock, const Kernel::Device::AhciReadDmaExtCommandPageWritableView commandPage,
    const Kernel::Device::AhciReadDmaExtDataBufferSpan dataBuffer, const Foundation::UInt64 startingLba,
    const Foundation::UInt64 sectorCount, const Kernel::Device::AhciReadDmaExtPollingPolicy polling) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_ahciCommandSynchronization};
    if (m_ahciReadDmaExtState != AhciReadDmaExtRuntimeState::NotAttempted)
    {
        return MakeAhciReadDmaExtFailure(AhciReadDmaExtRuntimeError::AlreadyAttempted, m_ahciReadDmaExtFailure.Command);
    }
    if (m_ahciIdentifyState != AhciIdentifyRuntimeState::Succeeded)
    {
        m_ahciReadDmaExtFailure = MakeAhciReadDmaExtFailure(AhciReadDmaExtRuntimeError::IdentifyNotSucceeded);
        m_ahciReadDmaExtState = AhciReadDmaExtRuntimeState::Failed;
        return m_ahciReadDmaExtFailure;
    }
    if ((m_ahciAvailability != AhciPciAvailability::Ready) || !HasWritableAhciRegion())
    {
        m_ahciReadDmaExtFailure = MakeAhciReadDmaExtFailure(AhciReadDmaExtRuntimeError::RegionAuthorityRejected);
        m_ahciReadDmaExtState = AhciReadDmaExtRuntimeState::Failed;
        return m_ahciReadDmaExtFailure;
    }

    Kernel::Device::AhciReadDmaExtExecutionReceipt candidate{};
    Kernel::Device::AhciReadDmaExtCommandExecutor executor{m_abarMemoryMappedIo, m_abarMemoryMappedIo, clock};
    const Kernel::Device::PciEndpointBar& abar = m_ahciResult.Binding.Abar;
    const Kernel::Device::AhciIdentifyCommandReceipt& identify = m_ahciIdentifyReceipt;
    m_ahciReadDmaExtFailure.Command = executor.Execute(
        Kernel::Device::AhciReadDmaExtExecutionRequest{
            .Abar =
                Kernel::Device::AhciHbaRegisterRange{
                    .BaseAddress = abar.BaseAddress,
                    .LengthBytes = abar.ExtentBytes,
                },
            .CommandPage = commandPage,
            .DataBuffer = dataBuffer,
            .DeviceSectorCount = identify.Device.SectorCount,
            .StartingLba = startingLba,
            .LogicalSectorSizeBytes = identify.Device.LogicalSectorSizeBytes,
            .SectorCount = sectorCount,
            .Polling = polling,
            .PortNumber = identify.PortNumber,
            .Supports48BitAddressing = identify.Device.Uses48BitAddressing,
            .Reserved = {},
        },
        candidate);
    if (m_ahciReadDmaExtFailure.Command.Error != Kernel::Device::AhciReadDmaExtExecutionError::None)
    {
        m_ahciReadDmaExtFailure.Error = AhciReadDmaExtRuntimeError::CommandRejected;
        m_ahciReadDmaExtState = AhciReadDmaExtRuntimeState::Failed;
        return m_ahciReadDmaExtFailure;
    }

    m_ahciReadDmaExtReceipt = candidate;
    m_ahciReadDmaExtFailure = MakeAhciReadDmaExtFailure(AhciReadDmaExtRuntimeError::None);
    m_ahciReadDmaExtState = AhciReadDmaExtRuntimeState::Succeeded;
    return m_ahciReadDmaExtFailure;
}

AhciReadDmaExtRuntimeQueryError
PciEcamRuntime::GetAhciReadDmaExtReceipt(Kernel::Device::AhciReadDmaExtExecutionReceipt& output) const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_ahciCommandSynchronization};
    if (m_ahciReadDmaExtState == AhciReadDmaExtRuntimeState::NotAttempted)
    {
        return AhciReadDmaExtRuntimeQueryError::NotReady;
    }
    if (m_ahciReadDmaExtState == AhciReadDmaExtRuntimeState::Failed)
    {
        return AhciReadDmaExtRuntimeQueryError::Failed;
    }
    output = m_ahciReadDmaExtReceipt;
    return AhciReadDmaExtRuntimeQueryError::None;
}

AhciReadDmaExtRuntimeState PciEcamRuntime::GetAhciReadDmaExtState() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_ahciCommandSynchronization};
    return m_ahciReadDmaExtState;
}

AhciReadDmaExtRuntimeFailure PciEcamRuntime::GetAhciReadDmaExtFailure() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_ahciCommandSynchronization};
    return m_ahciReadDmaExtFailure;
}
} // namespace Asteria::Platform::Device
