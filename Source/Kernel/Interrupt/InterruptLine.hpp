// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_INTERRUPT_INTERRUPT_LINE_HPP_INCLUDED
#define ASTERIA_KERNEL_INTERRUPT_INTERRUPT_LINE_HPP_INCLUDED

#include "Kernel/Object/IKernelObject.hpp"

namespace Asteria::Kernel::Interrupt
{
/// Boot-published authority for one platform GSI. The publisher owns the
/// electrical routing policy and keeps this object alive while registered.
class InterruptLine final : public Object::IKernelObject
{
public:
    constexpr InterruptLine(Foundation::UInt32 gsi, Object::ResourceDomainId owner) noexcept
        : m_gsi{gsi}, m_owner{owner}
    {
    }
    [[nodiscard]] Foundation::UInt32 GetLine() const noexcept
    {
        return m_gsi;
    }
    [[nodiscard]] Object::ObjectType GetObjectType() const noexcept override
    {
        return Object::ObjectType::InterruptLine;
    }
    [[nodiscard]] Object::ResourceDomainId GetChargeOwner() const noexcept override
    {
        return m_owner;
    }

private:
    Foundation::UInt32 m_gsi;
    Object::ResourceDomainId m_owner;
};
}
#endif
