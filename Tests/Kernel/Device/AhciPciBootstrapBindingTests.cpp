// SPDX-License-Identifier: Apache-2.0
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <thread>

#include "Kernel/Device/AhciPciBootstrapBinding.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Foundation::Size;
using Foundation::UInt8;
using Foundation::UInt16;
using Foundation::UInt32;
using Foundation::UInt64;
using Hal::Device::MemoryMappedIoReadError;
using Hal::Device::MemoryMappedIoWidth;
using Hal::Device::PciConfigurationReadError;
using Kernel::Device::AhciHbaGlobalRegister;
using Kernel::Device::AhciHbaSnapshotError;
using Kernel::Device::AhciPciBootstrapBinding;
using Kernel::Device::AhciPciBootstrapBindingError;
using Kernel::Device::AhciPciBootstrapBindingFailure;
using Kernel::Device::AhciPciBootstrapBindingQueryError;
using Kernel::Device::AhciPciBootstrapBindingRequest;
using Kernel::Device::AhciPciBootstrapBindingResult;
using Kernel::Device::AhciPciBootstrapBindingState;
using Kernel::Device::PciBusRange;
using Kernel::Device::PciBusRangeView;
using Kernel::Device::PciEndpointBarDecodeError;
using Kernel::Device::PciEndpointBarDecoder;
using Kernel::Device::PciEndpointBarExtentState;
using Kernel::Device::PciEndpointBarKind;
using Kernel::Device::PciFunctionAddress;
using Kernel::Device::PciFunctionCatalog;
using Kernel::Device::PciFunctionCatalogError;

