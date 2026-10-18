// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_AMD64_INTERRUPT_AMD64_LOCAL_APIC_HPP_INCLUDED
#define ASTERIA_PLATFORM_AMD64_INTERRUPT_AMD64_LOCAL_APIC_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::Platform::Amd64::Interrupt
{
/// CPU-local adapter, used with migration disabled. Initialize observes the
/// enabled firmware mode without changing it. MMIO must be mapped UC in xAPIC
/// mode; x2APIC needs no mapping. Ordinary offsets exclude ICR and self-IPI.
class Amd64LocalApic final
{
public:
    [[nodiscard]] bool Initialize(Foundation::UInt64 mappedAddress) noexcept;
    /// Caller provides an identity UC mapping of the enabled xAPIC base.
    [[nodiscard]] bool InitializeIdentity() noexcept;
    [[nodiscard]] Foundation::UInt32 Read(Foundation::UInt32 offset) const noexcept;
    void Write(Foundation::UInt32 offset, Foundation::UInt32 value) const noexcept;
    [[nodiscard]] Foundation::UInt32 GetId() const noexcept;
    [[nodiscard]] bool Send(Foundation::UInt32 destination, Foundation::UInt32 command) const noexcept;
    // Processor stop policy terminal freeze: do not enter ordinary wait/progress while stopping a
    // lock holder. Busy delivery is reported as unconfirmed.
    [[nodiscard]] bool TrySendNmi(Foundation::UInt32 destination) const noexcept;
    [[nodiscard]] bool SendSelf(Foundation::UInt8 vector) const noexcept;
    [[nodiscard]] constexpr bool IsX2Apic() const noexcept
    {
        return m_x2Apic;
    }
    [[nodiscard]] constexpr bool IsReady() const noexcept
    {
        return m_registers != nullptr || m_x2Apic;
    }
    [[nodiscard]] static constexpr Foundation::UInt32 MsrFor(Foundation::UInt32 offset) noexcept
    {
        return 0x800U + (offset >> 4U);
    }

private:
    volatile Foundation::UInt32* m_registers{nullptr};
    bool m_x2Apic{false};
};
} // Namespace Asteria::Platform::Amd64::Interrupt
#endif
