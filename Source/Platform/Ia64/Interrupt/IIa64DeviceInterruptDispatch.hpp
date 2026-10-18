// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_INTERRUPT_I_IA64_DEVICE_INTERRUPT_DISPATCH_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_INTERRUPT_I_IA64_DEVICE_INTERRUPT_DISPATCH_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::Platform::Ia64::Interrupt
{
/// Where a device vector the processor took goes: the PL3 interrupt claim's bridge and the BSP's
/// idle poll hand it here, on the BSP with PSR.i clear, once the vector is no longer in service.
class IIa64DeviceInterruptDispatch
{
public:
    virtual void Dispatch(Foundation::UInt64 vector) noexcept = 0;

protected:
    constexpr IIa64DeviceInterruptDispatch() noexcept = default;
    ~IIa64DeviceInterruptDispatch() noexcept = default;
};
} // Namespace Asteria::Platform::Ia64::Interrupt

#endif // ASTERIA_PLATFORM_IA64_INTERRUPT_I_IA64_DEVICE_INTERRUPT_DISPATCH_HPP_INCLUDED