constexpr UInt64 MaximumUInt64{~UInt64{0U}};
constexpr UInt64 AbarBase{0x0000'0000'FEBF'0000ULL};

struct FakePciFunction
{
    PciFunctionAddress Address;
    UInt32 Identifier;
    UInt32 ClassRegister;
    UInt32 HeaderRegister;
    UInt32 Bar5;
    PciConfigurationReadError Bar5Error;
};

class FakePciConfigurationSpace final : public Hal::Device::IPciConfigurationSpace
{
public:
    void AddFunction(const PciFunctionAddress address, const UInt8 classCode, const UInt8 subclass,
                     const UInt8 programmingInterface, const UInt8 headerLayout, const UInt32 bar5,
                     const bool multifunction = false) noexcept
    {
        if (m_functionCount >= MaximumFunctions)
        {
            return;
        }
        const UInt32 classRegister = (static_cast<UInt32>(classCode) << 24U) | (static_cast<UInt32>(subclass) << 16U) |
                                     (static_cast<UInt32>(programmingInterface) << 8U) | 0x02U;
        const UInt8 headerType = static_cast<UInt8>(headerLayout | (multifunction ? 0x80U : 0U));
        m_functions[m_functionCount] = FakePciFunction{
            .Address = address,
            .Identifier = 0x5678'1234U + static_cast<UInt32>(m_functionCount),
            .ClassRegister = classRegister,
            .HeaderRegister = static_cast<UInt32>(headerType) << 16U,
            .Bar5 = bar5,
            .Bar5Error = PciConfigurationReadError::None,
        };
        ++m_functionCount;
    }

    void SetBar5Error(const PciFunctionAddress address, const PciConfigurationReadError error) noexcept
    {
        FakePciFunction* const function = Find(address);
        if (function != nullptr)
        {
            function->Bar5Error = error;
        }
    }

    [[nodiscard]] PciConfigurationReadError Read32(const PciFunctionAddress address, const UInt16 registerOffset,
                                                   UInt32& output) noexcept override
    {
        ++m_readCount;
        FakePciFunction* const function = Find(address);
        if (registerOffset == 0x00U)
        {
            output = function == nullptr ? 0xFFFF'FFFFU : function->Identifier;
            return PciConfigurationReadError::None;
        }
        if (function == nullptr)
        {
            return PciConfigurationReadError::InvalidAddress;
        }
        if (registerOffset == 0x08U)
        {
            output = function->ClassRegister;
            return PciConfigurationReadError::None;
        }
        if (registerOffset == 0x0CU)
        {
            output = function->HeaderRegister;
            return PciConfigurationReadError::None;
        }
        if (registerOffset == 0x24U)
        {
            ++m_bar5ReadCount;
            m_lastBar5Address = address;
            if (function->Bar5Error != PciConfigurationReadError::None)
            {
                return function->Bar5Error;
            }
            output = function->Bar5;
            return PciConfigurationReadError::None;
        }
        return PciConfigurationReadError::InvalidAddress;
    }

    [[nodiscard]] Size GetReadCount() const noexcept
    {
        return m_readCount;
    }

    [[nodiscard]] Size GetBar5ReadCount() const noexcept
    {
        return m_bar5ReadCount;
    }

    [[nodiscard]] PciFunctionAddress GetLastBar5Address() const noexcept
    {
        return m_lastBar5Address;
    }

private:
    [[nodiscard]] FakePciFunction* Find(const PciFunctionAddress address) noexcept
    {
        for (Size index{0U}; index < m_functionCount; ++index)
        {
            if (m_functions[index].Address.IsEqualTo(address))
            {
                return &m_functions[index];
            }
        }
        return nullptr;
    }

    static constexpr Size MaximumFunctions{16U};

    FakePciFunction m_functions[MaximumFunctions]{};
    Size m_functionCount{0U};
    Size m_readCount{0U};
    Size m_bar5ReadCount{0U};
    PciFunctionAddress m_lastBar5Address{};
};

class FakeMemoryMappedIo final : public Hal::Device::IMemoryMappedIo
{
public:
    void SetSuccessfulSnapshot(const UInt64 baseAddress = AbarBase) noexcept
    {
        m_baseAddress = baseAddress;
        m_values[0U] = 0xCC32'1F03U;
        m_values[1U] = 0x8000'0002U;
        m_values[2U] = 0x0000'0005U;
        m_values[3U] = 0x0000'000BU;
        m_values[4U] = 0x0001'0301U;
    }

    void SetFailure(const Size readIndex, const MemoryMappedIoReadError error) noexcept
    {
        m_failureIndex = readIndex;
        m_failure = error;
    }

    void PauseFirstRead() noexcept
    {
        m_pauseFirstRead.store(true);
    }

    void WaitUntilPaused() const noexcept
    {
        while (!m_firstReadPaused.load())
        {
            std::this_thread::yield();
        }
    }

    void ReleaseFirstRead() noexcept
    {
        m_releaseFirstRead.store(true);
    }

    [[nodiscard]] MemoryMappedIoReadError Read(const UInt64 physicalAddress, const MemoryMappedIoWidth width,
                                               UInt64& output) noexcept override
    {
        const Size readIndex = m_readCount;
        if (readIndex < MaximumReads)
        {
            m_addresses[readIndex] = physicalAddress;
            m_widths[readIndex] = width;
        }
        ++m_readCount;
        if ((readIndex == 0U) && m_pauseFirstRead.load())
        {
            m_firstReadPaused.store(true);
            while (!m_releaseFirstRead.load())
            {
                std::this_thread::yield();
            }
        }
        if ((readIndex == m_failureIndex) && (m_failure != MemoryMappedIoReadError::None))
        {
            return m_failure;
        }
        if ((width != MemoryMappedIoWidth::DoubleWord) || (physicalAddress < m_baseAddress) ||
            (((physicalAddress - m_baseAddress) & 3U) != 0U))
        {
            return MemoryMappedIoReadError::InvalidAddress;
        }
        const UInt64 registerIndex = (physicalAddress - m_baseAddress) / 4U;
        if (registerIndex >= 5U)
        {
            return MemoryMappedIoReadError::InvalidAddress;
        }
        output = m_values[static_cast<Size>(registerIndex)];
        return MemoryMappedIoReadError::None;
    }

    [[nodiscard]] Size GetReadCount() const noexcept
    {
        return m_readCount;
    }

    [[nodiscard]] UInt64 GetAddress(const Size index) const noexcept
    {
        return m_addresses[index];
    }

    [[nodiscard]] MemoryMappedIoWidth GetWidth(const Size index) const noexcept
    {
        return m_widths[index];
    }

private:
    static constexpr Size MaximumReads{8U};

    UInt64 m_baseAddress{AbarBase};
    UInt64 m_values[5U]{};
    UInt64 m_addresses[MaximumReads]{};
    MemoryMappedIoWidth m_widths[MaximumReads]{};
    Size m_readCount{0U};
    Size m_failureIndex{MaximumReads};
    MemoryMappedIoReadError m_failure{MemoryMappedIoReadError::None};
    std::atomic<bool> m_pauseFirstRead{false};
    std::atomic<bool> m_firstReadPaused{false};
    std::atomic<bool> m_releaseFirstRead{false};
};

class BindingFixture final
{
public:
    BindingFixture() noexcept
        : Catalog{Configuration}, Decoder{Configuration}, SnapshotReader{MemoryMappedIo},
          Binding{Catalog, Decoder, SnapshotReader}
    {
        MemoryMappedIo.SetSuccessfulSnapshot();
    }

    [[nodiscard]] bool Discover() noexcept
    {
        constexpr PciBusRange range{
            .Segment = 0U,
            .FirstBus = 0U,
            .LastBus = 0U,
            .Reserved = {},
        };
        return Catalog
                   .Discover(PciBusRangeView{
                       .Data = &range,
                       .Count = 1U,
                   })
                   .Error == PciFunctionCatalogError::None;
    }

    FakePciConfigurationSpace Configuration;
    FakeMemoryMappedIo MemoryMappedIo;
    PciFunctionCatalog Catalog;
    PciEndpointBarDecoder Decoder;
    Kernel::Device::AhciHbaRegisterSnapshotReader SnapshotReader;
    AhciPciBootstrapBinding Binding;
};

[[nodiscard]] constexpr PciFunctionAddress MakeAddress(const UInt8 device) noexcept
{
    return PciFunctionAddress{0U, 0U, device, 0U};
}

void AddAhciFunction(BindingFixture& fixture, const UInt8 device,
                     const UInt32 bar5 = static_cast<UInt32>(AbarBase)) noexcept
{
    fixture.Configuration.AddFunction(MakeAddress(device), 0x01U, 0x06U, 0x01U, 0x00U, bar5);
}

[[nodiscard]] constexpr AhciPciBootstrapBindingRequest
MakeRequest(const UInt64 extentBytes = 0x1000U, const UInt64 maximumPhysicalAddress = MaximumUInt64) noexcept
{
    return AhciPciBootstrapBindingRequest{
        .MaximumPhysicalAddress = maximumPhysicalAddress,
        .AbarExtentBytes = extentBytes,
    };
}

[[nodiscard]] constexpr AhciPciBootstrapBindingResult MakeSentinel() noexcept
{
    return AhciPciBootstrapBindingResult{
        .Function =
            {
                .Address = PciFunctionAddress{9U, 8U, 7U, 6U},
                .VendorId = 0x1111U,
                .DeviceId = 0x2222U,
                .RevisionId = 3U,
                .ProgrammingInterface = 4U,
                .Subclass = 5U,
                .ClassCode = 6U,
                .HeaderLayout = 7U,
                .Multifunction = true,
            },
        .Abar =
            {
                .BaseAddress = 0x1111'2222'3333'4444ULL,
                .ExtentBytes = 0x5555'6666'7777'8888ULL,
                .Kind = PciEndpointBarKind::Memory64,
                .ExtentState = PciEndpointBarExtentState::Authoritative,
                .BarIndex = 4U,
                .RegisterCount = 2U,
                .Prefetchable = true,
                .Reserved = {1U, 2U, 3U},
            },
        .Snapshot =
            {
                .Capabilities = 0x1111'1111U,
                .GlobalHostControl = 0x2222'2222U,
                .InterruptStatus = 0x3333'3333U,
                .PortsImplemented = 0x4444'4444U,
                .Version = 0x5555'5555U,
                .DecodedCapabilities =
                    {
                        .PortCount = 6U,
                        .CommandSlotCount = 7U,
                        .InterfaceSpeedSupport = 8U,
                        .Supports64BitAddressing = true,
                        .SupportsNativeCommandQueuing = true,
                        .SupportsPortMultiplier = true,
                        .SupportsStaggeredSpinUp = true,
                        .SupportsAggressiveLinkPowerManagement = true,
                    },
                .DecodedControl =
                    {
                        .AhciModeEnabled = true,
                        .InterruptsEnabled = true,
                        .ResetRequested = true,
                        .Reserved = {1U, 2U, 3U, 4U, 5U},
                    },
                .DecodedVersion =
                    {
                        .Major = 9U,
                        .Minor = 10U,
                    },
            },
    };
}

[[nodiscard]] constexpr bool IsSentinel(const AhciPciBootstrapBindingResult& result) noexcept
{
    return result.Function.Address.IsEqualTo(PciFunctionAddress{9U, 8U, 7U, 6U}) &&
           (result.Function.VendorId == 0x1111U) && (result.Function.DeviceId == 0x2222U) &&
           result.Function.Multifunction && (result.Abar.BaseAddress == 0x1111'2222'3333'4444ULL) &&
           (result.Abar.ExtentBytes == 0x5555'6666'7777'8888ULL) &&
           (result.Abar.Kind == PciEndpointBarKind::Memory64) && result.Abar.Prefetchable &&
           (result.Abar.Reserved[2U] == 3U) && (result.Snapshot.Capabilities == 0x1111'1111U) &&
           (result.Snapshot.Version == 0x5555'5555U) && (result.Snapshot.DecodedCapabilities.PortCount == 6U) &&
           result.Snapshot.DecodedCapabilities.Supports64BitAddressing &&
           result.Snapshot.DecodedControl.ResetRequested && (result.Snapshot.DecodedControl.Reserved[4U] == 5U) &&
           (result.Snapshot.DecodedVersion.Major == 9U) && (result.Snapshot.DecodedVersion.Minor == 10U);
}

[[nodiscard]] constexpr bool FailureReservedIsZero(const AhciPciBootstrapBindingFailure& failure) noexcept
{
    for (const UInt8 value : failure.Reserved)
    {
        if (value != 0U)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool HasSuccessfulPublication(const AhciPciBootstrapBindingResult& result,
                                            const PciFunctionAddress expectedAddress) noexcept
{
    return result.Function.Address.IsEqualTo(expectedAddress) && (result.Function.ClassCode == 0x01U) &&
           (result.Function.Subclass == 0x06U) && (result.Function.ProgrammingInterface == 0x01U) &&
           (result.Function.HeaderLayout == 0x00U) && (result.Abar.BaseAddress == AbarBase) &&
           (result.Abar.ExtentBytes == 0x1000U) && (result.Abar.Kind == PciEndpointBarKind::Memory32) &&
           (result.Abar.ExtentState == PciEndpointBarExtentState::Authoritative) && (result.Abar.BarIndex == 5U) &&
           (result.Abar.RegisterCount == 1U) && !result.Abar.Prefetchable &&
           (result.Snapshot.Capabilities == 0xCC32'1F03U) && (result.Snapshot.GlobalHostControl == 0x8000'0002U) &&
           (result.Snapshot.InterruptStatus == 5U) && (result.Snapshot.PortsImplemented == 0xBU) &&
           (result.Snapshot.Version == 0x0001'0301U) && (result.Snapshot.DecodedCapabilities.PortCount == 4U) &&
           (result.Snapshot.DecodedCapabilities.CommandSlotCount == 32U) &&
           result.Snapshot.DecodedCapabilities.Supports64BitAddressing &&
           result.Snapshot.DecodedControl.AhciModeEnabled && result.Snapshot.DecodedControl.InterruptsEnabled &&
           (result.Snapshot.DecodedVersion.Major == 1U) && (result.Snapshot.DecodedVersion.Minor == 0x0301U);
}

void CheckNotPublished(TestContext& runner, AhciPciBootstrapBinding& binding, const char* const description) noexcept
{
    AhciPciBootstrapBindingResult output = MakeSentinel();
    const AhciPciBootstrapBindingQueryError query = binding.GetResult(output);
    ASTERIA_EXPECT(runner, description, query == AhciPciBootstrapBindingQueryError::NotReady,
                   binding.GetState() == AhciPciBootstrapBindingState::Empty, IsSentinel(output));
}

void TestSuccessfulPublicationAndReadSequence(TestContext& runner) noexcept
{
    BindingFixture fixture;
    fixture.Configuration.AddFunction(MakeAddress(1U), 0x02U, 0x00U, 0x00U, 0x00U, 0U);
    AddAhciFunction(fixture, 7U);
    ASTERIA_EXPECT(runner, "the success fixture catalog is ready", fixture.Discover());
    CheckNotPublished(runner, fixture.Binding, "a result query before Prepare preserves every output component");

    const Size configurationReadsBefore = fixture.Configuration.GetReadCount();
    const AhciPciBootstrapBindingFailure prepared = fixture.Binding.Prepare(MakeRequest());
    AhciPciBootstrapBindingResult result = MakeSentinel();
    const AhciPciBootstrapBindingQueryError query = fixture.Binding.GetResult(result);

    bool mmioSequenceMatches = fixture.MemoryMappedIo.GetReadCount() == 5U;
    for (Size index{0U}; index < 5U; ++index)
    {
        mmioSequenceMatches =
            mmioSequenceMatches &&
            (fixture.MemoryMappedIo.GetAddress(index) == (AbarBase + (static_cast<UInt64>(index) * 4U))) &&
            (fixture.MemoryMappedIo.GetWidth(index) == MemoryMappedIoWidth::DoubleWord);
    }
    ASTERIA_EXPECT(runner, "one compliant AHCI endpoint publishes its descriptor, ABAR, and snapshot",
                   prepared.Error == AhciPciBootstrapBindingError::None, prepared.MatchingFunctionCount == 1U,
                   FailureReservedIsZero(prepared), query == AhciPciBootstrapBindingQueryError::None,
                   fixture.Binding.GetState() == AhciPciBootstrapBindingState::Ready,
                   HasSuccessfulPublication(result, MakeAddress(7U)));
    ASTERIA_EXPECT(runner, "binding reads only selected BAR5 once and each HBA register once",
                   fixture.Configuration.GetReadCount() == (configurationReadsBefore + 1U),
                   fixture.Configuration.GetBar5ReadCount() == 1U,
                   fixture.Configuration.GetLastBar5Address().IsEqualTo(MakeAddress(7U)), mmioSequenceMatches);

    const Size configurationReadsAfter = fixture.Configuration.GetReadCount();
    const Size mmioReadsAfter = fixture.MemoryMappedIo.GetReadCount();
    const AhciPciBootstrapBindingFailure repeated = fixture.Binding.Prepare(MakeRequest(0x2000U));
    AhciPciBootstrapBindingResult repeatedResult{};
    ASTERIA_EXPECT(runner, "a published binding is immutable and repeated Prepare performs no reads",
                   repeated.Error == AhciPciBootstrapBindingError::AlreadyPrepared,
                   repeated.MatchingFunctionCount == 1U,
                   fixture.Configuration.GetReadCount() == configurationReadsAfter,
                   fixture.MemoryMappedIo.GetReadCount() == mmioReadsAfter,
                   fixture.Binding.GetResult(repeatedResult) == AhciPciBootstrapBindingQueryError::None,
                   HasSuccessfulPublication(repeatedResult, MakeAddress(7U)));
}

void TestExactClassAndHeaderFiltering(TestContext& runner) noexcept
{
    BindingFixture fixture;
    fixture.Configuration.AddFunction(MakeAddress(1U), 0x02U, 0x06U, 0x01U, 0x00U, 0U);
    fixture.Configuration.AddFunction(MakeAddress(2U), 0x01U, 0x05U, 0x01U, 0x00U, 0U);
    fixture.Configuration.AddFunction(MakeAddress(3U), 0x01U, 0x06U, 0x00U, 0x00U, 0U);
    fixture.Configuration.AddFunction(MakeAddress(4U), 0x01U, 0x06U, 0x01U, 0x01U, 0U);
    AddAhciFunction(fixture, 12U);
    ASTERIA_EXPECT(runner, "the class-filter fixture catalog is ready", fixture.Discover());
    const Size readsBefore = fixture.Configuration.GetReadCount();
    const AhciPciBootstrapBindingFailure prepared = fixture.Binding.Prepare(MakeRequest());
    AhciPciBootstrapBindingResult result{};
    ASTERIA_EXPECT(runner, "class, subclass, programming interface, and endpoint layout all filter exactly",
                   prepared.Error == AhciPciBootstrapBindingError::None, prepared.MatchingFunctionCount == 1U,
                   fixture.Binding.GetResult(result) == AhciPciBootstrapBindingQueryError::None,
                   result.Function.Address.IsEqualTo(MakeAddress(12U)),
                   fixture.Configuration.GetReadCount() == (readsBefore + 1U),
                   fixture.Configuration.GetBar5ReadCount() == 1U);

    BindingFixture rejected;
    rejected.Configuration.AddFunction(MakeAddress(1U), 0x02U, 0x06U, 0x01U, 0x00U, 0U);
    rejected.Configuration.AddFunction(MakeAddress(2U), 0x01U, 0x05U, 0x01U, 0x00U, 0U);
    rejected.Configuration.AddFunction(MakeAddress(3U), 0x01U, 0x06U, 0x00U, 0x00U, 0U);
    rejected.Configuration.AddFunction(MakeAddress(4U), 0x01U, 0x06U, 0x01U, 0x01U, 0U);
    ASTERIA_EXPECT(runner, "the rejected-class catalog is ready", rejected.Discover());
    const Size rejectedReadsBefore = rejected.Configuration.GetReadCount();
    const AhciPciBootstrapBindingFailure noMatch = rejected.Binding.Prepare(MakeRequest());
    ASTERIA_EXPECT(runner, "near-match PCI functions never trigger BAR or MMIO reads",
                   noMatch.Error == AhciPciBootstrapBindingError::NoAhciController, noMatch.MatchingFunctionCount == 0U,
                   rejected.Configuration.GetReadCount() == rejectedReadsBefore,
                   rejected.Configuration.GetBar5ReadCount() == 0U, rejected.MemoryMappedIo.GetReadCount() == 0U);
    CheckNotPublished(runner, rejected.Binding, "class-filter failure publishes no partial output");
}

void TestNoControllerAmbiguityAndCatalogState(TestContext& runner) noexcept
{
    BindingFixture empty;
    ASTERIA_EXPECT(runner, "an empty PCI catalog can become ready", empty.Discover());
    const Size emptyReadsBefore = empty.Configuration.GetReadCount();
    const AhciPciBootstrapBindingFailure absent = empty.Binding.Prepare(MakeRequest());
    ASTERIA_EXPECT(runner, "a ready catalog with no device reports deterministic absence without reads",
                   absent.Error == AhciPciBootstrapBindingError::NoAhciController, absent.MatchingFunctionCount == 0U,
                   FailureReservedIsZero(absent), empty.Configuration.GetReadCount() == emptyReadsBefore,
                   empty.MemoryMappedIo.GetReadCount() == 0U);

    BindingFixture ambiguous;
    AddAhciFunction(ambiguous, 19U, 0xDEAD'0000U);
    AddAhciFunction(ambiguous, 3U, static_cast<UInt32>(AbarBase));
    ASTERIA_EXPECT(runner, "the reverse-insertion ambiguity catalog is ready", ambiguous.Discover());
    const Size ambiguousReadsBefore = ambiguous.Configuration.GetReadCount();
    const AhciPciBootstrapBindingFailure multiple = ambiguous.Binding.Prepare(MakeRequest());
    ASTERIA_EXPECT(runner, "multiple compliant functions fail independent of discovery order",
                   multiple.Error == AhciPciBootstrapBindingError::AmbiguousAhciController,
                   multiple.MatchingFunctionCount == 2U, ambiguous.Configuration.GetReadCount() == ambiguousReadsBefore,
                   ambiguous.Configuration.GetBar5ReadCount() == 0U, ambiguous.MemoryMappedIo.GetReadCount() == 0U);
    CheckNotPublished(runner, ambiguous.Binding, "ambiguity publishes neither candidate nor a partial snapshot");

    BindingFixture unready;
    const AhciPciBootstrapBindingFailure catalogNotReady = unready.Binding.Prepare(MakeRequest());
    ASTERIA_EXPECT(runner, "an undiscovered catalog is distinguished from a ready empty catalog",
                   catalogNotReady.Error == AhciPciBootstrapBindingError::CatalogNotReady,
                   catalogNotReady.Catalog.Error == PciFunctionCatalogError::NotReady,
                   catalogNotReady.MatchingFunctionCount == 0U, unready.Configuration.GetReadCount() == 0U,
                   unready.MemoryMappedIo.GetReadCount() == 0U);
}

void CheckAbarFailureNotPublished(TestContext& runner, BindingFixture& fixture,
                                  const AhciPciBootstrapBindingFailure& failure,
                                  const AhciPciBootstrapBindingError expectedError,
                                  const PciEndpointBarDecodeError expectedDecodeError, const Size expectedBarReads,
                                  const char* const description) noexcept
{
    ASTERIA_EXPECT(runner, description, failure.Error == expectedError, failure.Abar.Error == expectedDecodeError,
                   failure.MatchingFunctionCount == 1U, fixture.Configuration.GetBar5ReadCount() == expectedBarReads,
                   fixture.MemoryMappedIo.GetReadCount() == 0U, FailureReservedIsZero(failure));
    CheckNotPublished(runner, fixture.Binding, "a BAR5 rejection leaves the aggregate publication unchanged");
}

void TestBar5TypeAndExtentFailures(TestContext& runner) noexcept
{
    BindingFixture belowAbarLimit;
    AddAhciFunction(belowAbarLimit, 5U);
    ASTERIA_EXPECT(runner, "the below-ABAR physical-limit fixture is ready", belowAbarLimit.Discover());
    const AhciPciBootstrapBindingFailure physicalLimitFailure =
        belowAbarLimit.Binding.Prepare(MakeRequest(0x1000U, AbarBase - 1U));
    CheckAbarFailureNotPublished(runner, belowAbarLimit, physicalLimitFailure,
                                 AhciPciBootstrapBindingError::AbarDecodeRejected,
                                 PciEndpointBarDecodeError::PhysicalAddressLimitExceeded, 1U,
                                 "binding forwards a maximum physical address below ABAR to the decoder");

    const UInt64 invalidExtents[2U]{0U, 24U};
    for (const UInt64 invalidExtent : invalidExtents)
    {
        BindingFixture fixture;
        AddAhciFunction(fixture, 5U);
        ASTERIA_EXPECT(runner, "an invalid-extent fixture is ready", fixture.Discover());
        const AhciPciBootstrapBindingFailure failure = fixture.Binding.Prepare(MakeRequest(invalidExtent));
        CheckAbarFailureNotPublished(runner, fixture, failure, AhciPciBootstrapBindingError::AbarDecodeRejected,
                                     PciEndpointBarDecodeError::InvalidExtent, 0U,
                                     "zero and non-power-of-two authoritative extents fail before BAR access");
    }

    BindingFixture shortExtent;
    AddAhciFunction(shortExtent, 5U);
    ASTERIA_EXPECT(runner, "the short-extent fixture is ready", shortExtent.Discover());
    const AhciPciBootstrapBindingFailure shortFailure = shortExtent.Binding.Prepare(MakeRequest(16U));
    ASTERIA_EXPECT(runner, "an extent too short for fixed HBA registers fails before MMIO",
                   shortFailure.Error == AhciPciBootstrapBindingError::SnapshotReadRejected,
                   shortFailure.Snapshot.Error == AhciHbaSnapshotError::InvalidRange,
                   shortExtent.Configuration.GetBar5ReadCount() == 1U, shortExtent.MemoryMappedIo.GetReadCount() == 0U);
    CheckNotPublished(runner, shortExtent.Binding, "a short ABAR extent publishes no decoded BAR alone");

    BindingFixture ioBar;
    AddAhciFunction(ioBar, 5U, 0x0000'C001U);
    ASTERIA_EXPECT(runner, "the I/O-BAR fixture is ready", ioBar.Discover());
    const AhciPciBootstrapBindingFailure ioFailure = ioBar.Binding.Prepare(MakeRequest(0x100U));
    CheckAbarFailureNotPublished(runner, ioBar, ioFailure, AhciPciBootstrapBindingError::AbarKindRejected,
                                 PciEndpointBarDecodeError::None, 1U,
                                 "an I/O BAR5 cannot become an AHCI MMIO register aperture");

    BindingFixture prefetchable;
    AddAhciFunction(prefetchable, 5U, static_cast<UInt32>(AbarBase) | 0x08U);
    ASTERIA_EXPECT(runner, "the prefetchable fixture is ready", prefetchable.Discover());
    const AhciPciBootstrapBindingFailure prefetchableFailure = prefetchable.Binding.Prepare(MakeRequest());
    CheckAbarFailureNotPublished(runner, prefetchable, prefetchableFailure,
                                 AhciPciBootstrapBindingError::AbarPrefetchable, PciEndpointBarDecodeError::None, 1U,
                                 "a prefetchable BAR5 is rejected for AHCI side-effecting registers");

    BindingFixture bar64;
    AddAhciFunction(bar64, 5U, static_cast<UInt32>(AbarBase) | 0x04U);
    ASTERIA_EXPECT(runner, "the 64-bit BAR5 fixture is ready", bar64.Discover());
    const AhciPciBootstrapBindingFailure bar64Failure = bar64.Binding.Prepare(MakeRequest());
    CheckAbarFailureNotPublished(runner, bar64, bar64Failure, AhciPciBootstrapBindingError::AbarDecodeRejected,
                                 PciEndpointBarDecodeError::MissingUpperBar, 1U,
                                 "a 64-bit BAR cannot begin at the final endpoint BAR register");

    BindingFixture readFailure;
    AddAhciFunction(readFailure, 5U);
    readFailure.Configuration.SetBar5Error(MakeAddress(5U), PciConfigurationReadError::HardwareFailure);
    ASTERIA_EXPECT(runner, "the BAR-read failure fixture is ready", readFailure.Discover());
    const AhciPciBootstrapBindingFailure failedRead = readFailure.Binding.Prepare(MakeRequest());
    ASTERIA_EXPECT(runner, "the exact BAR5 configuration read failure propagates without MMIO",
                   failedRead.Error == AhciPciBootstrapBindingError::AbarDecodeRejected,
                   failedRead.Abar.Error == PciEndpointBarDecodeError::ConfigurationReadFailed,
                   failedRead.Abar.ConfigurationRead == PciConfigurationReadError::HardwareFailure,
                   readFailure.Configuration.GetBar5ReadCount() == 1U, readFailure.MemoryMappedIo.GetReadCount() == 0U);
    CheckNotPublished(runner, readFailure.Binding, "a BAR5 read failure preserves atomic empty publication");
}

void TestSnapshotFailurePropagation(TestContext& runner) noexcept
{
    constexpr AhciHbaGlobalRegister registers[5U]{
        AhciHbaGlobalRegister::Capabilities,    AhciHbaGlobalRegister::GlobalHostControl,
        AhciHbaGlobalRegister::InterruptStatus, AhciHbaGlobalRegister::PortsImplemented,
        AhciHbaGlobalRegister::Version,
    };
    for (Size failureIndex{0U}; failureIndex < 5U; ++failureIndex)
    {
        BindingFixture fixture;
        AddAhciFunction(fixture, 6U);
        fixture.MemoryMappedIo.SetFailure(failureIndex, MemoryMappedIoReadError::HardwareFailure);
        ASTERIA_EXPECT(runner, "an MMIO-failure fixture is ready", fixture.Discover());
        const AhciPciBootstrapBindingFailure failure = fixture.Binding.Prepare(MakeRequest());
        ASTERIA_EXPECT(runner, "each initial HBA register failure propagates and stops its read sequence",
                       failure.Error == AhciPciBootstrapBindingError::SnapshotReadRejected,
                       failure.Snapshot.Error == AhciHbaSnapshotError::MemoryMappedIoReadFailed,
                       failure.Snapshot.MemoryMappedIoRead == MemoryMappedIoReadError::HardwareFailure,
                       failure.Snapshot.Register == registers[failureIndex],
                       fixture.Configuration.GetBar5ReadCount() == 1U,
                       fixture.MemoryMappedIo.GetReadCount() == (failureIndex + 1U));
        CheckNotPublished(runner, fixture.Binding, "an MMIO snapshot failure publishes no descriptor or ABAR fragment");
    }
}

void TestConcurrentAtomicPublication(TestContext& runner) noexcept
{
    BindingFixture fixture;
    AddAhciFunction(fixture, 8U);
    ASTERIA_EXPECT(runner, "the concurrent fixture catalog is ready", fixture.Discover());
    fixture.MemoryMappedIo.PauseFirstRead();

    AhciPciBootstrapBindingFailure prepareFailure{};
    std::thread preparer{[&fixture, &prepareFailure]() noexcept
                         {
                             prepareFailure = fixture.Binding.Prepare(MakeRequest());
                         }};
    fixture.MemoryMappedIo.WaitUntilPaused();

    AhciPciBootstrapBindingResult queryOutput = MakeSentinel();
    AhciPciBootstrapBindingQueryError queryError{AhciPciBootstrapBindingQueryError::NotReady};
    std::atomic<bool> queryStarted{false};
    std::thread query{[&fixture, &queryOutput, &queryError, &queryStarted]() noexcept
                      {
                          queryStarted.store(true);
                          queryError = fixture.Binding.GetResult(queryOutput);
                      }};
    while (!queryStarted.load())
    {
        std::this_thread::yield();
    }
    fixture.MemoryMappedIo.ReleaseFirstRead();
    preparer.join();
    query.join();

    ASTERIA_EXPECT(runner, "a concurrent query observes the whole publication after Prepare commits",
                   prepareFailure.Error == AhciPciBootstrapBindingError::None,
                   queryError == AhciPciBootstrapBindingQueryError::None,
                   HasSuccessfulPublication(queryOutput, MakeAddress(8U)), fixture.MemoryMappedIo.GetReadCount() == 5U);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"AhciPciBootstrapBindingTests"};
    Asteria::Test::TestSuccessfulPublicationAndReadSequence(runner);
    Asteria::Test::TestExactClassAndHeaderFiltering(runner);
    Asteria::Test::TestNoControllerAmbiguityAndCatalogState(runner);
    Asteria::Test::TestBar5TypeAndExtentFailures(runner);
    Asteria::Test::TestSnapshotFailurePropagation(runner);
    Asteria::Test::TestConcurrentAtomicPublication(runner);
    return runner.Finish();
}
