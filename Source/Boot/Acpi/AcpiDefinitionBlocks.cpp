// SPDX-License-Identifier: Apache-2.0

#include "Boot/Acpi/AcpiDefinitionBlocks.hpp"

#include "Boot/Acpi/AcpiTableParser.hpp"

namespace Asteria::Boot::Acpi
{
namespace
{
[[nodiscard]] bool SignatureIs(const char* signature, const char* expected, const Foundation::Size length) noexcept
{
    for (Foundation::Size index{0U}; index < length; ++index)
    {
        if (signature[index] != expected[index])
        {
            return false;
        }
    }
    return true;
}

class DefinitionVisitor final : public IAcpiTableVisitor
{
public:
    AcpiTopology* Output{nullptr};
    Foundation::UInt32 FadtCount{0U};
    void Visit(const AcpiTableRecord& record, const Foundation::UInt8* bytes) noexcept override;
};

void DefinitionVisitor::Visit(const AcpiTableRecord& record, const Foundation::UInt8* bytes) noexcept
{
    auto& output = *Output;
    if (SignatureIs(record.Signature, "FACP", 4U))
    {
        ++FadtCount;
    }
    if (SignatureIs(record.Signature, "DSDT", 4U) && output.Dsdt.Length == 0U)
    {
        output.Dsdt = record;
    }
    else if (SignatureIs(record.Signature, "SSDT", 4U))
    {
        if (output.SsdtCount < MaximumAcpiSsdts)
        {
            output.Ssdts[output.SsdtCount++] = record;
        }
        else
        {
            ++output.SsdtOverflowCount;
        }
    }
    else if (SignatureIs(record.Signature, "FACP", 4U) && record.ChecksumValid != 0U)
    {
        output.RawFadtRevision = record.Revision;
        if (record.Length >= 111U)
        {
            output.RawIapcBootArchitecture = static_cast<Foundation::UInt16>(
                static_cast<Foundation::UInt16>(bytes[109U]) |
                static_cast<Foundation::UInt16>(static_cast<Foundation::UInt16>(bytes[110U]) << 8U));
        }
    }
}

} // namespace

void RetainAcpiDefinitionBlocks(const BootInfo& info, AcpiTopology& output) noexcept
{
    DefinitionVisitor definitions{};
    definitions.Output = &output;
    AcpiTableParser::VisitTables(info, output, definitions);
    if (definitions.FadtCount != 1U)
    {
        output.Dsdt = {};
        output.RawFadtRevision = 0U;
        output.RawIapcBootArchitecture = 0U;
    }
}

} // namespace Asteria::Boot::Acpi
