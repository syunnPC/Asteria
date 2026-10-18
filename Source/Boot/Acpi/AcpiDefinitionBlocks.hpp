// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_BOOT_ACPI_ACPI_DEFINITION_BLOCKS_HPP_INCLUDED
#define ASTERIA_BOOT_ACPI_ACPI_DEFINITION_BLOCKS_HPP_INCLUDED

#include "Boot/BootInfo.hpp"

namespace Asteria::Boot::Acpi
{
/// Appends bounded definition-block metadata to a successfully parsed, cleared
/// topology. Synchronous, allocation-free, with immutable pre-exit table bytes.
void RetainAcpiDefinitionBlocks(const BootInfo& info, AcpiTopology& output) noexcept;
} // namespace Asteria::Boot::Acpi

#endif // ASTERIA_BOOT_ACPI_ACPI_DEFINITION_BLOCKS_HPP_INCLUDED
