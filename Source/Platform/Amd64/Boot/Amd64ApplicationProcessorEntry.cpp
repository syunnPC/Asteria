// SPDX-License-Identifier: Apache-2.0

#include "Foundation/Types.hpp"

extern "C" [[noreturn]] void AsteriaAmd64RunApplicationProcessor(void* context,
                                                                 Asteria::Foundation::UInt64 processor) noexcept;

// A separate link-time entry unit allows firmware profiles to prepare an AP
// before entering the common runtime; the runtime independently checks mode.
extern "C" [[noreturn]] void AsteriaAmd64ApplicationProcessorEntry(void* const context,
                                                                   const Asteria::Foundation::UInt64 processor) noexcept
{
    AsteriaAmd64RunApplicationProcessor(context, processor);
}
