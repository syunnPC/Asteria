// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Memory/InitialTranslationActivator.hpp"

extern "C" Asteria::Foundation::UInt64 AsteriaIa64ActivateInitialTranslation() noexcept;

extern "C" [[noreturn]] void AsteriaIa64RunInitialTranslatedKernel() noexcept;

// The target entry points are in InitialTranslationActivator.S.

namespace Asteria::Platform::Ia64::Memory
{
namespace
{
constexpr Foundation::UInt64 AdapterUnsupportedProcessorState{1U};
}

InitialTranslationActivationError InitialTranslationActivator::Activate() noexcept
{
    const Foundation::UInt64 adapterStatus = AsteriaIa64ActivateInitialTranslation();
    if (adapterStatus == AdapterUnsupportedProcessorState)
    {
        return InitialTranslationActivationError::UnsupportedProcessorState;
    }
    return InitialTranslationActivationError::UnexpectedAdapterReturn;
}
} // namespace Asteria::Platform::Ia64::Memory
