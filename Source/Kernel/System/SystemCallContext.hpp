// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_SYSTEM_SYSTEM_CALL_CONTEXT_HPP_INCLUDED
#define ASTERIA_KERNEL_SYSTEM_SYSTEM_CALL_CONTEXT_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Kernel/Memory/FaultToken.hpp"
#include "Kernel/Object/CapabilityTypes.hpp"
#include "Kernel/Protection/ProtectionDomain.hpp"

namespace Asteria::Kernel::Memory
{
class IPagerResolution;
class IUserMemoryAccess;
}

namespace Asteria::Kernel::Object
{
class CapabilitySpace;
}

namespace Asteria::Kernel::System
{
class IObjectManagement;
class SystemCallScratch;

/// Authority and bounded storage derived by SystemCallIngress for one dispatch.
/// User-controlled register arguments never select any of these collaborators.
struct SystemCallContext
{
    Object::CapabilitySpace* Capabilities{nullptr};
    Memory::IUserMemoryAccess* UserMemory{nullptr};
    SystemCallScratch* Scratch{nullptr};
    Object::ResourceDomainId ResourceDomain{};
    Memory::AddressSpaceId AddressSpace{};
    /// The pager resolution this caller may use, or null: FaultResolve then
    /// completes with InvalidConfiguration.
    Memory::IPagerResolution* PagerResolution{nullptr};
    /// The object management ObjectCreate and ObjectDestroy reach, or null: they then complete with
    /// InvalidConfiguration. Authority comes from the caller's capabilities, not from this pointer.
    IObjectManagement* ObjectManagement{nullptr};
    /// The caller's ProtectionDomain, which DeviceControl's assignee rule names.
    Protection::ProtectionDomainId ProtectionDomain{};
};

static_assert(sizeof(SystemCallContext) == 80U);
static_assert(alignof(SystemCallContext) == 8U);
} // Namespace Asteria::Kernel::System

#endif // ASTERIA_KERNEL_SYSTEM_SYSTEM_CALL_CONTEXT_HPP_INCLUDED
