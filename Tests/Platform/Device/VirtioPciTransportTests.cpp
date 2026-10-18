// SPDX-License-Identifier: Apache-2.0
#include "Platform/Device/VirtioPciTransport.hpp"

#include <memory>

#include "Platform/Device/VirtioIommuModel.hpp"
#include "Support/TestContext.hpp"

namespace Asteria::Test
{
namespace
{
using TestSupport::TestContext;
using TestSupport::VirtioFixture;
using TestSupport::Put;
using Platform::Device::VirtioTransportError;
using Platform::Device::VirtioPciTransport;
using Foundation::UInt32;

void TestCapabilities(TestContext& context) noexcept
{
    for (UInt32 mutation{0U}; mutation < 14U; ++mutation)
    {
        auto storage = std::make_unique<VirtioFixture>();
        VirtioFixture& fixture = *storage;
        switch (mutation)
        {
        case 0U:
            break;
        case 1U:
            fixture.Model.Pci[0x41U] = 0x40U;
            break; // Cycle.
        case 2U:
            fixture.Model.Pci[0x41U] = 0x51U;
            break;
        case 3U:
            fixture.Model.Pci[0x42U] = 15U;
            break;
        case 4U:
            fixture.Model.Pci[0x44U] = 6U;
            break;
        case 5U:
            Put(fixture.Model.Pci, 0x48U, 4U, 4090U);
            break;
        case 6U:
            Put(fixture.Model.Pci, 0x4CU, 4U, 55U);
            break;
        case 7U:
            fixture.Model.Pci[0x52U] = 16U;
            break;
        case 8U:
            fixture.Model.Pci[0x43U] = 9U;
            break; // Missing common.
        case 9U:
            Put(fixture.Model.Pci, 0x10U, 4U, fixture.Model.BarBase | 1U);
            break;
        case 10U:
            fixture.Bars[0U].Base += 0x1000U;
            break;
        case 11U:
            fixture.Model.Pci[0x76U] = 255U;
            break;
        case 12U:
            Put(fixture.Model.Pci, 0x10U, 4U, fixture.Model.BarBase | 4U);
            fixture.Model.Pci[0x44U] = 1U;
            break; // High-half BAR.
        case 13U:
            Put(fixture.Model.Pci, 0x58U, 4U, 0x101U);
            break;
        default:
            break;
        }
        ASTERIA_EXPECT(context, "capability walk rejects malformed links, BARs and lengths",
                       fixture.Discover() ==
                           (mutation == 0U ? VirtioTransportError::None : VirtioTransportError::MalformedDevice));
    }
}

void TestIgnoredCapabilities(TestContext& context) noexcept
{
    for (UInt32 mutation{0U}; mutation < 3U; ++mutation)
    {
        auto storage = std::make_unique<VirtioFixture>();
        VirtioFixture& fixture = *storage;
        if (mutation == 0U)
        {
            // Reserved BAR before the valid common cap, with invalid length/extent.
            Put(fixture.Model.Pci, 0x34U, 4U, 0x84U);
            fixture.Model.Cap(0x84U, 0x40U, 0U, 1U, 0xFFFF'FFFFU, 0U);
            fixture.Model.Pci[0x88U] = 6U;
        }
        else
        {
            fixture.Model.Pci[0x75U] = 0x84U;
            fixture.Model.Cap(0x84U, 0U, mutation == 1U ? 20U : 0U, 2U, 0U, 2U);
            fixture.Model.Pci[0x88U] = 1U;
            // QEMU modern-pio-notify: a second notify capability in an I/O BAR.
            Put(fixture.Model.Pci, 0x14U, 4U, 0xC001U);
            fixture.Bars[1U] = {.Base = 0xC000U, .Length = 4U};
        }
        ASTERIA_EXPECT(context, "reserved BAR and already-selected types are ignored without validation",
                       fixture.Discover() == VirtioTransportError::None,
                       fixture.Initialize() == VirtioTransportError::None, fixture.Model.Violations == 0U);
    }
}

void TestFeaturesAndQueues(TestContext& context) noexcept
{
    for (UInt32 mutation{0U}; mutation < 7U; ++mutation)
    {
        auto storage = std::make_unique<VirtioFixture>();
        VirtioFixture& fixture = *storage;
        if (mutation == 1U)
        {
            fixture.Model.Features &= ~VirtioPciTransport::Version1;
        }
        if (mutation == 2U)
        {
            fixture.Model.Features &= ~64ULL;
        }
        if (mutation == 3U)
        {
            fixture.Model.RejectFeatures = true;
        }
        if (mutation == 4U)
        {
            fixture.Model.Queues[0U].Size = 0U;
        }
        if (mutation == 5U)
        {
            fixture.Model.Queues[1U].NotifyOffset = 0xFFFFU;
        }
        if (mutation == 6U)
        {
            fixture.Model.Queues[0U].Size = 3U;
        }
        ASTERIA_CHECK(context, fixture.Discover() == VirtioTransportError::None);
        const VirtioTransportError result = fixture.Initialize();
        ASTERIA_EXPECT(context, "required features and malformed queues are refused",
                       (result == VirtioTransportError::None) == (mutation == 0U));
        if (mutation == 0U)
        {
            ASTERIA_EXPECT(context, "two split rings use caller physical pages and clamped sizes",
                           fixture.Model.Accepted == (VirtioPciTransport::Version1 | 0x47U),
                           fixture.Model.Queues[0U].Size == 2U, fixture.Model.Queues[1U].Size == 8U,
                           fixture.Model.Queues[0U].Enabled == 1U,
                           fixture.Model.Queues[0U].Addresses[0U] == fixture.Model.RamBase,
                           fixture.Model.Queues[1U].Addresses[2U] == fixture.Model.RamBase + 5U * 4096U,
                           fixture.Model.Available(1U) == 8U, fixture.Model.Violations == 0U);
        }
    }
    auto storage = std::make_unique<VirtioFixture>();
    VirtioFixture& fixture = *storage;
    ASTERIA_CHECK(context, fixture.Discover() == VirtioTransportError::None);
    ASTERIA_CHECK(context, fixture.Transport.Reset(8U) == VirtioTransportError::None);
    ASTERIA_CHECK(context, fixture.Transport.Negotiate(1ULL << 34U, 0U) == VirtioTransportError::UnsupportedFeatures);
    ASTERIA_CHECK(context, fixture.Transport.Negotiate(0U, 0U) == VirtioTransportError::None);
    auto pages = fixture.Page(0U);
    const Platform::Device::VirtioDmaPage overlap[3U]{pages, pages, fixture.Page(2U)};
    ASTERIA_CHECK(context, fixture.Transport.SetupQueue(0U, overlap, 8U) == VirtioTransportError::InvalidArguments);
}

void TestResetAndUsedValidation(TestContext& context) noexcept
{
    auto resetStorage = std::make_unique<VirtioFixture>();
    VirtioFixture& reset = *resetStorage;
    ASTERIA_CHECK(context, reset.Discover() == VirtioTransportError::None);
    reset.Model.StuckReset = true;
    ASTERIA_EXPECT(context, "reset wait has a precise finite bound",
                   reset.Transport.Reset(5U) == VirtioTransportError::TimedOut, reset.Model.StatusReads == 5U);
    for (UInt32 mutation{0U}; mutation < 3U; ++mutation)
    {
        auto storage = std::make_unique<VirtioFixture>();
        VirtioFixture& fixture = *storage;
        ASTERIA_CHECK(context, fixture.Discover() == VirtioTransportError::None);
        ASTERIA_CHECK(context, fixture.Initialize() == VirtioTransportError::None);
        ASTERIA_CHECK(context, fixture.Device.ClearBypass() == VirtioTransportError::None);
        ASTERIA_CHECK(context,
                      fixture.Backend.Claim(fixture.Owner, 32U) == Hal::Device::IommuBackendOwnershipError::None);
        if (mutation == 0U)
        {
            fixture.Model.BadUsedId = true;
        }
        if (mutation == 1U)
        {
            fixture.Model.BadUsedLength = true;
        }
        if (mutation == 2U)
        {
            fixture.Model.BadUsedAdvance = true;
        }
        ASTERIA_EXPECT(context, "bad used IDs, lengths and index advances poison the transport",
                       fixture.Backend.Attach(fixture.Endpoint) == Hal::Device::IommuBackendError::CorruptState,
                       !fixture.Transport.IsReady());
    }
}
} // namespace
} // namespace Asteria::Test

int main()
{
    Asteria::TestSupport::TestContext context{"VirtioPciTransportTests"};
    Asteria::Test::TestCapabilities(context);
    Asteria::Test::TestIgnoredCapabilities(context);
    Asteria::Test::TestFeaturesAndQueues(context);
    Asteria::Test::TestResetAndUsedValidation(context);
    return context.Finish();
}
