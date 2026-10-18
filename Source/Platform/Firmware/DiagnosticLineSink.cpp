// SPDX-License-Identifier: Apache-2.0

#include "Platform/Firmware/DiagnosticLineSink.hpp"

#include "Platform/Firmware/Diagnostic.hpp"

#if defined(ASTERIA_TARGET_IA64)
#include "Platform/Ia64/Diagnostics/Ia64Stop.hpp"
#endif
#if defined(ASTERIA_TARGET_AMD64)
#include "Platform/Amd64/Diagnostics/Amd64Stop.hpp"
#endif

namespace Asteria::Platform::Firmware
{
void DiagnosticLineSink::WriteLine(const Foundation::UInt8* const text, const Foundation::Size lengthBytes) noexcept
{
    WriteDiagnosticUserLine(text, lengthBytes);
#if defined(ASTERIA_TARGET_AMD64)
    Amd64::Diagnostics::ObserveAmd64StopPoint(text, lengthBytes);
#elif defined(ASTERIA_TARGET_IA64)
    Ia64::Diagnostics::ObserveIa64StopPoint(text, lengthBytes);
#endif
}
} // namespace Asteria::Platform::Firmware
