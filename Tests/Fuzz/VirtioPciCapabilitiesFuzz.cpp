// SPDX-License-Identifier: Apache-2.0
// Capability discovery must be bounded, read-only, and never accept windows
// outside retained BAR authority. Replay includes a valid modern function and
// malformed link/length seeds; sanitizers check arbitrary mutated config bytes.
#include "Platform/Device/VirtioIommuModel.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>

extern "C" void AsteriaFuzzSeeds(void (*const emit)(const std::uint8_t* data, std::size_t size))
{
    Asteria::TestSupport::VirtioIommuModel model;
    emit(model.Pci, sizeof(model.Pci));
    model.Pci[0x41U] = 0x40U;
    emit(model.Pci, sizeof(model.Pci));
    model.Pci[0x42U] = 255U;
    emit(model.Pci, sizeof(model.Pci));
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* const data, const std::size_t size)
{
    Asteria::TestSupport::VirtioIommuModel model;
    for (std::size_t index{0U}; index < sizeof(model.Pci); ++index)
    {
        model.Pci[index] = index < size ? data[index] : 0U;
    }
    Asteria::Platform::Device::VirtioPciTransport transport{model, model, model};
    const Asteria::Platform::Device::VirtioPciBar bars[6U]{
        {.Base = model.BarBase, .Length = 4096U},
    };
    const auto result = transport.Discover(Asteria::Kernel::Device::PciFunctionAddress{0U, 0U, 1U, 0U}, bars);
    // Discovery can neither change status nor publish queues/DMA/notifications.
    if (model.Status != 0U || model.Notifications != 0U || model.Requests != 0U || model.Violations != 0U ||
        transport.IsReady())
    {
        std::abort();
    }
    if (result == Asteria::Platform::Device::VirtioTransportError::None)
    {
        // A successful walk must leave a usable common window in BAR authority.
        if (transport.Reset(2U) != Asteria::Platform::Device::VirtioTransportError::None)
        {
            // Arbitrary common offsets can name non-device locations in this
            // fixture, but MMIO must still remain inside its BAR aperture.
            if (model.Violations == 0U)
            {
                std::abort();
            }
        }
    }
    return 0;
}
