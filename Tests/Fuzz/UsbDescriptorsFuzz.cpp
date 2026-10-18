// SPDX-License-Identifier: Apache-2.0
#include "User/Input/UsbDescriptors.hpp"
#include "User/Input/UsbConfiguration.hpp"

#include <cstring>

#include "User/Input/UsbCompositeFixture.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace
{
namespace In = Asteria::User::Input;
void Require(const bool good) noexcept
{
    if (!good)
    {
        std::abort();
    }
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, const std::size_t size) noexcept
{
    In::BootInterface output{};
    output.Configuration = 0xA5U;
    output.Endpoint.Address = 0xA5U;
    const auto before = output;
    if (In::UsbDescriptors::FindBoot(data, size, output))
    {
        Require((output.Endpoint.Address & 0x80U) != 0U);
        Require((output.Endpoint.Attributes & 3U) == 3U);
        Require((output.Endpoint.MaxPacketSize & 0x7FFU) <= In::MaximumInterruptBytes);
        In::ConfigurationHeader header{};
        Require(In::UsbDescriptors::Configuration(data, size, header));
        Require((header.TotalLength <= size) && (header.TotalLength <= In::MaximumConfigurationBytes));
        In::BootInterface again{};
        Require(In::UsbDescriptors::FindBoot(data, header.TotalLength, again));
        Require((output.Number == again.Number) && (output.Endpoint.Address == again.Endpoint.Address));
    }
    else
    {
        Require(std::memcmp(&before, &output, sizeof(output)) == 0);
    }
    In::UsbConfiguration configuration{};
    const auto original = configuration;
    if (In::UsbConfiguration::Parse(data, size, configuration) == In::DescriptorError::None)
    {
        In::UsbConfiguration again{};
        Require(In::UsbConfiguration::Parse(data, configuration.Header().TotalLength, again) ==
                In::DescriptorError::None);
        Require(configuration.InterfaceCount() == again.InterfaceCount());
        for (In::Size i{0U}; i < configuration.InterfaceCount(); ++i)
        {
            In::InterfaceDescriptor interface{};
            Require(configuration.Interface(i, interface));
            for (In::Size j{0U}; j < interface.Hid.DescriptorCount; ++j)
            {
                In::HidClassEntry entry{};
                Require(configuration.HidEntry(interface, j, entry));
                Require(entry.Length != 0U);
            }
            for (In::Size j{0U}; j < interface.EndpointCount; ++j)
            {
                In::EndpointDescriptor endpoint{};
                Require(configuration.Endpoint(interface, j, endpoint));
                Require((endpoint.Address & 15U) != 0U);
            }
            In::InterfaceBinding binding{};
            const auto saved = binding;
            if (configuration.Bind(i, binding) == In::BindingError::None)
            {
                Require(interface.Alternate == 0U);
                Require((binding.In.Address & 0x80U) != 0U);
                if (binding.Class == In::InterfaceClass::MassStorage)
                {
                    Require((binding.Out.Address & 0x80U) == 0U);
                    Require((binding.Out.Attributes & 3U) == 2U);
                }
            }
            else
            {
                Require(std::memcmp(&saved, &binding, sizeof(saved)) == 0);
            }
        }
    }
    else
    {
        Require(std::memcmp(&original, &configuration, sizeof(original)) == 0);
    }
    In::DeviceDescriptor device{};
    static_cast<void>(In::UsbDescriptors::Device(data, size, device));
    return 0;
}

extern "C" void AsteriaFuzzSeeds(void (*const emit)(const std::uint8_t*, std::size_t)) noexcept
{
    constexpr std::uint8_t bytes[]{9U, 2U, 34U,   0U,    1U, 1U, 8U, 0xA0U, 50U,   9U, 4U, 0U, 0U,    1U, 3U, 1U, 1U,
                                   0U, 9U, 0x21U, 0x11U, 1U, 0U, 1U, 0x22U, 0x3FU, 0U, 7U, 5U, 0x81U, 3U, 8U, 0U, 10U};
    emit(bytes, sizeof(bytes));
    emit(bytes, 0U);
    emit(Asteria::Test::Input::CompositeConfiguration, sizeof(Asteria::Test::Input::CompositeConfiguration));
}
