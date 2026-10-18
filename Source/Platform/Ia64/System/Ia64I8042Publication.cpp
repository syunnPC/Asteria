// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/System/Ia64PagerRuntime.hpp"

#include "Kernel/Boot/I8042Publication.hpp"

// Target images define placement new once (Kernel/Boot/KernelEntry.cpp).
#if defined(ASTERIA_TARGET_IA64) || defined(ASTERIA_TARGET_AMD64)
void* operator new(Asteria::Foundation::Size, void*) noexcept;
#else
#include <new>
#endif

namespace Asteria::Platform::Ia64::System
{
namespace I8042 = ::Asteria::Boot::Bootstrap;

// Boot-only publication owns four objects for the lifetime of the pager.
// A failed optional publication revokes every partial capability and leaves
// all four start-block handles zero; no controller commands are issued. The
// ISA line policies it may have recorded stay unused: publication runs once.
void Ia64PagerRuntime::PublishI8042(const Ia64PagerRuntimeInput& input) noexcept
{
    I8042::I8042Publication publication{};
    const auto* const topology = input.I8042Topology;
    if (topology != nullptr)
    {
        publication =
            I8042::GateI8042(topology->IapcBootArchitecture, topology->Reserved, input.I8042DsdtMatches, m_ports);
        // Without the firmware's port window the status read cannot reach a
        // controller: a missing resource, not a floating port.
        if ((publication.Reason == I8042::I8042PublicationReason::FloatingPort) && !m_ports.IsConfigured())
        {
            publication.Reason = I8042::I8042PublicationReason::Resources;
        }
    }
    if (publication.Reason == I8042::I8042PublicationReason::Published)
    {
        publication.Reason = I8042::I8042PublicationReason::Resources;
        bool ready = m_domains && (m_deviceInterrupts != nullptr);
        for (Foundation::Size index{0U}; ready && (index < 2U); ++index)
        {
            // The evidence's trigger and polarity, ISA edge/high by default.
            const auto route = I8042::ResolveI8042Route(*topology, index == 0U ? 1U : 12U, publication.AmlInterrupts);
            ready = input.DeviceInterrupts->SetIsaPolicy(
                route.Gsi,
                route.ActiveLow ? Interrupt::Ia64LinePolarity::ActiveLow : Interrupt::Ia64LinePolarity::ActiveHigh,
                route.Level ? Interrupt::Ia64LineTrigger::Level : Interrupt::Ia64LineTrigger::Edge);
        }
        for (Foundation::Size index{0U}; ready && (index < 2U); ++index)
        {
            const Kernel::Device::DeviceRegionDescriptor descriptor{
                .BaseAddress = index == 0U ? 0x60U : 0x64U,
                .LengthBytes = 1U,
                .Kind = Kernel::Device::DeviceRegionKind::PortIo,
                .Permissions =
                    Kernel::Device::DeviceRegionPermissions{Kernel::Device::DeviceRegionPermission::Read}.With(
                        Kernel::Device::DeviceRegionPermission::Write),
                .Widths = Kernel::Device::DeviceAccessWidths{Kernel::Device::DeviceAccessWidth::Byte},
                .CachePolicy = Kernel::Memory::MemoryCachePolicy::Uncached,
                .Reserved = {},
            };
            const auto policy =
                index == 0U ? Kernel::Device::DevicePortWritePolicy{}
                            : Kernel::Device::DevicePortWritePolicy{.Lists = I8042::I8042CommandWrites, .Count = 1U};
            ready = m_i8042Ports[index]
                        .Initialize(*m_collaborators.Registry, m_collaborators.ResourceIdentity,
                                    *m_collaborators.Resources, descriptor, policy)
                        .Error == Kernel::Device::DeviceRegionError::None;
            if (!ready)
            {
                break;
            }
            const auto region = m_collaborators.Issuer->CreateObject(m_i8042Ports[index]);
            if (!region.HasValue())
            {
                ready = false;
                break;
            }
            m_i8042[index] = region.GetValue();
            const auto route = I8042::ResolveI8042Route(*topology, index == 0U ? 1U : 12U, publication.AmlInterrupts);
            auto* const line = ::new (static_cast<void*>(m_i8042LineStorage[index]))
                Kernel::Interrupt::InterruptLine{route.Gsi, m_collaborators.Resources->GetIdentifier()};
            const auto registered = m_collaborators.Issuer->CreateObject(*line);
            if (!registered.HasValue())
            {
                ready = false;
                break;
            }
            m_i8042[index + 2U] = registered.GetValue();
        }
        for (Foundation::Size index{0U}; ready && (index < 4U); ++index)
        {
            const auto rights = index < 2U
                                    ? Kernel::Object::CapabilityRights{Kernel::Object::CapabilityRight::Read}.With(
                                          Kernel::Object::CapabilityRight::Write)
                                    : Kernel::Object::CapabilityRights{Kernel::Object::CapabilityRight::Invoke};
            ready = MintInto(Abi::PagerBootstrapI8042DataWord + index * 2U, m_i8042[index],
                             rights.With(Kernel::Object::CapabilityRight::Inspect)
                                 .With(Kernel::Object::CapabilityRight::Duplicate)
                                 .With(Kernel::Object::CapabilityRight::Transfer));
        }
        if (ready)
        {
            publication.Reason = I8042::I8042PublicationReason::Published;
            m_startBlock[Abi::PagerBootstrapI8042DeclaredWord] = publication.Declared;
        }
    }
    if (publication.Reason != I8042::I8042PublicationReason::Published)
    {
        for (Foundation::Size index{0U}; index < 4U; ++index)
        {
            const Foundation::Size word = Abi::PagerBootstrapI8042DataWord + index * 2U;
            if (m_startBlock[word] != 0U)
            {
                static_cast<void>(
                    m_space.Close(Kernel::Object::CapabilityHandle{m_startBlock[word], m_startBlock[word + 1U]}));
            }
        }
        for (auto& region : m_i8042Ports)
        {
            static_cast<void>(region.Quiesce());
        }
        for (auto& identity : m_i8042)
        {
            if (identity.IsValid())
            {
                static_cast<void>(m_collaborators.Issuer->DestroyObject(identity));
                static_cast<void>(m_collaborators.Issuer->CollectDestroyedObject(identity));
                identity = {};
            }
        }
        for (Foundation::Size index{0U}; index < 8U; ++index)
        {
            m_startBlock[Abi::PagerBootstrapI8042DataWord + index] = 0U;
        }
        publication.Declared = 0U;
    }
    I8042::ReportI8042(publication, topology == nullptr ? 0U : topology->TrailingReserved);
}
}
