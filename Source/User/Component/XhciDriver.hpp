// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_COMPONENT_XHCI_DRIVER_HPP_INCLUDED
#define ASTERIA_USER_COMPONENT_XHCI_DRIVER_HPP_INCLUDED

#include "User/Component/ComponentProgram.hpp"
#include "User/System/SystemCallClient.hpp"

namespace Asteria::User::Component
{
inline constexpr Foundation::UInt32 XhciReadyMethod{0x58484349U};
inline constexpr Foundation::UInt64 XhciInterruptTag{0x58484349U};
/// One assigned xHCI function, its uncached BAR and a confined coherent DMA
/// arena. Root devices only: 16 ports, eight boot interfaces each, up to 16
/// scratchpads. No allocation outside explicit Frame/MemoryObject creation;
/// one thread serializes controller, USB core and broker replies. Refused MSI,
/// BIOS handoff or IOMMU attachment ends the instance for manager quarantine.
/// Interrupt IN has no deadline; a 10ms wake backstop progresses enumeration.
/// The source capability/epoch and reply Endpoint use the PS/2 bootstrap words.
[[nodiscard]] Foundation::UInt64 ServeXhciDevice(const System::SystemCallClient& client,
                                                 const Foundation::UInt64 (&block)[Abi::ComponentBlockWords],
                                                 ComponentClock clock) noexcept;
} // namespace Asteria::User::Component
#endif
