// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Foundation/Types.hpp"
#include "Platform/Ia64/Firmware/SalSystemTableParser.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Platform::Ia64::Firmware::SalSystemTableHeaderSize;
using Platform::Ia64::Firmware::SalSystemTableInformation;
using Platform::Ia64::Firmware::SalSystemTableParseError;
using Platform::Ia64::Firmware::SalSystemTableParser;

constexpr Foundation::Size FixtureCapacity{512U};
constexpr Foundation::Size EntrypointOffset{SalSystemTableHeaderSize};
constexpr Foundation::Size FirstMemoryOffset{EntrypointOffset + 48U};
constexpr Foundation::Size SecondMemoryOffset{FirstMemoryOffset + 32U};
constexpr Foundation::Size FeaturesOffset{SecondMemoryOffset + 32U};
constexpr Foundation::Size TranslationRegisterOffset{FeaturesOffset + 16U};
constexpr Foundation::Size PurgeCoherenceOffset{TranslationRegisterOffset + 32U};
constexpr Foundation::Size ApWakeOffset{PurgeCoherenceOffset + 16U};

void WriteLittleEndian16(Foundation::UInt8* const bytes, const Foundation::UInt16 value) noexcept
{
    bytes[0] = static_cast<Foundation::UInt8>(value & 0xFFU);
    bytes[1] = static_cast<Foundation::UInt8>(value >> 8U);
}

void WriteLittleEndian32(Foundation::UInt8* const bytes, const Foundation::UInt32 value) noexcept
{
    for (Foundation::UInt8 index{0U}; index < 4U; ++index)
    {
        bytes[index] = static_cast<Foundation::UInt8>(value >> static_cast<Foundation::UInt8>(index * 8U));
    }
}

void WriteLittleEndian64(Foundation::UInt8* const bytes, const Foundation::UInt64 value) noexcept
{
    for (Foundation::UInt8 index{0U}; index < 8U; ++index)
    {
        bytes[index] = static_cast<Foundation::UInt8>(value >> static_cast<Foundation::UInt8>(index * 8U));
    }
}

class SalTableFixture final
{
public:
    SalTableFixture() noexcept
    {
        m_bytes[0] = static_cast<Foundation::UInt8>('S');
        m_bytes[1] = static_cast<Foundation::UInt8>('S');
        m_bytes[2] = static_cast<Foundation::UInt8>('T');
        m_bytes[3] = static_cast<Foundation::UInt8>('_');
        WriteLittleEndian16(m_bytes + 8U, 0x0340U);
        WriteLittleEndian16(m_bytes + 20U, 0x0100U);
        WriteLittleEndian16(m_bytes + 22U, 0x0100U);
        m_bytes[24] = static_cast<Foundation::UInt8>('Q');
        m_bytes[25] = static_cast<Foundation::UInt8>('E');
        m_bytes[26] = static_cast<Foundation::UInt8>('M');
        m_bytes[27] = static_cast<Foundation::UInt8>('U');
        m_bytes[56] = static_cast<Foundation::UInt8>('I');
        m_bytes[57] = static_cast<Foundation::UInt8>('A');
        m_bytes[58] = static_cast<Foundation::UInt8>('-');
        m_bytes[59] = static_cast<Foundation::UInt8>('6');
        m_bytes[60] = static_cast<Foundation::UInt8>('4');
    }

