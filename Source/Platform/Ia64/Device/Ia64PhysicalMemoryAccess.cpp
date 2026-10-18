// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Device/Ia64PhysicalMemoryAccess.hpp"

extern "C" Asteria::Foundation::UInt64
AsteriaIa64ReadUncachedPhysicalDoubleWord(Asteria::Foundation::UInt64 physicalAddress) noexcept;

extern "C" void AsteriaIa64WriteUncachedPhysicalDoubleWord(Asteria::Foundation::UInt64 physicalAddress,
                                                           Asteria::Foundation::UInt32 value) noexcept;

// The target entry points are in Ia64PhysicalMemoryAccess.S.
#if !defined(__ia64__)
extern "C" Asteria::Foundation::UInt64
AsteriaIa64ReadUncachedPhysicalDoubleWord(const Asteria::Foundation::UInt64 physicalAddress) noexcept
{
    const auto* const address = reinterpret_cast<const volatile Asteria::Foundation::UInt32*>(physicalAddress);
    __asm__ volatile("" ::: "memory");
    const Asteria::Foundation::UInt32 value = *address;
    __asm__ volatile("" ::: "memory");
    return value;
}

extern "C" void AsteriaIa64WriteUncachedPhysicalDoubleWord(const Asteria::Foundation::UInt64 physicalAddress,
                                                           const Asteria::Foundation::UInt32 value) noexcept
{
    auto* const address = reinterpret_cast<volatile Asteria::Foundation::UInt32*>(physicalAddress);
    __asm__ volatile("" ::: "memory");
    *address = value;
    __asm__ volatile("" ::: "memory");
}
#endif

extern "C" Asteria::Foundation::UInt8
AsteriaIa64ReadUncachedPhysicalByte(Asteria::Foundation::UInt64 physicalAddress) noexcept;
extern "C" void AsteriaIa64WriteUncachedPhysicalByte(Asteria::Foundation::UInt64 physicalAddress,
                                                     Asteria::Foundation::UInt8 value) noexcept;
#if !defined(__ia64__)
extern "C" Asteria::Foundation::UInt8
AsteriaIa64ReadUncachedPhysicalByte(const Asteria::Foundation::UInt64 physicalAddress) noexcept
{
    return *reinterpret_cast<const volatile Asteria::Foundation::UInt8*>(physicalAddress);
}
extern "C" void AsteriaIa64WriteUncachedPhysicalByte(const Asteria::Foundation::UInt64 physicalAddress,
                                                     const Asteria::Foundation::UInt8 value) noexcept
{
    *reinterpret_cast<volatile Asteria::Foundation::UInt8*>(physicalAddress) = value;
}
#endif

extern "C" Asteria::Foundation::UInt16
AsteriaIa64ReadUncachedPhysicalWord(Asteria::Foundation::UInt64 physicalAddress) noexcept;
extern "C" void AsteriaIa64WriteUncachedPhysicalWord(Asteria::Foundation::UInt64 physicalAddress,
                                                     Asteria::Foundation::UInt16 value) noexcept;
#if !defined(__ia64__)
extern "C" Asteria::Foundation::UInt16
AsteriaIa64ReadUncachedPhysicalWord(const Asteria::Foundation::UInt64 physicalAddress) noexcept
{
    return *reinterpret_cast<const volatile Asteria::Foundation::UInt16*>(physicalAddress);
}
extern "C" void AsteriaIa64WriteUncachedPhysicalWord(const Asteria::Foundation::UInt64 physicalAddress,
                                                     const Asteria::Foundation::UInt16 value) noexcept
{
    *reinterpret_cast<volatile Asteria::Foundation::UInt16*>(physicalAddress) = value;
}
#endif

extern "C" Asteria::Foundation::UInt64
AsteriaIa64ReadUncachedPhysicalQuadWord(Asteria::Foundation::UInt64 physicalAddress) noexcept;
extern "C" void AsteriaIa64WriteUncachedPhysicalQuadWord(Asteria::Foundation::UInt64 physicalAddress,
                                                         Asteria::Foundation::UInt64 value) noexcept;
