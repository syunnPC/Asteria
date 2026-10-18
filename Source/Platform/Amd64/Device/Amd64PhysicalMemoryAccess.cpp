// SPDX-License-Identifier: Apache-2.0

#include "Platform/Amd64/Device/Amd64PhysicalMemoryAccess.hpp"

extern "C" Asteria::Foundation::UInt64
AsteriaAmd64ReadUncachedPhysicalDoubleWord(Asteria::Foundation::UInt64 physicalAddress) noexcept;

extern "C" void AsteriaAmd64WriteUncachedPhysicalDoubleWord(Asteria::Foundation::UInt64 physicalAddress,
                                                            Asteria::Foundation::UInt32 value) noexcept;

// The target entry points are in Amd64MmioWindow.cpp.
#if !defined(ASTERIA_TARGET_AMD64)
extern "C" Asteria::Foundation::UInt64
AsteriaAmd64ReadUncachedPhysicalDoubleWord(const Asteria::Foundation::UInt64 physicalAddress) noexcept
{
    const auto* const pointer = reinterpret_cast<const volatile Asteria::Foundation::UInt32*>(
        static_cast<Asteria::Foundation::UIntPointer>(physicalAddress));
    return *pointer;
}
extern "C" void AsteriaAmd64WriteUncachedPhysicalDoubleWord(const Asteria::Foundation::UInt64 physicalAddress,
                                                            const Asteria::Foundation::UInt32 value) noexcept
{
    auto* const pointer = reinterpret_cast<volatile Asteria::Foundation::UInt32*>(
        static_cast<Asteria::Foundation::UIntPointer>(physicalAddress));
    *pointer = value;
}
#endif

namespace Asteria::Platform::Amd64::Device
{
Foundation::UInt8 Amd64PhysicalMemoryAccess::GetMaximumPhysicalAddressBits() const noexcept
{
    return MaximumPhysicalAddressBits;
}

bool Amd64PhysicalMemoryAccess::IsPhysicalRangeImplemented(const Foundation::UInt64 start,
                                                           const Foundation::UInt64 length,
                                                           const Foundation::UInt8 physicalAddressBits) const noexcept
{
    if ((physicalAddressBits < 32U) || (physicalAddressBits > MaximumPhysicalAddressBits) || (length == 0U))
    {
        return false;
    }
    const Foundation::UInt64 limit = Foundation::UInt64{1U} << physicalAddressBits;
    return (start < limit) && (length <= (limit - start));
}

Foundation::UInt64 Amd64PhysicalMemoryAccess::ReadDoubleWord(const Foundation::UInt64 physicalAddress) const noexcept
{
    return AsteriaAmd64ReadUncachedPhysicalDoubleWord(physicalAddress);
}

void Amd64PhysicalMemoryAccess::WriteDoubleWord(const Foundation::UInt64 physicalAddress,
                                                const Foundation::UInt32 value) const noexcept
{
    AsteriaAmd64WriteUncachedPhysicalDoubleWord(physicalAddress, value);
}
} // namespace Asteria::Platform::Amd64::Device
