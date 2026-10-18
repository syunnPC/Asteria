// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_COMPONENT_SERVICE_SCRATCH_HPP_INCLUDED
#define ASTERIA_USER_COMPONENT_SERVICE_SCRATCH_HPP_INCLUDED

#include "User/Component/InputBrokerLogic.hpp"
#include "User/Component/PointerRoute.hpp"
#include "User/Filesystem/FsVolume.hpp"
#include "User/Filesystem/Partitions.hpp"
#include "User/Filesystem/Fat/FatVolume.hpp"
#include "User/Filesystem/Exfat/ExfatVolume.hpp"
#include "User/Component/FsViews.hpp"
#include "User/Component/XhciStorage.hpp"

namespace Asteria::User::Component
{
struct InputCore
{
    InputBrokerLogic Logic;
    PointerRoute Route;
};

// fixed view tables and bounded COW chains share the image's
// behaviour-exclusive storage; grants remain the existing ABI budget.
struct FilesystemCore
{
    union Backend
    {
        Filesystem::FsVolume Native;
        Filesystem::FatVolume Fat;
        Filesystem::ExfatVolume Exfat;
        Filesystem::Partitions Partitions;
        constexpr Backend() noexcept : Native{}
        {
        }
    } Volume;
    FsViews Views;
};
struct XhciCore
{
    XhciStorage Storage;
    Usb::PortMemory Ports[Usb::MaximumDevices];
    Usb::EndpointMemory Pipes[Usb::MaximumDevices][Usb::MaximumEndpointPipes];
    Usb::DmaMemory Scratchpads[Abi::XhciScratchpadBudget];
    Usb::ControllerMemory Memory;
};
/// A component incarnation runs exactly one immutable behaviour. Overlay its input, filesystem and
/// xHCI working sets. The display tile has its own module. Explicit lifetime
/// construction, no allocation; caller initializes once on entry to its service, then borrows only
/// that behaviour's member. Not shared between threads or behaviours. Reinitialization ends
/// resource-free lifetimes.
class ServiceScratch final
{
public:
    constexpr ServiceScratch() noexcept = default;
    ServiceScratch(const ServiceScratch&) = delete;
    ServiceScratch& operator=(const ServiceScratch&) = delete;
    ServiceScratch(ServiceScratch&&) = delete;
    ServiceScratch& operator=(ServiceScratch&&) = delete;
    // Test seat runs Input OR PS/2, never both in one incarnation. Borrow raw storage before either
    // lifetime starts.
    [[nodiscard]] void* UninitializedStorage() noexcept
    {
        return m_bytes;
    }
    [[nodiscard]] static constexpr Foundation::Size StorageBytes() noexcept
    {
        return Bytes;
    }
    void InitializeInput() noexcept;
    void InitializeFilesystem() noexcept;
    void InitializeNative() noexcept;
    void InitializePartitions() noexcept;
    void InitializeFat() noexcept;
    void InitializeExfat() noexcept;
    void InitializeXhci() noexcept;
    [[nodiscard]] FilesystemCore& FilesystemState() const noexcept;
    [[nodiscard]] XhciCore& Xhci() const noexcept;
    [[nodiscard]] InputCore& Input() const noexcept;
    [[nodiscard]] Filesystem::FsVolume& Filesystem() const noexcept;

private:
    static constexpr Foundation::Size OtherBytes{sizeof(InputCore) > sizeof(XhciCore) ? sizeof(InputCore)
                                                                                      : sizeof(XhciCore)};
    static constexpr Foundation::Size Bytes{OtherBytes > sizeof(FilesystemCore) ? OtherBytes : sizeof(FilesystemCore)};
    alignas(InputCore) alignas(FilesystemCore) alignas(XhciCore) Foundation::UInt8 m_bytes[Bytes]{};
    InputCore* m_input{nullptr};
    FilesystemCore* m_filesystem{nullptr};
    XhciCore* m_xhci{nullptr};
};
/// The sole zero-initialized scratch owner in each component address space.
[[nodiscard]] ServiceScratch& Scratch() noexcept;
} // Namespace Asteria::User::Component
#endif // ASTERIA_USER_COMPONENT_SERVICE_SCRATCH_HPP_INCLUDED
