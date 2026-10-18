// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_ACPI_AML_GRAMMAR_HPP_INCLUDED
#define ASTERIA_ACPI_AML_GRAMMAR_HPP_INCLUDED

#include "Acpi/Aml/Types.hpp"

namespace Asteria::Acpi::Aml
{
/// Argument shapes transcribed from the encodings in ACPI 6.5 section 20;
/// Processor and Unload are accepted legacy encodings from Appendix C.
/// n NameString, e TermArg, q DataRefObject, t SuperName/Target, b/w/d raw
/// integer, l TermList, k PackageElementList, r ByteList, f FieldList.
/// Package length and literal prefixes are handled before these shapes.
[[nodiscard]] const char* ArgumentShape(UInt16 opcode) noexcept;
[[nodiscard]] bool NamespaceDeclaration(UInt16 opcode) noexcept;
[[nodiscard]] bool HasPackageLength(UInt16 opcode) noexcept;
[[nodiscard]] bool NameLead(UInt8 byte) noexcept;
[[nodiscard]] UInt32 Segment(const UInt8* bytes) noexcept;
} // namespace Asteria::Acpi::Aml

#endif