    void AddEntrypoint(const Foundation::UInt64 palProcedure = 0x0010'0000U,
                       const Foundation::UInt64 salProcedure = 0x0020'0000U,
                       const Foundation::UInt64 salGlobalData = 0x0030'0000U) noexcept
    {
        Foundation::UInt8* const descriptor = AddDescriptor(0U, 48U);
        WriteLittleEndian64(descriptor + 8U, palProcedure);
        WriteLittleEndian64(descriptor + 16U, salProcedure);
        WriteLittleEndian64(descriptor + 24U, salGlobalData);
    }

    void AddMemory() noexcept
    {
        static_cast<void>(AddDescriptor(1U, 32U));
    }

    void AddFeatures(const Foundation::UInt8 features = 0x0FU) noexcept
    {
        Foundation::UInt8* const descriptor = AddDescriptor(2U, 16U);
        descriptor[1] = features;
    }

    void AddTranslationRegister(const Foundation::UInt64 virtualAddress = 0U,
                                const Foundation::UInt8 pageShift = 22U) noexcept
    {
        Foundation::UInt8* const descriptor = AddDescriptor(3U, 32U);
        WriteLittleEndian64(descriptor + 8U, virtualAddress);
        WriteLittleEndian64(descriptor + 16U, static_cast<Foundation::UInt64>(pageShift) << 2U);
    }

    void AddPurgeCoherence(const Foundation::UInt32 domainCount = 2U,
                           const Foundation::UInt64 informationAddress = 0x0040'0000U) noexcept
    {
        Foundation::UInt8* const descriptor = AddDescriptor(4U, 16U);
        WriteLittleEndian32(descriptor + 4U, domainCount);
        WriteLittleEndian64(descriptor + 8U, informationAddress);
    }

    void AddApWake(const Foundation::UInt64 vector = 0xFFU) noexcept
    {
        Foundation::UInt8* const descriptor = AddDescriptor(5U, 16U);
        WriteLittleEndian64(descriptor + 8U, vector);
    }

    void AddOpaque(const Foundation::UInt8 type, const Foundation::Size size) noexcept
    {
        static_cast<void>(AddDescriptor(type, size));
    }

    void Finalize() noexcept
    {
        WriteLittleEndian32(m_bytes + 4U, static_cast<Foundation::UInt32>(m_length));
        WriteLittleEndian16(m_bytes + 10U, m_entryCount);
        RepairChecksum();
    }

    void RepairChecksum() noexcept
    {
        m_bytes[12] = 0U;
        Foundation::UInt8 checksum{0U};
        for (Foundation::Size index{0U}; index < GetDeclaredLength(); ++index)
        {
            checksum = static_cast<Foundation::UInt8>(checksum + m_bytes[index]);
        }
        m_bytes[12] = static_cast<Foundation::UInt8>(0U - checksum);
    }

    void SetDeclaredLength(const Foundation::UInt32 length) noexcept
    {
        WriteLittleEndian32(m_bytes + 4U, length);
    }

    void SetEntryCount(const Foundation::UInt16 count) noexcept
    {
        WriteLittleEndian16(m_bytes + 10U, count);
    }

    [[nodiscard]] Foundation::UInt8* Bytes() noexcept
    {
        return m_bytes;
    }

    [[nodiscard]] const Foundation::UInt8* Bytes() const noexcept
    {
        return m_bytes;
    }

    [[nodiscard]] Foundation::Size Length() const noexcept
    {
        return m_length;
    }

private:
    [[nodiscard]] Foundation::UInt8* AddDescriptor(const Foundation::UInt8 type, const Foundation::Size size) noexcept
    {
        Foundation::UInt8* const descriptor = m_bytes + m_length;
        descriptor[0] = type;
        m_length += size;
        ++m_entryCount;
        return descriptor;
    }

    [[nodiscard]] Foundation::Size GetDeclaredLength() const noexcept
    {
        return static_cast<Foundation::Size>(m_bytes[4]) | (static_cast<Foundation::Size>(m_bytes[5]) << 8U) |
               (static_cast<Foundation::Size>(m_bytes[6]) << 16U) | (static_cast<Foundation::Size>(m_bytes[7]) << 24U);
    }

    Foundation::UInt8 m_bytes[FixtureCapacity]{};
    Foundation::Size m_length{SalSystemTableHeaderSize};
    Foundation::UInt16 m_entryCount{0U};
};

[[nodiscard]] SalTableFixture MakeEntrypointTable() noexcept
{
    SalTableFixture fixture;
    fixture.AddEntrypoint();
    fixture.Finalize();
    return fixture;
}

[[nodiscard]] SalTableFixture MakeFullTable() noexcept
{
    SalTableFixture fixture;
    fixture.AddEntrypoint();
    fixture.AddMemory();
    fixture.AddMemory();
    fixture.AddFeatures();
    fixture.AddTranslationRegister();
    fixture.AddPurgeCoherence();
    fixture.AddApWake();
    fixture.Finalize();
    return fixture;
}

[[nodiscard]] bool HasError(const SalTableFixture& fixture, const SalSystemTableParseError expected,
                            const Foundation::Size availableBytes = FixtureCapacity) noexcept
{
    const auto result = SalSystemTableParser::Parse(fixture.Bytes(), availableBytes);
    return !result.HasValue() && (result.GetError() == expected);
}

void TestExactDecode(TestContext& runner) noexcept
{
    const SalTableFixture fixture = MakeFullTable();
    const auto result = SalSystemTableParser::Parse(fixture.Bytes(), fixture.Length());
    if (!result.HasValue())
    {
        ASTERIA_EXPECT(runner, "the complete SAL fixture parses successfully", false);
        return;
    }
    const SalSystemTableInformation& information = result.GetValue();
    ASTERIA_EXPECT(
        runner, "all validated SAL fields decode exactly", information.PalProcedurePhysicalAddress == 0x0010'0000U,
        information.SalProcedurePhysicalAddress == 0x0020'0000U,
        information.SalGlobalDataPhysicalAddress == 0x0030'0000U, information.TranslationRegisterVirtualAddress == 0U,
        information.TranslationRegisterEncodedPageSize == (22U << 2U),
        information.CoherenceDomainInformationPhysicalAddress == 0x0040'0000U, information.ApWakeVector == 0xFFU,
        information.TotalTableLength == fixture.Length(), information.CoherenceDomainCount == 2U,
        information.SalRevision == 0x0340U, information.EntryCount == 7U, information.SalAVersion == 0x0100U,
        information.SalBVersion == 0x0100U, information.MemoryDescriptorCount == 2U,
        information.PlatformFeatures == 0x0FU, information.HasPlatformFeatures, information.HasTranslationRegister,
        information.HasPurgeCoherenceInformation, information.HasApWakeVector, information.Reserved8 == 0U);

    const SalTableFixture minimum = MakeEntrypointTable();
    const auto minimumResult = SalSystemTableParser::Parse(minimum.Bytes(), FixtureCapacity);
    ASTERIA_EXPECT(runner, "the entrypoint-only table and a larger readable extent are accepted",
                   minimumResult.HasValue(), minimumResult.GetValue().EntryCount == 1U,
                   minimumResult.GetValue().MemoryDescriptorCount == 0U, !minimumResult.GetValue().HasPlatformFeatures,
                   !minimumResult.GetValue().HasTranslationRegister,
                   !minimumResult.GetValue().HasPurgeCoherenceInformation, !minimumResult.GetValue().HasApWakeVector);
}

void TestHeaderValidation(TestContext& runner) noexcept
{
    ASTERIA_EXPECT(runner, "a null zero-length buffer is rejected",
                   SalSystemTableParser::Parse(nullptr, 0U).GetError() == SalSystemTableParseError::NullBuffer);
    ASTERIA_EXPECT(runner, "a null nonzero-length buffer is rejected",
                   SalSystemTableParser::Parse(nullptr, SalSystemTableHeaderSize).GetError() ==
                       SalSystemTableParseError::NullBuffer);

    const SalTableFixture full = MakeFullTable();
    bool allHeaderPrefixesRejected{true};
    for (Foundation::Size size{0U}; size < SalSystemTableHeaderSize; ++size)
    {
        const auto result = SalSystemTableParser::Parse(full.Bytes(), size);
        allHeaderPrefixesRejected = allHeaderPrefixesRejected && !result.HasValue() &&
                                    (result.GetError() == SalSystemTableParseError::HeaderTooShort);
    }
    ASTERIA_EXPECT(runner, "every truncated header prefix is rejected before field reads", allHeaderPrefixesRejected);

    bool everySignatureByteChecked{true};
    for (Foundation::Size index{0U}; index < 4U; ++index)
    {
        SalTableFixture fixture = MakeFullTable();
        fixture.Bytes()[index] ^= 1U;
        everySignatureByteChecked =
            everySignatureByteChecked && HasError(fixture, SalSystemTableParseError::InvalidSignature);
    }
    ASTERIA_EXPECT(runner, "all four signature bytes are checked", everySignatureByteChecked);

    SalTableFixture fixture = MakeEntrypointTable();
    fixture.SetDeclaredLength(95U);
    ASTERIA_EXPECT(runner, "a declared length below the entry-count minimum is rejected",
                   HasError(fixture, SalSystemTableParseError::InvalidTableLength));
    fixture = MakeFullTable();
    fixture.SetDeclaredLength(500U);
    ASTERIA_EXPECT(runner, "a declared length above the entry-count maximum is rejected",
                   HasError(fixture, SalSystemTableParseError::InvalidTableLength));
    fixture = MakeFullTable();
    ASTERIA_EXPECT(runner, "a declared table extending beyond the readable extent is rejected",
                   HasError(fixture, SalSystemTableParseError::TruncatedTable, fixture.Length() - 1U));

    bool everyRevisionByteChecked{true};
    constexpr Foundation::Size RevisionOffsets[]{8U, 9U, 20U, 21U, 22U, 23U};
    for (const Foundation::Size offset : RevisionOffsets)
    {
        fixture = MakeFullTable();
        fixture.Bytes()[offset] = 0xFAU;
        fixture.RepairChecksum();
        everyRevisionByteChecked =
            everyRevisionByteChecked && HasError(fixture, SalSystemTableParseError::InvalidRevisionEncoding);
    }
    ASTERIA_EXPECT(runner, "every SAL and firmware BCD revision byte is validated", everyRevisionByteChecked);

    fixture = MakeFullTable();
    WriteLittleEndian16(fixture.Bytes() + 8U, 0x0270U);
    fixture.RepairChecksum();
    ASTERIA_EXPECT(runner, "SAL 2.7 is rejected as older than the parsed descriptor contract",
                   HasError(fixture, SalSystemTableParseError::UnsupportedRevision));
    fixture = MakeFullTable();
    WriteLittleEndian16(fixture.Bytes() + 8U, 0x0400U);
    fixture.RepairChecksum();
    ASTERIA_EXPECT(runner, "an incompatible future major SAL revision is rejected",
                   HasError(fixture, SalSystemTableParseError::UnsupportedRevision));

    fixture = MakeFullTable();
    WriteLittleEndian16(fixture.Bytes() + 8U, 0x0280U);
    fixture.Bytes()[FeaturesOffset + 1U] = 0x07U;
    fixture.RepairChecksum();
    ASTERIA_EXPECT(runner, "the oldest supported SAL 2.8 revision is accepted",
                   SalSystemTableParser::Parse(fixture.Bytes(), fixture.Length()).HasValue());
    fixture.Bytes()[FeaturesOffset + 1U] = 0x08U;
    fixture.RepairChecksum();
    ASTERIA_EXPECT(runner, "SAL 2.x rejects the later ITC-drift feature bit as reserved",
                   HasError(fixture, SalSystemTableParseError::InvalidPlatformFeaturesDescriptor));
    bool earlySalThreeRevisionsRejectItcDrift{true};
    constexpr Foundation::UInt16 EarlySalThreeRevisions[]{
        0x0300U,
        0x0310U,
        0x0320U,
    };
    for (const Foundation::UInt16 revision : EarlySalThreeRevisions)
    {
        fixture = MakeFullTable();
        WriteLittleEndian16(fixture.Bytes() + 8U, revision);
        fixture.Bytes()[FeaturesOffset + 1U] = 0x08U;
        fixture.RepairChecksum();
        earlySalThreeRevisionsRejectItcDrift =
            earlySalThreeRevisionsRejectItcDrift &&
            HasError(fixture, SalSystemTableParseError::InvalidPlatformFeaturesDescriptor);
    }
    ASTERIA_EXPECT(runner, "SAL 3.0 through 3.2 keep the later ITC-drift feature bit reserved",
                   earlySalThreeRevisionsRejectItcDrift);
    fixture = MakeFullTable();
    WriteLittleEndian16(fixture.Bytes() + 8U, 0x0330U);
    fixture.Bytes()[FeaturesOffset + 1U] = 0x08U;
    fixture.RepairChecksum();
    ASTERIA_EXPECT(runner, "SAL 3.3 admits the ITC-drift feature at its introduction boundary",
                   SalSystemTableParser::Parse(fixture.Bytes(), fixture.Length()).HasValue());
    fixture = MakeFullTable();
    WriteLittleEndian16(fixture.Bytes() + 8U, 0x0399U);
    fixture.RepairChecksum();
    ASTERIA_EXPECT(runner, "a backward-compatible future SAL 3.x minor revision is accepted",
                   SalSystemTableParser::Parse(fixture.Bytes(), fixture.Length()).HasValue());

    bool everyTablePrefixRejected{true};
    fixture = MakeFullTable();
    for (Foundation::Size size{SalSystemTableHeaderSize}; size < fixture.Length(); ++size)
    {
        const auto result = SalSystemTableParser::Parse(fixture.Bytes(), size);
        everyTablePrefixRejected = everyTablePrefixRejected && !result.HasValue() &&
                                   (result.GetError() == SalSystemTableParseError::TruncatedTable);
    }
    ASTERIA_EXPECT(runner, "every readable extent shorter than the declared table is rejected",
                   everyTablePrefixRejected);

    bool everyHeaderReservedByteChecked{true};
    for (Foundation::Size offset{13U}; offset < 20U; ++offset)
    {
        fixture = MakeFullTable();
        fixture.Bytes()[offset] = 1U;
        fixture.RepairChecksum();
        everyHeaderReservedByteChecked =
            everyHeaderReservedByteChecked && HasError(fixture, SalSystemTableParseError::InvalidHeaderReservedField);
    }
    for (Foundation::Size offset{88U}; offset < 96U; ++offset)
    {
        fixture = MakeFullTable();
        fixture.Bytes()[offset] = 1U;
        fixture.RepairChecksum();
        everyHeaderReservedByteChecked =
            everyHeaderReservedByteChecked && HasError(fixture, SalSystemTableParseError::InvalidHeaderReservedField);
    }
    ASTERIA_EXPECT(runner, "every reserved header byte is required to be zero", everyHeaderReservedByteChecked);

    bool everyCoveredByteAffectsChecksum{true};
    for (Foundation::Size offset{24U}; offset < 88U; ++offset)
    {
        fixture = MakeFullTable();
        fixture.Bytes()[offset] ^= 1U;
        everyCoveredByteAffectsChecksum =
            everyCoveredByteAffectsChecksum && HasError(fixture, SalSystemTableParseError::InvalidChecksum);
    }
    ASTERIA_EXPECT(runner, "mutating any OEM or product byte invalidates the table checksum",
                   everyCoveredByteAffectsChecksum);
}

void TestDescriptorFraming(TestContext& runner) noexcept
{
    SalTableFixture fixture;
    fixture.AddOpaque(6U, 16U);
    fixture.Finalize();
    ASTERIA_EXPECT(runner, "a reserved descriptor type is rejected",
                   HasError(fixture, SalSystemTableParseError::UnknownDescriptorType));

    fixture = SalTableFixture{};
    fixture.AddEntrypoint();
    fixture.AddFeatures();
    fixture.AddMemory();
    fixture.Finalize();
    ASTERIA_EXPECT(runner, "descriptor types must be in ascending order",
                   HasError(fixture, SalSystemTableParseError::InvalidDescriptorOrder));

    fixture = MakeFullTable();
    fixture.SetEntryCount(6U);
    fixture.RepairChecksum();
    ASTERIA_EXPECT(runner, "unclaimed bytes after the declared entry count are rejected",
                   HasError(fixture, SalSystemTableParseError::TableLengthMismatch));
    fixture = MakeFullTable();
    fixture.SetEntryCount(8U);
    fixture.RepairChecksum();
    ASTERIA_EXPECT(runner, "an entry count extending beyond the table is rejected",
                   HasError(fixture, SalSystemTableParseError::DescriptorTruncated));

    fixture = SalTableFixture{};
    fixture.AddOpaque(0U, 16U);
    fixture.Finalize();
    ASTERIA_EXPECT(runner, "a fixed-length descriptor truncated by the table extent is rejected",
                   HasError(fixture, SalSystemTableParseError::DescriptorTruncated));

    fixture = SalTableFixture{};
    fixture.AddFeatures();
    fixture.Finalize();
    ASTERIA_EXPECT(runner, "a structurally valid table without an entrypoint is rejected",
                   HasError(fixture, SalSystemTableParseError::MissingEntrypointDescriptor));

    bool everySingletonDuplicateRejected{true};
    fixture = SalTableFixture{};
    fixture.AddEntrypoint();
    fixture.AddEntrypoint();
    fixture.Finalize();
    everySingletonDuplicateRejected =
        everySingletonDuplicateRejected && HasError(fixture, SalSystemTableParseError::DuplicateDescriptor);
    fixture = SalTableFixture{};
    fixture.AddEntrypoint();
    fixture.AddFeatures();
    fixture.AddFeatures();
    fixture.Finalize();
    everySingletonDuplicateRejected =
        everySingletonDuplicateRejected && HasError(fixture, SalSystemTableParseError::DuplicateDescriptor);
    fixture = SalTableFixture{};
    fixture.AddEntrypoint();
    fixture.AddTranslationRegister();
    fixture.AddTranslationRegister();
    fixture.Finalize();
    everySingletonDuplicateRejected =
        everySingletonDuplicateRejected && HasError(fixture, SalSystemTableParseError::DuplicateDescriptor);
    fixture = SalTableFixture{};
    fixture.AddEntrypoint();
    fixture.AddPurgeCoherence();
    fixture.AddPurgeCoherence();
    fixture.Finalize();
    everySingletonDuplicateRejected =
        everySingletonDuplicateRejected && HasError(fixture, SalSystemTableParseError::DuplicateDescriptor);
    fixture = SalTableFixture{};
    fixture.AddEntrypoint();
    fixture.AddApWake();
    fixture.AddApWake();
    fixture.Finalize();
    everySingletonDuplicateRejected =
        everySingletonDuplicateRejected && HasError(fixture, SalSystemTableParseError::DuplicateDescriptor);
    ASTERIA_EXPECT(runner, "every singleton descriptor type rejects duplicates", everySingletonDuplicateRejected);

    fixture = SalTableFixture{};
    fixture.AddEntrypoint();
    fixture.AddMemory();
    fixture.AddMemory();
    fixture.AddMemory();
    fixture.Finalize();
    const auto repeatedMemory = SalSystemTableParser::Parse(fixture.Bytes(), fixture.Length());
    ASTERIA_EXPECT(runner, "repeated type-one memory descriptors are counted and accepted", repeatedMemory.HasValue(),
                   repeatedMemory.GetValue().MemoryDescriptorCount == 3U);
}

void TestEntrypointValidation(TestContext& runner) noexcept
{
    bool everyReservedByteChecked{true};
    for (Foundation::Size offset{EntrypointOffset + 1U}; offset < EntrypointOffset + 8U; ++offset)
    {
        SalTableFixture fixture = MakeFullTable();
        fixture.Bytes()[offset] = 1U;
        fixture.RepairChecksum();
        everyReservedByteChecked =
            everyReservedByteChecked && HasError(fixture, SalSystemTableParseError::InvalidEntrypointDescriptor);
    }
    for (Foundation::Size offset{EntrypointOffset + 32U}; offset < EntrypointOffset + 48U; ++offset)
    {
        SalTableFixture fixture = MakeFullTable();
        fixture.Bytes()[offset] = 1U;
        fixture.RepairChecksum();
        everyReservedByteChecked =
            everyReservedByteChecked && HasError(fixture, SalSystemTableParseError::InvalidEntrypointDescriptor);
    }
    ASTERIA_EXPECT(runner, "every reserved entrypoint byte is required to be zero", everyReservedByteChecked);

    bool invalidEntrypointAddressesRejected{true};
    constexpr Foundation::Size AddressOffsets[]{
        EntrypointOffset + 8U,
        EntrypointOffset + 16U,
        EntrypointOffset + 24U,
    };
    constexpr Foundation::UInt64 InvalidAddresses[]{
        0U,
        Foundation::UInt64{1U} << 63U,
    };
    for (const Foundation::Size offset : AddressOffsets)
    {
        for (const Foundation::UInt64 address : InvalidAddresses)
        {
            SalTableFixture fixture = MakeFullTable();
            WriteLittleEndian64(fixture.Bytes() + offset, address);
            fixture.RepairChecksum();
            invalidEntrypointAddressesRejected =
                invalidEntrypointAddressesRejected &&
                HasError(fixture, SalSystemTableParseError::InvalidEntrypointDescriptor);
        }
    }
    SalTableFixture fixture = MakeFullTable();
    WriteLittleEndian64(fixture.Bytes() + EntrypointOffset + 8U, 0x0010'0008U);
    fixture.RepairChecksum();
    invalidEntrypointAddressesRejected =
        invalidEntrypointAddressesRejected && HasError(fixture, SalSystemTableParseError::InvalidEntrypointDescriptor);
    fixture = MakeFullTable();
    WriteLittleEndian64(fixture.Bytes() + EntrypointOffset + 16U, 0x0020'0008U);
    fixture.RepairChecksum();
    invalidEntrypointAddressesRejected =
        invalidEntrypointAddressesRejected && HasError(fixture, SalSystemTableParseError::InvalidEntrypointDescriptor);
    ASTERIA_EXPECT(runner, "entrypoint addresses are nonzero and bounded while code is bundle aligned",
                   invalidEntrypointAddressesRejected);

    fixture = MakeFullTable();
    WriteLittleEndian64(fixture.Bytes() + EntrypointOffset + 8U, 0x0010'0060U);
    WriteLittleEndian64(fixture.Bytes() + EntrypointOffset + 16U, 0x0010'2020U);
    WriteLittleEndian64(fixture.Bytes() + EntrypointOffset + 24U, 0x001D'174CU);
    fixture.RepairChecksum();
    const auto standardQemu = SalSystemTableParser::Parse(fixture.Bytes(), fixture.Length());
    ASTERIA_EXPECT(runner, "the standard QEMU firmware entrypoints and unaligned GP value parse",
                   standardQemu.HasValue(), standardQemu.GetValue().PalProcedurePhysicalAddress == 0x0010'0060U,
                   standardQemu.GetValue().SalProcedurePhysicalAddress == 0x0010'2020U,
                   standardQemu.GetValue().SalGlobalDataPhysicalAddress == 0x001D'174CU);

    WriteLittleEndian64(fixture.Bytes() + EntrypointOffset + 24U, 0x0010'2000U);
    fixture.RepairChecksum();
    const auto compatibleQemu = SalSystemTableParser::Parse(fixture.Bytes(), fixture.Length());
    ASTERIA_EXPECT(runner, "the compatible QEMU firmware code-side GP anchor parses", compatibleQemu.HasValue(),
                   compatibleQemu.GetValue().SalGlobalDataPhysicalAddress == 0x0010'2000U);

    WriteLittleEndian64(fixture.Bytes() + EntrypointOffset + 24U, 0x7FFF'FFFF'FFFF'FFFFULL);
    fixture.RepairChecksum();
    const auto maximumGpValue = SalSystemTableParser::Parse(fixture.Bytes(), fixture.Length());
    ASTERIA_EXPECT(runner, "the maximum inert 63-bit GP value parses before extent binding", maximumGpValue.HasValue(),
                   maximumGpValue.GetValue().SalGlobalDataPhysicalAddress == 0x7FFF'FFFF'FFFF'FFFFULL);
}

void TestOptionalDescriptorValidation(TestContext& runner) noexcept
{
    SalTableFixture fixture = MakeFullTable();
    fixture.Bytes()[FeaturesOffset + 1U] |= 0x10U;
    fixture.RepairChecksum();
    ASTERIA_EXPECT(runner, "reserved platform feature bits are rejected",
                   HasError(fixture, SalSystemTableParseError::InvalidPlatformFeaturesDescriptor));

    bool everyFeatureReservedByteChecked{true};
    for (Foundation::Size offset{FeaturesOffset + 2U}; offset < FeaturesOffset + 16U; ++offset)
    {
        fixture = MakeFullTable();
        fixture.Bytes()[offset] = 1U;
        fixture.RepairChecksum();
        everyFeatureReservedByteChecked =
            everyFeatureReservedByteChecked &&
            HasError(fixture, SalSystemTableParseError::InvalidPlatformFeaturesDescriptor);
    }
    ASTERIA_EXPECT(runner, "every reserved platform-features byte is checked", everyFeatureReservedByteChecked);

    bool invalidTranslationRegistersRejected{true};
    constexpr Foundation::Size TranslationReservedOffsets[]{
        TranslationRegisterOffset + 3U,
        TranslationRegisterOffset + 7U,
        TranslationRegisterOffset + 24U,
        TranslationRegisterOffset + 31U,
    };
    for (const Foundation::Size offset : TranslationReservedOffsets)
    {
        fixture = MakeFullTable();
        fixture.Bytes()[offset] = 1U;
        fixture.RepairChecksum();
        invalidTranslationRegistersRejected =
            invalidTranslationRegistersRejected &&
            HasError(fixture, SalSystemTableParseError::InvalidTranslationRegisterDescriptor);
    }
    fixture = MakeFullTable();
    fixture.Bytes()[TranslationRegisterOffset + 1U] = 1U;
    fixture.RepairChecksum();
    invalidTranslationRegistersRejected =
        invalidTranslationRegistersRejected &&
        HasError(fixture, SalSystemTableParseError::InvalidTranslationRegisterDescriptor);
    fixture = MakeFullTable();
    fixture.Bytes()[TranslationRegisterOffset + 2U] = 1U;
    fixture.RepairChecksum();
    invalidTranslationRegistersRejected =
        invalidTranslationRegistersRejected &&
        HasError(fixture, SalSystemTableParseError::InvalidTranslationRegisterDescriptor);
    fixture = MakeFullTable();
    fixture.Bytes()[TranslationRegisterOffset + 16U] |= 1U;
    fixture.RepairChecksum();
    invalidTranslationRegistersRejected =
        invalidTranslationRegistersRejected &&
        HasError(fixture, SalSystemTableParseError::InvalidTranslationRegisterDescriptor);
    fixture = MakeFullTable();
    WriteLittleEndian64(fixture.Bytes() + TranslationRegisterOffset + 16U, Foundation::UInt64{11U} << 2U);
    fixture.RepairChecksum();
    invalidTranslationRegistersRejected =
        invalidTranslationRegistersRejected &&
        HasError(fixture, SalSystemTableParseError::InvalidTranslationRegisterDescriptor);
    fixture = MakeFullTable();
    WriteLittleEndian64(fixture.Bytes() + TranslationRegisterOffset + 8U, 0x0010'0001U);
    fixture.RepairChecksum();
    invalidTranslationRegistersRejected =
        invalidTranslationRegistersRejected &&
        HasError(fixture, SalSystemTableParseError::InvalidTranslationRegisterDescriptor);
    ASTERIA_EXPECT(runner, "the SAL-owned ITR zero descriptor and page-size encoding are validated",
                   invalidTranslationRegistersRejected);

    bool invalidPurgeDescriptorsRejected{true};
    for (Foundation::Size offset{PurgeCoherenceOffset + 1U}; offset < PurgeCoherenceOffset + 4U; ++offset)
    {
        fixture = MakeFullTable();
        fixture.Bytes()[offset] = 1U;
        fixture.RepairChecksum();
        invalidPurgeDescriptorsRejected = invalidPurgeDescriptorsRejected &&
                                          HasError(fixture, SalSystemTableParseError::InvalidPurgeCoherenceDescriptor);
    }
    fixture = MakeFullTable();
    WriteLittleEndian32(fixture.Bytes() + PurgeCoherenceOffset + 4U, 0U);
    fixture.RepairChecksum();
    invalidPurgeDescriptorsRejected =
        invalidPurgeDescriptorsRejected && HasError(fixture, SalSystemTableParseError::InvalidPurgeCoherenceDescriptor);
    constexpr Foundation::UInt64 InvalidPurgeAddresses[]{
        0U,
        Foundation::UInt64{1U} << 63U,
        0x0040'0004U,
    };
    for (const Foundation::UInt64 address : InvalidPurgeAddresses)
    {
        fixture = MakeFullTable();
        WriteLittleEndian64(fixture.Bytes() + PurgeCoherenceOffset + 8U, address);
        fixture.RepairChecksum();
        invalidPurgeDescriptorsRejected = invalidPurgeDescriptorsRejected &&
                                          HasError(fixture, SalSystemTableParseError::InvalidPurgeCoherenceDescriptor);
    }
    ASTERIA_EXPECT(runner, "purge-coherence reserved, count, and address fields are validated",
                   invalidPurgeDescriptorsRejected);

    bool invalidApWakeDescriptorsRejected{true};
    fixture = MakeFullTable();
    fixture.Bytes()[ApWakeOffset + 1U] = 1U;
    fixture.RepairChecksum();
    invalidApWakeDescriptorsRejected =
        invalidApWakeDescriptorsRejected && HasError(fixture, SalSystemTableParseError::InvalidApWakeDescriptor);
    for (Foundation::Size offset{ApWakeOffset + 2U}; offset < ApWakeOffset + 8U; ++offset)
    {
        fixture = MakeFullTable();
        fixture.Bytes()[offset] = 1U;
        fixture.RepairChecksum();
        invalidApWakeDescriptorsRejected =
            invalidApWakeDescriptorsRejected && HasError(fixture, SalSystemTableParseError::InvalidApWakeDescriptor);
    }
    constexpr Foundation::UInt64 InvalidVectors[]{0x0FU, 0x100U};
    for (const Foundation::UInt64 vector : InvalidVectors)
    {
        fixture = MakeFullTable();
        WriteLittleEndian64(fixture.Bytes() + ApWakeOffset + 8U, vector);
        fixture.RepairChecksum();
        invalidApWakeDescriptorsRejected =
            invalidApWakeDescriptorsRejected && HasError(fixture, SalSystemTableParseError::InvalidApWakeDescriptor);
    }
    ASTERIA_EXPECT(runner, "AP wake mechanism, reserved bytes, and vector range are validated",
                   invalidApWakeDescriptorsRejected);
}

void TestInputRemainsUnchanged(TestContext& runner) noexcept
{
    SalTableFixture fixture = MakeFullTable();
    Foundation::UInt8 before[FixtureCapacity]{};
    for (Foundation::Size index{0U}; index < FixtureCapacity; ++index)
    {
        before[index] = fixture.Bytes()[index];
    }
    const auto result = SalSystemTableParser::Parse(fixture.Bytes(), fixture.Length());
    bool unchanged{result.HasValue()};
    for (Foundation::Size index{0U}; index < FixtureCapacity; ++index)
    {
        unchanged = unchanged && (before[index] == fixture.Bytes()[index]);
    }
    ASTERIA_EXPECT(runner, "successful parsing leaves every input byte unchanged", unchanged);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"SalSystemTableParserTests"};
    Asteria::Test::TestExactDecode(runner);
    Asteria::Test::TestHeaderValidation(runner);
    Asteria::Test::TestDescriptorFraming(runner);
    Asteria::Test::TestEntrypointValidation(runner);
    Asteria::Test::TestOptionalDescriptorValidation(runner);
    Asteria::Test::TestInputRemainsUnchanged(runner);
    return runner.Finish();
}
