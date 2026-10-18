// SPDX-License-Identifier: Apache-2.0
#include "User/Input/UsbConfiguration.hpp"

#include <cstring>

#include "Support/TestContext.hpp"
#include "User/Input/UsbCompositeFixture.hpp"

namespace
{
namespace In = Asteria::User::Input;
using Asteria::Test::Input::CompositeConfiguration;
} // namespace

int main()
{
    Asteria::TestSupport::TestContext context{"USB configuration model"};
    In::UInt8 bytes[sizeof(CompositeConfiguration)]{};
    std::memcpy(bytes, CompositeConfiguration, sizeof(bytes));
    bytes[2U] = sizeof(bytes);
    In::UsbConfiguration configuration{};
    ASTERIA_EXPECT(context, "complete immutable view",
                   In::UsbConfiguration::Parse(bytes, sizeof(bytes), configuration) == In::DescriptorError::None,
                   configuration.InterfaceCount() == 4U, configuration.AssociationCount() == 1U);
    In::InterfaceDescriptor interface{};
    In::EndpointDescriptor endpoint{};
    In::HidClassEntry hid{};
    In::InterfaceAssociation association{};
    In::InterfaceBinding binding{};
    ASTERIA_EXPECT(context, "alternate and HID metadata retained", configuration.Interface(1U, interface),
                   interface.Number == 0U, interface.Alternate == 1U, interface.HasHid, interface.Hid.Version == 0x111U,
                   configuration.Endpoint(interface, 0U, endpoint), endpoint.Address == 0x81U,
                   configuration.HidEntry(interface, 0U, hid), hid.Type == 0x22U, hid.Length == 63U,
                   configuration.Bind(1U, binding) == In::BindingError::Alternate);
    ASTERIA_EXPECT(context, "association fields", configuration.Association(0U, association), association.First == 0U,
                   association.Count == 2U, association.Class == 3U);
    ASTERIA_EXPECT(context, "keyboard binding", configuration.Bind(0U, binding) == In::BindingError::None,
                   binding.Class == In::InterfaceClass::BootKeyboard, binding.In.Address == 0x81U,
                   binding.ReportLength == 63U);
    ASTERIA_EXPECT(context, "mouse binding", configuration.Bind(2U, binding) == In::BindingError::None,
                   binding.Class == In::InterfaceClass::BootMouse, binding.In.Address == 0x82U);
    ASTERIA_EXPECT(context, "BOT pair binding only", configuration.Bind(3U, binding) == In::BindingError::None,
                   binding.Class == In::InterfaceClass::MassStorage, binding.In.Address == 0x83U,
                   binding.Out.Address == 3U, binding.In.MaxPacketSize == 512U);
    interface.Offset = ~In::Size{0U};
    ASTERIA_EXPECT(context, "forged interface view refused", !configuration.Endpoint(interface, 0U, endpoint));
    const In::UsbConfiguration before = configuration;
    for (In::Size length{0U}; length < sizeof(bytes); ++length)
    {
        ASTERIA_EXPECT(context, "all truncations failure atomic",
                       In::UsbConfiguration::Parse(bytes, length, configuration) != In::DescriptorError::None,
                       std::memcmp(&before, &configuration, sizeof(before)) == 0);
    }
    struct Mutation
    {
        In::Size Offset;
        In::UInt8 Value;
        In::DescriptorError Reason;
    };
    constexpr Mutation mutations[]{
        {4U, 4U, In::DescriptorError::InterfaceCount}, {45U, 0U, In::DescriptorError::DuplicateInterface},
        {12U, 4U, In::DescriptorError::Association},   {87U, 0x81U, In::DescriptorError::DuplicateEndpoint},
        {31U, 0U, In::DescriptorError::Hid},           {103U, 0x80U, In::DescriptorError::Endpoint},
        {96U, 3U, In::DescriptorError::EndpointCount},
    };
    for (const Mutation mutation : mutations)
    {
        std::memcpy(bytes, CompositeConfiguration, sizeof(bytes));
        bytes[2U] = sizeof(bytes);
        bytes[mutation.Offset] = mutation.Value;
        ASTERIA_EXPECT(context, "specific refusal reason",
                       In::UsbConfiguration::Parse(bytes, sizeof(bytes), configuration) == mutation.Reason,
                       std::memcmp(&before, &configuration, sizeof(before)) == 0);
    }
    return context.Finish();
}
