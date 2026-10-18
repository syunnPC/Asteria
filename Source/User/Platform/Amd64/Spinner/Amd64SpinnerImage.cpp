// SPDX-License-Identifier: Apache-2.0

#include "User/Platform/Amd64/System/Amd64SystemCallTransport.hpp"
#include "User/Spinner/SpinnerProgram.hpp"

extern "C" bool AsteriaAmd64SpinnerStateIntact() noexcept;

// The kernel launches the spinner with its DiagnosticChannel and WaitSet
// handles in RDI/RSI and RDX/RCX (Amd64SpinnerImage.S).
extern "C" void AsteriaAmd64SpinnerImageMain(const Asteria::Foundation::UInt64 diagnosticSelector,
                                             const Asteria::Foundation::UInt64 diagnosticAuthenticator,
                                             const Asteria::Foundation::UInt64 waitSetSelector,
                                             const Asteria::Foundation::UInt64 waitSetAuthenticator) noexcept
{
    Asteria::User::Spinner::RunSpinner(Asteria::User::Platform::Amd64::System::GetAmd64SystemCallTransport(),
                                       {diagnosticSelector, diagnosticAuthenticator},
                                       {waitSetSelector, waitSetAuthenticator},
                                       Asteria::User::Spinner::SpinnerIterations, AsteriaAmd64SpinnerStateIntact);
}
