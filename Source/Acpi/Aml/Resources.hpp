// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_ACPI_AML_RESOURCES_HPP_INCLUDED
#define ASTERIA_ACPI_AML_RESOURCES_HPP_INCLUDED

#include "Acpi/Aml/Types.hpp"

namespace Asteria::Acpi::Aml
{
enum class ResourceKind : UInt8
{
    Io,
    FixedIo,
    Irq,
    Dma,
    ExtendedIrq,
    Memory24,
    Memory32,
    FixedMemory32,
    WordAddress,
    DWordAddress,
    QWordAddress,
    ExtendedAddress,
    Vendor,
    End,
    Gpio,
    I2c,
    Spi,
    Uart,
    Pin,
    Dependent,
    Skipped,
};
struct Resource
{
    Bytes Raw{};
    Bytes Source{};
    UInt64 Minimum{0U};
    UInt64 Maximum{0U};
    UInt64 Length{0U};
    UInt64 Alignment{0U};
    UInt64 Translation{0U};
    UInt64 Attributes{0U};
    UInt32 Interrupt{0U};
    UInt16 Flags{0U};
    UInt8 Space{0U};
    UInt8 SourceIndex{0U};
    ResourceKind Kind{ResourceKind::Skipped};
};
struct Resources
{
    Resource Items[MaximumResources]{};
    Size Count{0U};
    Size ErrorOffset{0U};
    Reason Error{Reason::None};
};
/// Decodes ACPI 6.5 section 6.4 resource templates. Borrowed Raw and Source
/// spans remain valid as long as the input. Failure retains a checked prefix
/// and marks the entire template Unknown. No allocation, recursion or I/O.
class ResourceDecoder final
{
public:
    [[nodiscard]] static bool Decode(Bytes bytes, Resources& output) noexcept;
};
[[nodiscard]] const char* ResourceText(ResourceKind kind) noexcept;
} // namespace Asteria::Acpi::Aml

#endif
