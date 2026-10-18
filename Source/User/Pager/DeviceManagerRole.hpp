// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_PAGER_DEVICE_MANAGER_ROLE_HPP_INCLUDED
#define ASTERIA_USER_PAGER_DEVICE_MANAGER_ROLE_HPP_INCLUDED

#include "Abi/ComponentBootstrap.hpp"
#include "Abi/SystemCallAbi.hpp"
#include "Foundation/Types.hpp"
#include "User/Pager/PagerProgram.hpp"
#include "User/System/AhciQuiesce.hpp"
#include "User/System/UsbHostQuiesce.hpp"
#include "User/System/XhciQuiesce.hpp"
#include "User/System/SystemCallClient.hpp"

namespace Asteria::User::Pager
{
/// How the Device Manager's last quiesce of a device ended.
enum class DeviceQuiesceOutcome : Foundation::UInt8
{
    /// None was run.
    None,
    /// Proven quiet and confirmed (DeviceQuiesced): its quarantine is lifted.
    Quiet,
    /// It did not go quiet within the polls: quarantine kept.
    TimedOut,
    /// Firmware still owns it: never reset, quarantine kept.
    FirmwareOwned,
    /// Its registers made no sense, or could not be reached: quarantine kept.
    Invalid,
};

/// What the Device Manager's last quiesce of a device found: whether it
/// still worked (a transfer, a factorial, an issued AHCI command, a running
/// schedule) when it began, how it ended, a USB host controller's kind, the
/// interrupt status it acknowledged, and its polls.
struct DeviceQuiesce
{
    bool Running;
    DeviceQuiesceOutcome Outcome;
    System::UsbHostKind Kind;
    Foundation::UInt8 Reserved;
    Foundation::UInt32 Status;
    Foundation::UInt64 Polls;
};

enum class DeviceQuiescePoll : Foundation::UInt8
{
    Running,
    Quiet,
};

/// The root service's Device Manager role, apart from its supervision of components: it holds the
/// assignable devices the platform granted (each a PCI function with Manage and the DeviceRegion of
/// its BAR, indexed by Abi::PagerDevice*) and gives each driver instance attenuated copies of its
/// device, the function assigned to the instance's ProtectionDomain. It keeps no state about the
/// components themselves.
class DeviceManagerRole final
{
public:
    constexpr DeviceManagerRole() noexcept = default;

    DeviceManagerRole(const DeviceManagerRole&) = delete;
    DeviceManagerRole& operator=(const DeviceManagerRole&) = delete;

    /// The device table of the pager's start block; zero handles for none.
    void Configure(const System::SystemCallClient& client,
                   const PagerDevice (&devices)[Abi::PagerBootstrapDevices]) noexcept;

    [[nodiscard]] bool QuiescePs2(const System::CapabilityHandle (&ports)[4U],
                                  Abi::SystemCallError& systemCall) const noexcept;

    [[nodiscard]] bool IsPresent(Foundation::Size device) const noexcept;

    /// Copies device's region (Map, Read, Write) and function (Invoke) into
    /// the instance's CapabilitySpace capabilities, writes them into its
    /// start block at regionWord and the two words after it (the region, then
    /// the function), and assigns the function to its ProtectionDomain
    /// domain, whose bus-master requests it then answers and no other's
    [[nodiscard, gnu::noinline]] bool Grant(Foundation::Size device, System::CapabilityHandle capabilities,
                                            System::CapabilityHandle domain, volatile Foundation::UInt64* block,
                                            Abi::SystemCallError& systemCall,
                                            Foundation::Size regionWord = Abi::ComponentDeviceRegionWord) noexcept;

