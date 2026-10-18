// SPDX-License-Identifier: Apache-2.0

#include "User/Pager/PowerOff.hpp"
#include "User/Component/FsFrame.hpp"
#include "User/Component/BlockFrame.hpp"
namespace Asteria::User::Pager
{
namespace
{
[[nodiscard]] bool ProbeFailure(const ScenarioContext& context, const char* phase,
                                const Foundation::UInt64 detail) noexcept
{
    char line[96U]{};
    Foundation::Size length{0U};
    constexpr char prefix[]{"# power: FAT probe failed: "};
    for (Foundation::Size i{0U}; i < sizeof(prefix) - 1U; ++i)
    {
        line[length++] = prefix[i];
    }
    for (Foundation::Size i{0U}; i < 40U && phase[i] != '\0'; ++i)
    {
        line[length++] = phase[i];
    }
    line[length++] = ' ';
    line[length++] = '0';
    line[length++] = 'x';
    constexpr char digits[]{"0123456789ABCDEF"};
    for (Foundation::Size shift{64U}; shift != 0U; shift -= 4U)
    {
        line[length++] = digits[(detail >> (shift - 4U)) & 15U];
    }
    line[length++] = '\n';
    WriteBetweenLines(context, line, length);
    return false;
}
} // namespace
bool PreparePowerTestVolume(ScenarioContext& context, Abi::SystemCallError& error) noexcept
{
    // PowerOff.md: emulator disks alone, under the existing eligibility gate.
    if ((context.Bootstrap->ManagementFeatures & Abi::PagerManagementDiskWritable) == 0U ||
        !context.Devices->IsPresent(Abi::PagerDeviceAhci))
    {
        return false;
    }
    auto* const driver =
        StartComponent(context, context.Component, context.NextTag++, Abi::ComponentBehaviorBlockDriver, error);
    if (driver == nullptr)
    {
        return ProbeFailure(context, "block startup", static_cast<Foundation::UInt64>(error));
    }
    context.DriverService = driver->Service;
    context.DriverGeneration = driver->Tag;
    auto* const filesystem =
        StartComponent(context, context.Component, context.NextTag++, Abi::ComponentBehaviorFilesystem, error);
    context.DriverService = {};
    context.DriverGeneration = 0U;
    if (filesystem == nullptr)
    {
        return ProbeFailure(context, "filesystem startup", static_cast<Foundation::UInt64>(error));
    }
    System::CapabilityHandle reply{};
    if (!context.Calls->CreateEndpoint(context.Management, reply, error))
    {
        return ProbeFailure(context, "reply endpoint", static_cast<Foundation::UInt64>(error));
    }
    static Component::FsMessage message{};
    // Filesystem v2: root installs its own writable view through the retained
    // manager face; ordinary requests name that view and its slot generation.
    const Foundation::UInt64 viewWords[Component::FsPayloadWords]{
        filesystem->Tag, 0U, 0U, 0U, 0U, Component::FsReadRight | Component::FsWriteRight, 0U, 0U};
    const auto viewBytes = Component::WriteFsFrame(context.Frame, Component::FsInstallViewMethod, filesystem->Tag,
                                                   viewWords, nullptr, 0U, &reply, nullptr);
    const bool viewSent =
        Check(context.Client->Send(filesystem->FsManager, System::AddressOf(context.Frame), viewBytes), error);
    const auto viewReceived = viewSent ? context.Supervisor->AwaitMessageFor(*filesystem, reply, context.Delivery,
                                                                             sizeof(context.Delivery), 500U, error)
                                       : 0U;
    message = {};
    if (viewReceived == 0U ||
        !Component::ReadFsFrame(context.Delivery, static_cast<Foundation::Size>(viewReceived), message) ||
        message.Method != Component::FsReplyMethod || message.Words[1U] != Component::FsDone)
    {
        const bool closed = context.Calls->Close(reply, error);
        static_cast<void>(closed);
        return ProbeFailure(context, "view installation", message.Words[1U]);
    }
    const auto view = message.Words[2U];
    const auto rootGeneration = message.Words[3U];
    constexpr Foundation::UInt8 name[]{'p', 'o', 'w', 'e', 'r', '.', 't', 'x', 't'};
    const Foundation::UInt64 words[Component::FsPayloadWords]{
        filesystem->Tag, sizeof(name), 0U, 0U, view, 0U, rootGeneration, 0U};
    const auto size = Component::WriteFsFrame(context.Frame, Component::FsCreateMethod, filesystem->Tag, words, name,
                                              sizeof(name), &reply, nullptr);
    bool staged = Check(context.Client->Send(filesystem->Service, System::AddressOf(context.Frame), size), error);
    const auto received = staged ? context.Supervisor->AwaitMessageFor(*filesystem, reply, context.Delivery,
                                                                       sizeof(context.Delivery), 500U, error)
                                 : 0U;
    message = {};
    const bool decoded =
        received != 0U && Component::ReadFsFrame(context.Delivery, static_cast<Foundation::Size>(received), message);
    staged = decoded && message.Method == Component::FsReplyMethod && message.Words[1U] == Component::FsDone;
    if (staged)
    {
        System::CapabilityHandle data{};
        const Foundation::UInt64 write[Component::FsPayloadWords]{
            filesystem->Tag, message.Words[2U], message.Words[3U], 0U, view, 0U, 0U, 0U};
        staged = context.Calls->CreateMemory(context.Management, Abi::ObjectTypeFrame, 1U, 0U, data, error);
        if (staged)
        {
            const auto bytes = Component::WriteFsFrame(context.Frame, Component::FsWriteMethod, filesystem->Tag + 1U,
                                                       write, nullptr, 0U, &reply, &data);
            staged = Check(context.Client->Send(filesystem->Service, System::AddressOf(context.Frame), bytes), error);
            const auto written = staged ? context.Supervisor->AwaitMessageFor(*filesystem, reply, context.Delivery,
                                                                              sizeof(context.Delivery), 500U, error)
                                        : 0U;
            staged = written != 0U &&
                     Component::ReadFsFrame(context.Delivery, static_cast<Foundation::Size>(written), message) &&
                     message.Method == Component::FsReplyMethod && message.Words[1U] == Component::FsDone;
            const bool dataClosed = context.Calls->Close(data, error);
            staged = staged && dataClosed;
        }
    }
    const bool closed = context.Calls->Close(reply, error);
    if (staged && closed)
    {
        constexpr char text[]{"# power: test FAT session staged (uncommitted)\n"};
        WriteBetweenLines(context, text, sizeof(text) - 1U);
    }
    return (staged && closed) || ProbeFailure(context, decoded ? "create reply" : "create delivery",
                                              decoded ? (message.Words[1U] | (message.Words[2U] << 32U))
                                                      : static_cast<Foundation::UInt64>(error));
}

bool RequestPower(ScenarioContext& context, const bool reboot, Abi::SystemCallError& error) noexcept
{
    const auto available = context.Client->ControlDevice(context.Management, Abi::DevicePowerQuery, {});
    if (available.Error != Abi::SystemCallError::None ||
        (available.Value0 & (reboot ? Abi::DeviceRebootAvailable : Abi::DevicePowerOffAvailable)) == 0U)
    {
        constexpr char text[]{"# power: unsupported firmware transition; EFI runtime services unmapped\n"};
        WriteBetweenLines(context, text, sizeof(text) - 1U);
        return true;
    }
    const char* const preparing = reboot ? "# power: Restarting\n" : "# power: Shutting down\n";
    WriteBetweenLines(context, preparing, reboot ? 20U : 23U);
    if (!context.Supervisor->PreparePower(error))
    {
        constexpr char text[]{"# power: flush/quiesce failed; transition refused\n"};
        WriteBetweenLines(context, text, sizeof(text) - 1U);
        return false;
    }
    constexpr char clean[]{"# power: filesystems clean; parking devices and CPUs\n"};
    WriteLine(context, clean, sizeof(clean) - 1U);
    Foundation::UInt64 epoch{0U};
    Foundation::UInt64 sequence{0U};
    if (!FencePointer(context, epoch, sequence, error))
    {
        return false;
    }
    if (context.Display != nullptr)
    {
        context.Display->Console = {};
        context.Display->Broker = nullptr;
    }
    if (!context.Supervisor->Abandon(error))
    {
        return false;
    }
    constexpr char ready[]{"# power: devices quiet; transition\n"};
    WriteLine(context, ready, sizeof(ready) - 1U);
    const auto result =
        context.Client->ControlDevice(context.Management, reboot ? Abi::DeviceReboot : Abi::DevicePowerOff, {});
    error = result.Error;
    return false;
}
} // namespace Asteria::User::Pager
