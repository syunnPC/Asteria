// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_OBJECT_I_KERNEL_OBJECT_HPP_INCLUDED
#define ASTERIA_KERNEL_OBJECT_I_KERNEL_OBJECT_HPP_INCLUDED

#include "Kernel/Object/CapabilityTypes.hpp"

namespace Asteria::Kernel::Object
{
/// Supplies immutable identity metadata for storage owned outside ObjectRegistry.
/// Implementations and their backing storage must remain alive and immovable from registration
/// until RootCapabilityIssuer returns them through CollectDestroyedObject.
/// Implementations of the metadata accessors must be bounded, nonblocking, allocation-free,
/// callback-free, and safe to call before the object is published.
class IKernelObject
{
public:
    [[nodiscard]] virtual ObjectType GetObjectType() const noexcept = 0;
    [[nodiscard]] virtual ResourceDomainId GetChargeOwner() const noexcept = 0;

protected:
    constexpr IKernelObject() noexcept = default;
    ~IKernelObject() noexcept = default;
};
} // Namespace Asteria::Kernel::Object

#endif // ASTERIA_KERNEL_OBJECT_I_KERNEL_OBJECT_HPP_INCLUDED