#if !defined(__ia64__)
extern "C" Asteria::Foundation::UInt64
AsteriaIa64ReadUncachedPhysicalQuadWord(const Asteria::Foundation::UInt64 physicalAddress) noexcept
{
    return *reinterpret_cast<const volatile Asteria::Foundation::UInt64*>(physicalAddress);
}
extern "C" void AsteriaIa64WriteUncachedPhysicalQuadWord(const Asteria::Foundation::UInt64 physicalAddress,
                                                         const Asteria::Foundation::UInt64 value) noexcept
{
    *reinterpret_cast<volatile Asteria::Foundation::UInt64*>(physicalAddress) = value;
}
#endif

namespace Asteria::Platform::Ia64::Device
{
namespace
{
constexpr Foundation::UInt64 PhysicalUncachedAliasBit{Foundation::UInt64{1U} << 63U};
} // namespace

Foundation::UInt8 Ia64PhysicalMemoryAccess::GetMaximumPhysicalAddressBits() const noexcept
{
    return MaximumPhysicalAddressBits;
}

bool Ia64PhysicalMemoryAccess::IsPhysicalRangeImplemented(const Foundation::UInt64 start,
                                                          const Foundation::UInt64 length,
                                                          const Foundation::UInt8 physicalAddressBits) const noexcept
{
    if ((physicalAddressBits < 32U) || (physicalAddressBits > MaximumPhysicalAddressBits) ||
        ((start & PhysicalUncachedAliasBit) != 0U))
    {
        return false;
    }
    if (physicalAddressBits == MaximumPhysicalAddressBits)
    {
        return length <= (PhysicalUncachedAliasBit - start);
    }
    const Foundation::UInt64 limit = Foundation::UInt64{1U} << physicalAddressBits;
    return (start < limit) && (length <= (limit - start));
}

Foundation::UInt64 Ia64PhysicalMemoryAccess::ReadDoubleWord(const Foundation::UInt64 physicalAddress) const noexcept
{
    return AsteriaIa64ReadUncachedPhysicalDoubleWord(physicalAddress);
}

void Ia64PhysicalMemoryAccess::WriteDoubleWord(const Foundation::UInt64 physicalAddress,
                                               const Foundation::UInt32 value) const noexcept
{
    AsteriaIa64WriteUncachedPhysicalDoubleWord(physicalAddress, value);
}
Foundation::UInt8 Ia64PhysicalMemoryAccess::ReadByte(const Foundation::UInt64 physicalAddress) const noexcept
{
    return AsteriaIa64ReadUncachedPhysicalByte(physicalAddress);
}

void Ia64PhysicalMemoryAccess::WriteByte(const Foundation::UInt64 physicalAddress,
                                         const Foundation::UInt8 value) const noexcept
{
    AsteriaIa64WriteUncachedPhysicalByte(physicalAddress, value);
}

Foundation::UInt16 Ia64PhysicalMemoryAccess::ReadWord(const Foundation::UInt64 physicalAddress) const noexcept
{
    return AsteriaIa64ReadUncachedPhysicalWord(physicalAddress);
}

Foundation::UInt64 Ia64PhysicalMemoryAccess::ReadQuadWord(const Foundation::UInt64 physicalAddress) const noexcept
{
    return AsteriaIa64ReadUncachedPhysicalQuadWord(physicalAddress);
}

void Ia64PhysicalMemoryAccess::WriteQuadWord(const Foundation::UInt64 physicalAddress,
                                             const Foundation::UInt64 value) const noexcept
{
    AsteriaIa64WriteUncachedPhysicalQuadWord(physicalAddress, value);
}

void Ia64PhysicalMemoryAccess::WriteWord(const Foundation::UInt64 physicalAddress,
                                         const Foundation::UInt16 value) const noexcept
{
    AsteriaIa64WriteUncachedPhysicalWord(physicalAddress, value);
}

} // namespace Asteria::Platform::Ia64::Device