    /// After the drain of the domain the device was assigned to, which left it quarantined:
    /// the device may still run a transfer the dead driver started, so no driver gets it
    /// again until this role confirms it quiet. BeginQuiesce maps its registers into the root
    /// service's own space at start through a device Frame and an uncached MemoryObject charged to
    /// authority; the caller polls between pauses until Quiet, never spinning, then FinishQuiesce
    /// acknowledges the interrupt status the transfer raised, confirms the device quiet for the
    /// drained assignment (DeviceQuiesced), and unmaps. A device that never goes quiet is
    /// abandoned, its quarantine kept. edu's recipe (User/System/EduQuiesce.hpp): no DMA or
    /// factorial running and no factorial interrupt still owed; then its interrupt status is
    /// acknowledged and its factorial interrupt request cleared. The AHCI controller's
    /// (User/System/AhciQuiesce.hpp, Source/User/System/AhciPort.hpp ): its interrupts masked and
    /// every implemented port stopped and reset (COMRESET, which drains an in-flight command on the
    /// emulator), until each reports a device or none and nothing issued; then the port and
    /// controller interrupt status is acknowledged. The registers of a controller's ports must fit
    /// the page it maps. The e1000e NIC's (User/System/NicQuiesce.hpp,
    /// Source/User/Network/NetworkStack.hpp ): every interrupt cause masked and the device reset,
    /// until the reset bit clears; then its interrupt cause is read, which clears it. An xHCI's
    /// (User/System/XhciQuiesce.hpp): halted, HCRST, Controller Not Ready clear, firmware's
    /// semaphore released. A USB host table function's (User/System/UsbHostQuiesce.hpp): its kind
    /// and the offset of its registers within their page read from its configuration dwords
    /// (DeviceReadConfiguration with Manage, the function released), an EHCI or OHCI mapped, a UHCI
    /// through PortIo on its region; halted, schedules off, host-controller reset, firmware's
    /// ownership released throughout. A device firmware still owns, or whose registers make no
    /// sense, is refused at once without a write.
    [[nodiscard, gnu::noinline]] bool BeginQuiesce(Foundation::Size device, System::CapabilityHandle authority,
                                                   System::CapabilityHandle space, Foundation::UInt64 start,
                                                   Abi::SystemCallError& systemCall) noexcept;
    [[nodiscard]] DeviceQuiescePoll PollQuiesce() noexcept;
    [[nodiscard, gnu::noinline]] bool FinishQuiesce(Abi::SystemCallError& systemCall) noexcept;
    void AbandonQuiesce() noexcept;
    [[nodiscard]] const DeviceQuiesce& GetLastQuiesce() const noexcept;
    /// The last quiesce of device (an Abi::PagerDevice* index).
    [[nodiscard]] const DeviceQuiesce& GetQuiesce(Foundation::Size device) const noexcept;
    /// Whether device was assigned since its last confirmation: a drain
    /// leaves it quarantined for this role to confirm quiet.
    [[nodiscard]] bool IsPending(Foundation::Size device) const noexcept;

private:
    /// A USB host table function's registers and configuration dwords, as
    /// the quiesce reaches them: an EHCI's or OHCI's mapped block, a UHCI's
    /// ports through PortIo on its region.
    class UsbHostAccess final : public System::IUsbHostQuietAccess
    {
    public:
        void Configure(const System::SystemCallClient& client, const PagerDevice& device,
                       volatile Foundation::UInt32* registers) noexcept;
        [[nodiscard]] Foundation::UInt32 Read32(Foundation::UInt32 offset) noexcept override;
        void Write32(Foundation::UInt32 offset, Foundation::UInt32 value) noexcept override;
        [[nodiscard]] Foundation::UInt16 Read16(Foundation::UInt32 offset) noexcept override;
        void Write16(Foundation::UInt32 offset, Foundation::UInt16 value) noexcept override;
        [[nodiscard]] bool ReadConfiguration(Foundation::UInt32 offset, Foundation::UInt32& value) noexcept override;

    private:
        const System::SystemCallClient* m_client{nullptr};
        PagerDevice m_device{};
        volatile Foundation::UInt32* m_registers{nullptr};
    };

    [[nodiscard]] bool BeginUsbHost(Foundation::Size device, System::CapabilityHandle authority,
                                    System::CapabilityHandle space, Foundation::UInt64 start,
                                    Abi::SystemCallError& systemCall) noexcept;

    /// The mapped registers of the quiesce under way.
    [[nodiscard]] volatile Foundation::UInt32* Registers() const noexcept;
    /// Unmaps the registers and destroys the objects that mapped them.
    [[nodiscard]] bool Unmap(Abi::SystemCallError& systemCall) noexcept;
    [[nodiscard]] static bool Check(const Abi::SystemCallResponse& response, Abi::SystemCallError& systemCall) noexcept;

    /// A granted device, its last assignment's sequence (DeviceAssign's
    /// value 0), which a confirmation after its drain names, and whether that
    /// assignment awaits its confirmation.
    struct Device
    {
        PagerDevice Handles;
        Foundation::UInt64 Sequence;
        bool Pending;
    };

    const System::SystemCallClient* m_client{nullptr};
    Device m_devices[Abi::PagerBootstrapDevices]{};
    /// The device of the quiesce under way.
    Foundation::Size m_quiescing{0U};
    System::CapabilityHandle m_space{};
    System::CapabilityHandle m_frame{};
    System::CapabilityHandle m_memory{};
    Foundation::UInt64 m_start{0U};
    Foundation::UInt64 m_pages{1U};
    System::AhciQuiesceState m_ahci{};
    System::XhciQuiesceState m_xhci{};
    System::UsbHostQuiesceState m_usbHost{};
    UsbHostAccess m_usbAccess{};
    DeviceQuiesce m_last{};
    DeviceQuiesce m_results[Abi::PagerBootstrapDevices]{};
};
} // Namespace Asteria::User::Pager

#endif // ASTERIA_USER_PAGER_DEVICE_MANAGER_ROLE_HPP_INCLUDED
