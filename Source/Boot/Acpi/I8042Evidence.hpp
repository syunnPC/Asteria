// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_BOOT_ACPI_I8042_EVIDENCE_HPP_INCLUDED
#define ASTERIA_BOOT_ACPI_I8042_EVIDENCE_HPP_INCLUDED

#include "Abi/I8042.hpp"
#include "Boot/Acpi/AcpiTopology.hpp"

namespace Asteria::Boot::Acpi
{
/// Reserved ACPI topology storage, zero means no AML evidence. Address bytes are
/// little-endian: DefinitionReserved[0..4], TimerReserved[0..2]. No native
/// pointers or layout changes. TrailingReserved identifies the keyboard PNP ID.
/// Reserved holds the declared-device bits and the IRQ 1/12 signaling bits
/// (Abi::I8042AmlEvidenceMask); the kernel publishes only the former.
class I8042Evidence final
{
public:
    static void Record(AcpiTopology& topology, const Foundation::UInt32 flags, const Foundation::UInt32 identifier,
                       const Foundation::UInt8 checksum, const Foundation::UInt64 address,
                       const Foundation::UInt32 length) noexcept
    {
        if ((flags & Abi::I8042KeyboardAml) == 0U)
        {
            return;
        }
        topology.Reserved = flags & static_cast<Foundation::UInt32>(Abi::I8042AmlEvidenceMask);
        topology.TrailingReserved = identifier;
        topology.FadtPadding = length;
        topology.FadtReserved = checksum;
        for (Foundation::Size byte{0U}; byte < 8U; ++byte)
        {
            const Foundation::UInt8 value = static_cast<Foundation::UInt8>((address >> (byte * 8U)) & 255U);
            if (byte < 5U)
            {
                topology.DefinitionReserved[byte] = value;
            }
            else
            {
                topology.TimerReserved[byte - 5U] = value;
            }
        }
    }
    [[nodiscard]] static bool Matches(const AcpiTopology& topology, const Foundation::UInt64 address,
                                      const Foundation::UInt32 length, const Foundation::UInt8 checksum) noexcept
    {
        Foundation::UInt64 boundAddress{0U};
        for (Foundation::Size byte{0U}; byte < 8U; ++byte)
        {
            const Foundation::UInt8 value =
                byte < 5U ? topology.DefinitionReserved[byte] : topology.TimerReserved[byte - 5U];
            boundAddress |= Foundation::UInt64{value} << (byte * 8U);
        }
        const Foundation::UInt32 id = topology.TrailingReserved;
        return (topology.Reserved & Abi::I8042KeyboardAml) != 0U &&
               (topology.Reserved & ~static_cast<Foundation::UInt32>(Abi::I8042AmlEvidenceMask)) == 0U &&
               ((id >= 0x0300U && id <= 0x030BU) || id == 0x0320U) && address != 0U && address == boundAddress &&
               length >= 36U && length == topology.FadtPadding && checksum == topology.FadtReserved;
    }
};
static_assert(__builtin_offsetof(AcpiTopology, Reserved) == 60U);
static_assert(__builtin_offsetof(AcpiTopology, TrailingReserved) == 628U);
static_assert(__builtin_offsetof(AcpiTopology, FadtReserved) == 1499U);
static_assert(__builtin_offsetof(AcpiTopology, FadtPadding) == 1500U);
static_assert(__builtin_offsetof(AcpiTopology, TimerReserved) == 2490U);
static_assert(__builtin_offsetof(AcpiTopology, DefinitionReserved) == 3923U);
}
#endif
