// SPDX-License-Identifier: Apache-2.0

#include "User/Spinner/SpinnerProgram.hpp"

namespace Asteria::User::Spinner
{
void RunSpinner(const System::SystemCallTransport transport, const System::CapabilityHandle diagnostic,
                const System::CapabilityHandle waitSet, const Foundation::UInt64 iterations,
                const SpinnerStateCheck stateCheck) noexcept
{
    // A volatile counter keeps every iteration a real load and store.
    volatile Foundation::UInt64 counter{0U};
    while (counter < iterations)
    {
        counter = counter + 1U;
    }
    const System::SystemCallClient client{transport};
    if ((stateCheck != nullptr) && !stateCheck())
    {
        static_cast<void>(client.DebugWrite(
            diagnostic,
            static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(SpinnerStateLostLine)),
            sizeof(SpinnerStateLostLine) - 1U));
    }
    else
    {
        static_cast<void>(client.DebugWrite(
            diagnostic, static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(SpinnerDoneLine)),
            sizeof(SpinnerDoneLine) - 1U));
    }
    // A volatile address keeps the compiler from treating the read as a
    // known-invalid access it may replace or diagnose.
    const volatile Foundation::UIntPointer faultAddress{SpinnerFaultAddress};
    static_cast<void>(*reinterpret_cast<const volatile Foundation::UInt64*>(faultAddress));
    while (client.Wait(waitSet).Error == Abi::SystemCallError::None)
    {
    }
}
} // namespace Asteria::User::Spinner
