// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Boot/Uefi/PciIoProtocol.hpp"
#include "Platform/Firmware/AhciPciResourceCapture.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Boot::Uefi::BootServices;
using Boot::Uefi::Guid;
using Boot::Uefi::Handle;
using Boot::Uefi::LocateSearchType;
using Boot::Uefi::PciIoProtocol;
using Boot::Uefi::PciIoWidth;
using Boot::Uefi::Status;
using Foundation::Size;
using Foundation::UInt8;
using Foundation::UInt16;
using Foundation::UInt32;
using Foundation::UInt64;
using Platform::Firmware::AhciPciConfigurationRegister;
using Platform::Firmware::AhciPciResource;
using Platform::Firmware::AhciPciResourceCapture;
using Platform::Firmware::AhciPciResourceCaptureError;
using Platform::Firmware::AhciPciResourceCaptureFailure;
using Platform::Firmware::AhciPciResourceCleanupError;
using Platform::Firmware::AhciPciResourceDescriptorError;

constexpr Status TestFailureStatus{Boot::Uefi::ErrorBit | 0x1234U};
constexpr UInt64 AbarBase{0xFEBF'0000U};
constexpr UInt64 AbarLength{0x1000U};

enum class FirmwareEvent : UInt8
{
    Locate,
    HandleProtocol,
    GetLocation,
    ReadIdentifier,
    ReadClass,
    ReadHeader,
    GetBarAttributes,
    FreeDescriptor,
    FreeHandles,
};

struct FakePciDevice
{
    PciIoProtocol Protocol{};
    Status HandleStatus{Boot::Uefi::Success};
    Status LocationStatus{Boot::Uefi::Success};
    Status IdentifierStatus{Boot::Uefi::Success};
    Status ClassStatus{Boot::Uefi::Success};
    Status HeaderStatus{Boot::Uefi::Success};
    Status BarStatus{Boot::Uefi::Success};
    Size Segment{0U};
    Size Bus{0U};
    Size Device{0U};
    Size Function{0U};
    UInt32 Identifier{0x5678'1234U};
    UInt32 ClassRegister{0x0106'0101U};
    UInt32 HeaderRegister{0U};
    UInt8 Resources[48U]{};
    bool NullInterface{false};
    bool NullResources{false};
};

class FirmwareFixture;
FirmwareFixture* g_activeFixture{nullptr};

class FirmwareFixture final
{
public:
    static constexpr Size MaximumDevices{64U};
    static constexpr Size MaximumEvents{512U};

    FirmwareFixture() noexcept
    {
        Services.FreePoolFunction = FreePool;
        Services.HandleProtocolFunction = HandleProtocol;
        Services.LocateHandleBufferFunction = LocateHandleBuffer;
        AddAhciDevice();
    }

    void Reset(const Size deviceCount = 1U) noexcept
    {
        *this = FirmwareFixture{NoInitializeTag{}};
        for (Size index{0U}; index < deviceCount; ++index)
        {
            AddAhciDevice();
        }
    }

    void AddAhciDevice() noexcept
    {
        if (DeviceCount >= MaximumDevices)
        {
            return;
        }
        FakePciDevice& device = Devices[DeviceCount];
        device.Protocol.Pci.Read = ReadConfiguration;
        device.Protocol.GetLocation = GetLocation;
        device.Protocol.GetBarAttributes = GetBarAttributes;
        device.Device = DeviceCount & 31U;
        device.Bus = DeviceCount >> 5U;
        InitializeResource(device.Resources, AbarBase, AbarLength);
        Handles[DeviceCount] = static_cast<Handle>(&device);
        ++DeviceCount;
    }

    [[nodiscard]] bool EventsEqual(const FirmwareEvent* const expected, const Size count) const noexcept
    {
        if (EventCount != count)
        {
            return false;
        }
        for (Size index{0U}; index < count; ++index)
        {
            if (Events[index] != expected[index])
            {
                return false;
            }
        }
        return true;
    }

    void Record(const FirmwareEvent event) noexcept
    {
        if (EventCount < MaximumEvents)
        {
            Events[EventCount] = event;
            ++EventCount;
        }
    }

    [[nodiscard]] FakePciDevice* FindProtocol(PciIoProtocol* const protocol) noexcept
    {
        for (Size index{0U}; index < DeviceCount; ++index)
        {
            if (&Devices[index].Protocol == protocol)
            {
                return &Devices[index];
            }
        }
        return nullptr;
    }

    [[nodiscard]] static bool IsProtocolGuid(const Guid& guid) noexcept
    {
        const Guid& expected = Boot::Uefi::PciIoProtocolGuid;
        if ((guid.Data1 != expected.Data1) || (guid.Data2 != expected.Data2) || (guid.Data3 != expected.Data3))
        {
            return false;
        }
        for (Size index{0U}; index < sizeof(guid.Data4); ++index)
        {
            if (guid.Data4[index] != expected.Data4[index])
            {
                return false;
            }
        }
        return true;
    }

    static void InitializeResource(UInt8 (&resources)[48U], const UInt64 baseAddress, const UInt64 lengthBytes) noexcept
    {
        for (UInt8& value : resources)
        {
            value = 0U;
        }
        resources[0U] = 0x8AU;
        resources[1U] = 0x2BU;
        resources[2U] = 0U;
        WriteLittleEndian64(resources + 6U, 32U);
        WriteLittleEndian64(resources + 14U, baseAddress);
        WriteLittleEndian64(resources + 22U, 0x1111'2222'3333'4444ULL);
        WriteLittleEndian64(resources + 30U, 0U);
        WriteLittleEndian64(resources + 38U, lengthBytes);
        resources[46U] = 0x79U;
        resources[47U] = 0U;
    }

    static void WriteLittleEndian64(UInt8* const bytes, const UInt64 value) noexcept
    {
        for (UInt32 index{0U}; index < 8U; ++index)
        {
            bytes[index] = static_cast<UInt8>(value >> (index * 8U));
        }
    }

    static Status ASTERIA_UEFI_API LocateHandleBuffer(const LocateSearchType searchType, Guid* const protocol,
                                                      void* const searchKey, Size* const handleCount,
                                                      Handle** const handles) noexcept
    {
        FirmwareFixture& fixture = *g_activeFixture;
        fixture.Record(FirmwareEvent::Locate);
        fixture.CallContractValid = fixture.CallContractValid && (searchType == LocateSearchType::ByProtocol) &&
                                    (protocol != nullptr) && IsProtocolGuid(*protocol) && (searchKey == nullptr) &&
                                    (handleCount != nullptr) && (handles != nullptr);
        if ((fixture.LocateStatus == Boot::Uefi::Success) || fixture.ReturnHandlesOnLocateFailure)
        {
            *handleCount = fixture.LocateCountOverrideEnabled ? fixture.LocateCountOverride : fixture.DeviceCount;
            *handles = fixture.NullHandleBuffer ? nullptr : fixture.Handles;
        }
        return fixture.LocateStatus;
    }

    static Status ASTERIA_UEFI_API HandleProtocol(const Handle handle, Guid* const protocol,
                                                  void** const interface) noexcept
    {
        FirmwareFixture& fixture = *g_activeFixture;
        fixture.Record(FirmwareEvent::HandleProtocol);
        fixture.CallContractValid = fixture.CallContractValid && (handle != nullptr) && (protocol != nullptr) &&
                                    IsProtocolGuid(*protocol) && (interface != nullptr);
        auto* const device = static_cast<FakePciDevice*>(handle);
        if (device->HandleStatus == Boot::Uefi::Success)
        {
            *interface = device->NullInterface ? nullptr : static_cast<void*>(&device->Protocol);
        }
        return device->HandleStatus;
    }

    static Status ASTERIA_UEFI_API GetLocation(PciIoProtocol* const protocol, Size* const segment, Size* const bus,
                                               Size* const deviceNumber, Size* const function) noexcept
    {
        FirmwareFixture& fixture = *g_activeFixture;
        fixture.Record(FirmwareEvent::GetLocation);
        FakePciDevice* const device = fixture.FindProtocol(protocol);
        fixture.CallContractValid = fixture.CallContractValid && (device != nullptr) && (segment != nullptr) &&
                                    (bus != nullptr) && (deviceNumber != nullptr) && (function != nullptr);
        if ((device != nullptr) && (device->LocationStatus == Boot::Uefi::Success))
        {
            *segment = device->Segment;
            *bus = device->Bus;
            *deviceNumber = device->Device;
            *function = device->Function;
        }
        return device == nullptr ? Boot::Uefi::InvalidParameter : device->LocationStatus;
    }

    static Status ASTERIA_UEFI_API ReadConfiguration(PciIoProtocol* const protocol, const PciIoWidth width,
                                                     const UInt32 offset, const Size count, void* const buffer) noexcept
    {
        FirmwareFixture& fixture = *g_activeFixture;
        FakePciDevice* const device = fixture.FindProtocol(protocol);
        fixture.CallContractValid = fixture.CallContractValid && (device != nullptr) && (width == PciIoWidth::Uint32) &&
                                    (count == 1U) && (buffer != nullptr);
        if (device == nullptr)
        {
            return Boot::Uefi::InvalidParameter;
        }

        UInt32 value{0U};
        Status status{Boot::Uefi::InvalidParameter};
        if (offset == 0x00U)
        {
            fixture.Record(FirmwareEvent::ReadIdentifier);
            value = device->Identifier;
            status = device->IdentifierStatus;
        }
        else if (offset == 0x08U)
        {
            fixture.Record(FirmwareEvent::ReadClass);
            value = device->ClassRegister;
            status = device->ClassStatus;
        }
        else if (offset == 0x0CU)
        {
            fixture.Record(FirmwareEvent::ReadHeader);
            value = device->HeaderRegister;
            status = device->HeaderStatus;
        }
        else
        {
            fixture.CallContractValid = false;
        }
        if (status == Boot::Uefi::Success)
        {
            *static_cast<UInt32*>(buffer) = value;
        }
        return status;
    }

    static Status ASTERIA_UEFI_API GetBarAttributes(PciIoProtocol* const protocol, const UInt8 barIndex,
                                                    UInt64* const supports, void** const resources) noexcept
    {
        FirmwareFixture& fixture = *g_activeFixture;
        fixture.Record(FirmwareEvent::GetBarAttributes);
        FakePciDevice* const device = fixture.FindProtocol(protocol);
        fixture.LastBarIndex = barIndex;
        fixture.CallContractValid = fixture.CallContractValid && (device != nullptr) && (barIndex == 5U) &&
                                    (supports == nullptr) && (resources != nullptr);
        if ((device != nullptr) && (device->BarStatus == Boot::Uefi::Success))
        {
            *resources = device->NullResources ? nullptr : static_cast<void*>(device->Resources);
        }
        return device == nullptr ? Boot::Uefi::InvalidParameter : device->BarStatus;
    }

    static Status ASTERIA_UEFI_API FreePool(void* const buffer) noexcept
    {
        FirmwareFixture& fixture = *g_activeFixture;
        if (buffer == static_cast<void*>(fixture.Handles))
        {
            fixture.Record(FirmwareEvent::FreeHandles);
            return fixture.HandleFreeStatus;
        }
        for (Size index{0U}; index < fixture.DeviceCount; ++index)
        {
            if (buffer == static_cast<void*>(fixture.Devices[index].Resources))
            {
                fixture.Record(FirmwareEvent::FreeDescriptor);
                return fixture.DescriptorFreeStatus;
            }
        }
        fixture.CallContractValid = false;
        return Boot::Uefi::InvalidParameter;
    }

    BootServices Services{};
    FakePciDevice Devices[MaximumDevices]{};
    Handle Handles[MaximumDevices]{};
    FirmwareEvent Events[MaximumEvents]{};
    Size DeviceCount{0U};
    Size EventCount{0U};
    Status LocateStatus{Boot::Uefi::Success};
    Status DescriptorFreeStatus{Boot::Uefi::Success};
    Status HandleFreeStatus{Boot::Uefi::Success};
    Size LocateCountOverride{0U};
    UInt8 LastBarIndex{0xFFU};
    bool ReturnHandlesOnLocateFailure{false};
    bool LocateCountOverrideEnabled{false};
    bool NullHandleBuffer{false};
    bool CallContractValid{true};

private:
    struct NoInitializeTag final
    {
    };

    explicit FirmwareFixture(NoInitializeTag) noexcept
    {
        Services.FreePoolFunction = FreePool;
        Services.HandleProtocolFunction = HandleProtocol;
        Services.LocateHandleBufferFunction = LocateHandleBuffer;
    }
};

class ActiveFixtureGuard final
{
public:
    explicit ActiveFixtureGuard(FirmwareFixture& fixture) noexcept
    {
        g_activeFixture = &fixture;
    }

    ~ActiveFixtureGuard() noexcept
    {
        g_activeFixture = nullptr;
    }

    ActiveFixtureGuard(const ActiveFixtureGuard&) = delete;
    ActiveFixtureGuard& operator=(const ActiveFixtureGuard&) = delete;
    ActiveFixtureGuard(ActiveFixtureGuard&&) = delete;
    ActiveFixtureGuard& operator=(ActiveFixtureGuard&&) = delete;
};

[[nodiscard]] constexpr AhciPciResource MakeSentinel() noexcept
{
    return AhciPciResource{
        .AbarBaseAddress = 0x1111'2222'3333'4444ULL,
        .AbarLengthBytes = 0x5555'6666'7777'8888ULL,
        .Segment = 0x9999U,
        .Bus = 0xAAU,
        .Device = 0xBBU,
        .Function = 0xCCU,
        .BarIndex = 1U,
        .Reserved = {2U, 3U},
    };
}

[[nodiscard]] constexpr bool IsSentinel(const AhciPciResource& resource) noexcept
{
    return (resource.AbarBaseAddress == 0x1111'2222'3333'4444ULL) &&
           (resource.AbarLengthBytes == 0x5555'6666'7777'8888ULL) && (resource.Segment == 0x9999U) &&
           (resource.Bus == 0xAAU) && (resource.Device == 0xBBU) && (resource.Function == 0xCCU) &&
           (resource.BarIndex == 1U) && (resource.Reserved[1U] == 3U);
}

[[nodiscard]] AhciPciResourceCaptureFailure Capture(FirmwareFixture& fixture, AhciPciResource& output) noexcept
{
    ActiveFixtureGuard guard{fixture};
    return AhciPciResourceCapture::Capture(fixture.Services, output);
}

void TestUniqueSelectionAndCallOrder(TestContext& runner) noexcept
{
    FirmwareFixture fixture;
    fixture.Reset(45U);
    for (Size index{0U}; index < fixture.DeviceCount; ++index)
    {
        fixture.Devices[index].Identifier = 0xFFFF'FFFFU;
    }
    fixture.Devices[1U].Identifier = 0x2222'1111U;
    fixture.Devices[1U].ClassRegister = 0x0206'0101U;
    fixture.Devices[2U].Identifier = 0x4444'3333U;
    fixture.Devices[2U].ClassRegister = 0x0105'0101U;
    fixture.Devices[3U].Identifier = 0x6666'5555U;
    fixture.Devices[3U].ClassRegister = 0x0106'0001U;
    fixture.Devices[4U].Identifier = 0x8888'7777U;
    fixture.Devices[4U].HeaderRegister = 1U << 16U;
    fixture.Devices[44U].Identifier = 0xAAAA'9999U;
    fixture.Devices[44U].Segment = 0x1234U;
    fixture.Devices[44U].Bus = 0xABU;
    fixture.Devices[44U].Device = 31U;
    fixture.Devices[44U].Function = 7U;

    AhciPciResource output = MakeSentinel();
    const AhciPciResourceCaptureFailure failure = Capture(fixture, output);
    const FirmwareEvent finalEvents[3U]{
        FirmwareEvent::GetBarAttributes,
        FirmwareEvent::FreeDescriptor,
        FirmwareEvent::FreeHandles,
    };
    bool finalOrder = fixture.EventCount >= 3U;
    for (Size index{0U}; index < 3U; ++index)
    {
        finalOrder = finalOrder && (fixture.Events[fixture.EventCount - 3U + index] == finalEvents[index]);
    }
    ASTERIA_EXPECT(runner, "all handles are searchable without a production cap and only exact BAR5 is captured",
                   failure.Error == AhciPciResourceCaptureError::None, failure.MatchingControllerCount == 1U,
                   failure.HandleIndex == 44U, failure.Descriptor == AhciPciResourceDescriptorError::None,
                   failure.Cleanup == AhciPciResourceCleanupError::None, output.AbarBaseAddress == AbarBase,
                   output.AbarLengthBytes == AbarLength, output.Segment == 0x1234U, output.Bus == 0xABU,
                   output.Device == 31U, output.Function == 7U, output.BarIndex == 5U, output.Reserved[0U] == 0U,
                   fixture.CallContractValid, fixture.LastBarIndex == 5U, finalOrder);
}

void TestNoneAndAmbiguous(TestContext& runner) noexcept
{
    FirmwareFixture none;
    none.Reset(3U);
    none.Devices[0U].Identifier = 0xFFFF'FFFFU;
    none.Devices[1U].ClassRegister = 0x0106'0001U;
    none.Devices[2U].HeaderRegister = 1U << 16U;
    AhciPciResource output = MakeSentinel();
    const auto absent = Capture(none, output);
    ASTERIA_EXPECT(runner, "absence and near matches do not call GetBarAttributes or publish",
                   absent.Error == AhciPciResourceCaptureError::NoAhciController, absent.MatchingControllerCount == 0U,
                   none.LastBarIndex == 0xFFU, none.Events[none.EventCount - 1U] == FirmwareEvent::FreeHandles,
                   IsSentinel(output));

    FirmwareFixture ambiguous;
    ambiguous.Reset(3U);
    ambiguous.Devices[2U].ClassRegister = 0x0200'0001U;
    const auto multiple = Capture(ambiguous, output);
    ASTERIA_EXPECT(runner, "two exact endpoints are rejected before any BAR resource query",
                   multiple.Error == AhciPciResourceCaptureError::AmbiguousAhciController,
                   multiple.MatchingControllerCount == 2U, ambiguous.LastBarIndex == 0xFFU,
                   ambiguous.Events[ambiguous.EventCount - 1U] == FirmwareEvent::FreeHandles, IsSentinel(output));

    FirmwareFixture noHandles;
    noHandles.Reset(0U);
    noHandles.LocateStatus = Boot::Uefi::NotFound;
    const auto locatedNone = Capture(noHandles, output);
    ASTERIA_EXPECT(runner, "LocateHandleBuffer NotFound is typed absence with its exact status",
                   locatedNone.Error == AhciPciResourceCaptureError::NoAhciController,
                   locatedNone.LocateHandleBufferStatus == Boot::Uefi::NotFound, noHandles.EventCount == 1U,
                   IsSentinel(output));
}

void TestFirmwareFailurePropagation(TestContext& runner) noexcept
{
    AhciPciResource output = MakeSentinel();

    FirmwareFixture locate;
    locate.LocateStatus = TestFailureStatus;
    locate.ReturnHandlesOnLocateFailure = true;
    const auto locateFailure = Capture(locate, output);
    ASTERIA_EXPECT(runner, "LocateHandleBuffer failure status is retained and returned storage is cleaned",
                   locateFailure.Error == AhciPciResourceCaptureError::LocateHandleBufferFailed,
                   locateFailure.LocateHandleBufferStatus == TestFailureStatus,
                   locate.Events[locate.EventCount - 1U] == FirmwareEvent::FreeHandles, IsSentinel(output));

    FirmwareFixture handle;
    handle.Devices[0U].HandleStatus = TestFailureStatus;
    const auto handleFailure = Capture(handle, output);
    ASTERIA_EXPECT(runner, "HandleProtocol failure status is retained",
                   handleFailure.Error == AhciPciResourceCaptureError::HandleProtocolFailed,
                   handleFailure.HandleProtocolStatus == TestFailureStatus, IsSentinel(output));

    FirmwareFixture location;
    location.Devices[0U].LocationStatus = TestFailureStatus;
    const auto locationFailure = Capture(location, output);
    ASTERIA_EXPECT(runner, "GetLocation failure status is retained",
                   locationFailure.Error == AhciPciResourceCaptureError::GetLocationFailed,
                   locationFailure.GetLocationStatus == TestFailureStatus, IsSentinel(output));

    constexpr AhciPciConfigurationRegister registers[3U]{
        AhciPciConfigurationRegister::Identifier,
        AhciPciConfigurationRegister::ClassCode,
        AhciPciConfigurationRegister::HeaderType,
    };
    for (Size index{0U}; index < 3U; ++index)
    {
        FirmwareFixture pciRead;
        Status* const statuses[3U]{
            &pciRead.Devices[0U].IdentifierStatus,
            &pciRead.Devices[0U].ClassStatus,
            &pciRead.Devices[0U].HeaderStatus,
        };
        *statuses[index] = TestFailureStatus + index;
        const auto readFailure = Capture(pciRead, output);
        ASTERIA_EXPECT(runner, "each PCI configuration read retains its exact failure phase and status",
                       readFailure.Error == AhciPciResourceCaptureError::ConfigurationReadFailed,
                       readFailure.ConfigurationReadStatus == (TestFailureStatus + index),
                       readFailure.ConfigurationRegister == registers[index], IsSentinel(output));
    }

    FirmwareFixture bar;
    bar.Devices[0U].BarStatus = TestFailureStatus;
    const auto barFailure = Capture(bar, output);
    ASTERIA_EXPECT(runner, "GetBarAttributes failure retains status and the exact requested BAR index",
                   barFailure.Error == AhciPciResourceCaptureError::GetBarAttributesFailed,
                   barFailure.GetBarAttributesStatus == TestFailureStatus, barFailure.BarIndex == 5U,
                   IsSentinel(output));
}

void TestInvalidFirmwareOutputs(TestContext& runner) noexcept
{
    AhciPciResource output = MakeSentinel();
    FirmwareFixture nullHandles;
    nullHandles.NullHandleBuffer = true;
    const auto invalidHandles = Capture(nullHandles, output);
    ASTERIA_EXPECT(runner, "a successful locate with a null handle buffer is rejected atomically",
                   invalidHandles.Error == AhciPciResourceCaptureError::InvalidHandleBuffer, IsSentinel(output));

    FirmwareFixture nullInterface;
    nullInterface.Devices[0U].NullInterface = true;
    const auto invalidInterface = Capture(nullInterface, output);
    ASTERIA_EXPECT(runner, "a null PCI I/O interface is rejected",
                   invalidInterface.Error == AhciPciResourceCaptureError::InvalidPciIoProtocol, IsSentinel(output));

    FirmwareFixture invalidLocation;
    invalidLocation.Devices[0U].Device = 32U;
    const auto location = Capture(invalidLocation, output);
    ASTERIA_EXPECT(runner, "an out-of-range BDF is rejected before configuration access",
                   location.Error == AhciPciResourceCaptureError::InvalidLocation, IsSentinel(output));

    FirmwareFixture nullResources;
    nullResources.Devices[0U].NullResources = true;
    const auto resources = Capture(nullResources, output);
    ASTERIA_EXPECT(runner, "a successful BAR query cannot publish a null resource list",
                   resources.Error == AhciPciResourceCaptureError::InvalidBarResourceBuffer, IsSentinel(output));

    FirmwareFixture missingCallback;
    missingCallback.Services.FreePoolFunction = nullptr;
    const auto invalidConfiguration = Capture(missingCallback, output);
    ASTERIA_EXPECT(runner, "required boot-service callbacks are validated before firmware calls",
                   invalidConfiguration.Error == AhciPciResourceCaptureError::InvalidConfiguration,
                   missingCallback.EventCount == 0U, IsSentinel(output));

    FirmwareFixture missingLocation;
    missingLocation.Devices[0U].Protocol.GetLocation = nullptr;
    const auto locationCallback = Capture(missingLocation, output);
    ASTERIA_EXPECT(runner, "a missing GetLocation callback is rejected before PCI reads",
                   locationCallback.Error == AhciPciResourceCaptureError::InvalidPciIoProtocol, IsSentinel(output));

    FirmwareFixture missingRead;
    missingRead.Devices[0U].Protocol.Pci.Read = nullptr;
    const auto readCallback = Capture(missingRead, output);
    ASTERIA_EXPECT(runner, "a missing PCI configuration read callback is rejected",
                   readCallback.Error == AhciPciResourceCaptureError::InvalidPciIoProtocol, IsSentinel(output));

    FirmwareFixture missingBar;
    missingBar.Devices[0U].Protocol.GetBarAttributes = nullptr;
    const auto barCallback = Capture(missingBar, output);
    ASTERIA_EXPECT(runner, "a missing BAR callback is rejected only after unique selection",
                   barCallback.Error == AhciPciResourceCaptureError::InvalidPciIoProtocol,
                   missingBar.LastBarIndex == 0xFFU, IsSentinel(output));
}

void TestDescriptorValidationAndRetry(TestContext& runner) noexcept
{
    constexpr AhciPciResourceDescriptorError expected[9U]{
        AhciPciResourceDescriptorError::InvalidDescriptor,
        AhciPciResourceDescriptorError::InvalidDescriptorLength,
        AhciPciResourceDescriptorError::NotMemory,
        AhciPciResourceDescriptorError::UnsupportedGranularity,
        AhciPciResourceDescriptorError::UnsupportedTranslation,
        AhciPciResourceDescriptorError::InvalidExtent,
        AhciPciResourceDescriptorError::InvalidExtent,
        AhciPciResourceDescriptorError::InvalidEndTag,
        AhciPciResourceDescriptorError::InvalidChecksum,
    };
    for (Size index{0U}; index < 9U; ++index)
    {
        FirmwareFixture fixture;
        UInt8* const bytes = fixture.Devices[0U].Resources;
        switch (index)
        {
        case 0U:
            bytes[0U] = 0x87U;
            break;
        case 1U:
            bytes[1U] = 0x2AU;
            break;
        case 2U:
            bytes[3U] = 1U;
            break;
        case 3U:
            FirmwareFixture::WriteLittleEndian64(bytes + 6U, 64U);
            break;
        case 4U:
            FirmwareFixture::WriteLittleEndian64(bytes + 30U, 1U);
            break;
        case 5U:
            FirmwareFixture::WriteLittleEndian64(bytes + 38U, 0U);
            break;
        case 6U:
            FirmwareFixture::WriteLittleEndian64(bytes + 14U, ~UInt64{0U});
            FirmwareFixture::WriteLittleEndian64(bytes + 38U, 2U);
            break;
        case 7U:
            bytes[46U] = 0x78U;
            break;
        case 8U:
            bytes[47U] = 1U;
            break;
        default:
            break;
        }
        AhciPciResource output = MakeSentinel();
        const auto failure = Capture(fixture, output);
        ASTERIA_EXPECT(runner, "each malformed QWORD resource or End Tag is rejected after ordered cleanup",
                       failure.Error == AhciPciResourceCaptureError::BarResourceDescriptorRejected,
                       failure.Descriptor == expected[index],
                       fixture.Events[fixture.EventCount - 2U] == FirmwareEvent::FreeDescriptor,
                       fixture.Events[fixture.EventCount - 1U] == FirmwareEvent::FreeHandles, IsSentinel(output));
    }

    FirmwareFixture retry;
    retry.Devices[0U].Resources[0U] = 0x87U;
    AhciPciResource output = MakeSentinel();
    const auto failed = Capture(retry, output);
    retry.EventCount = 0U;
    FirmwareFixture::InitializeResource(retry.Devices[0U].Resources, AbarBase, AbarLength);
    const auto succeeded = Capture(retry, output);
    ASTERIA_EXPECT(runner, "a failed atomic capture can be retried on the same caller-owned output",
                   failed.Error == AhciPciResourceCaptureError::BarResourceDescriptorRejected,
                   succeeded.Error == AhciPciResourceCaptureError::None, output.AbarBaseAddress == AbarBase,
                   output.AbarLengthBytes == AbarLength);

    FirmwareFixture ignoredMaximum;
    FirmwareFixture::WriteLittleEndian64(ignoredMaximum.Devices[0U].Resources + 22U, 0U);
    output = MakeSentinel();
    const auto maximum = Capture(ignoredMaximum, output);
    ASTERIA_EXPECT(runner, "Address Range Maximum is not used as BAR geometry authority",
                   maximum.Error == AhciPciResourceCaptureError::None, output.AbarBaseAddress == AbarBase,
                   output.AbarLengthBytes == AbarLength);

    FirmwareFixture checkedEndTag;
    UInt8 checksum{0U};
    for (Size index{0U}; index < 47U; ++index)
    {
        checksum = static_cast<UInt8>(checksum + checkedEndTag.Devices[0U].Resources[index]);
    }
    checkedEndTag.Devices[0U].Resources[47U] = static_cast<UInt8>(0U - checksum);
    output = MakeSentinel();
    const auto checked = Capture(checkedEndTag, output);
    ASTERIA_EXPECT(runner, "a present End Tag checksum is accepted only when the complete template sums to zero",
                   checked.Error == AhciPciResourceCaptureError::None, output.AbarBaseAddress == AbarBase,
                   output.AbarLengthBytes == AbarLength);
}

void TestCleanupFailures(TestContext& runner) noexcept
{
    constexpr AhciPciResourceCleanupError cleanupErrors[3U]{
        AhciPciResourceCleanupError::DescriptorBuffer,
        AhciPciResourceCleanupError::HandleBuffer,
        AhciPciResourceCleanupError::DescriptorAndHandleBuffers,
    };
    for (Size index{0U}; index < 3U; ++index)
    {
        FirmwareFixture fixture;
        if ((index == 0U) || (index == 2U))
        {
            fixture.DescriptorFreeStatus = TestFailureStatus;
        }
        if ((index == 1U) || (index == 2U))
        {
            fixture.HandleFreeStatus = TestFailureStatus + 1U;
        }
        AhciPciResource output = MakeSentinel();
        const auto failure = Capture(fixture, output);
        ASTERIA_EXPECT(runner, "every descriptor and handle cleanup failure is retained without publication",
                       failure.Error == AhciPciResourceCaptureError::CleanupFailed,
                       failure.Cleanup == cleanupErrors[index],
                       failure.DescriptorFreeStatus ==
                           (((index == 0U) || (index == 2U)) ? TestFailureStatus : Boot::Uefi::Success),
                       failure.HandleBufferFreeStatus ==
                           (((index == 1U) || (index == 2U)) ? TestFailureStatus + 1U : Boot::Uefi::Success),
                       fixture.Events[fixture.EventCount - 2U] == FirmwareEvent::FreeDescriptor,
                       fixture.Events[fixture.EventCount - 1U] == FirmwareEvent::FreeHandles, IsSentinel(output));
    }

    FirmwareFixture primaryAndCleanup;
    primaryAndCleanup.Devices[0U].Resources[0U] = 0U;
    primaryAndCleanup.DescriptorFreeStatus = TestFailureStatus;
    primaryAndCleanup.HandleFreeStatus = TestFailureStatus + 1U;
    AhciPciResource output = MakeSentinel();
    const auto failure = Capture(primaryAndCleanup, output);
    ASTERIA_EXPECT(runner, "primary parse failure and both cleanup statuses remain independently typed",
                   failure.Error == AhciPciResourceCaptureError::BarResourceDescriptorRejected,
                   failure.Descriptor == AhciPciResourceDescriptorError::InvalidDescriptor,
                   failure.Cleanup == AhciPciResourceCleanupError::DescriptorAndHandleBuffers, IsSentinel(output));
}

// A capture by identifier selects the one endpoint whose identifier register matches, whatever its
// class, and reads the BAR it names; no match is NoAhciController, a bad request is refused before
// firmware is called.
void TestCaptureByIdentifier(TestContext& runner) noexcept
{
    FirmwareFixture fixture;
    fixture.Reset(3U);
    fixture.Devices[1U].Identifier = 0x11E8'1234U;
    fixture.Devices[1U].ClassRegister = 0x00FF'0000U;
    fixture.Devices[1U].Function = 0U;
    AhciPciResource output = MakeSentinel();
    AhciPciResourceCaptureFailure failure{};
    AhciPciResourceCaptureFailure absent{};
    AhciPciResource untouched = MakeSentinel();
    {
        ActiveFixtureGuard guard{fixture};
        failure = AhciPciResourceCapture::CaptureByIdentifier(fixture.Services, 0x11E8'1234U, 0U, output);
    }
    const UInt8 barIndex = fixture.LastBarIndex;
    fixture.Reset(2U);
    {
        ActiveFixtureGuard guard{fixture};
        absent = AhciPciResourceCapture::CaptureByIdentifier(fixture.Services, 0x11E8'1234U, 0U, untouched);
    }
    const auto unnamed = AhciPciResourceCapture::CaptureByIdentifier(fixture.Services, 0U, 0U, untouched);
    const auto beyond = AhciPciResourceCapture::CaptureByIdentifier(fixture.Services, 0x11E8'1234U, 6U, untouched);
    ASTERIA_EXPECT(runner, "a capture by identifier selects that endpoint and reads the BAR it names",
                   failure.Error == AhciPciResourceCaptureError::None, failure.MatchingControllerCount == 1U,
                   output.AbarBaseAddress == AbarBase, output.AbarLengthBytes == AbarLength, output.Device == 1U,
                   output.BarIndex == 0U, barIndex == 0U, absent.Error == AhciPciResourceCaptureError::NoAhciController,
                   unnamed.Error == AhciPciResourceCaptureError::InvalidConfiguration,
                   beyond.Error == AhciPciResourceCaptureError::InvalidConfiguration, IsSentinel(untouched));
}
} // Namespace
} // Namespace Asteria::Test

int main()
{
    TestContext runner{"AhciPciResourceCaptureTests"};
    Asteria::Test::TestUniqueSelectionAndCallOrder(runner);
    Asteria::Test::TestNoneAndAmbiguous(runner);
    Asteria::Test::TestFirmwareFailurePropagation(runner);
    Asteria::Test::TestInvalidFirmwareOutputs(runner);
    Asteria::Test::TestDescriptorValidationAndRetry(runner);
    Asteria::Test::TestCleanupFailures(runner);
    Asteria::Test::TestCaptureByIdentifier(runner);
    return runner.Finish();
}
