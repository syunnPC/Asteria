// SPDX-License-Identifier: Apache-2.0

#include "User/Component/XhciStorage.hpp"

namespace Asteria::User::Component
{
namespace
{
struct XhciPlacement
{
    void* Address;
};
}
}

[[nodiscard]] inline void* operator new(Asteria::Foundation::Size,
                                        Asteria::User::Component::XhciPlacement place) noexcept
{
    return place.Address;
}

namespace Asteria::User::Component
{
void XhciStorage::Initialize(Foundation::UInt8* const controller, Foundation::UInt8* const firstDevices,
                             Foundation::UInt8* const lastDevices) noexcept
{
    m_registerOwner = new (XhciPlacement{controller + sizeof(Usb::XhciController)}) System::XhciRegisters{};
    m_controllerOwner = new (XhciPlacement{controller}) Usb::XhciController{Registers()};
    m_progressOwner = new (XhciPlacement{m_progress}) XhciProgress{};
    for (Foundation::Size index{0U}; index < Usb::MaximumDevices; ++index)
    {
        m_deviceOwners[index] = new (XhciPlacement{
            (index < Abi::XhciFirstDevices ? firstDevices + index * sizeof(Input::UsbDevice)
                                           : lastDevices + (index - Abi::XhciFirstDevices) * sizeof(Input::UsbDevice))})
            Input::UsbDevice{};
    }
}
System::XhciRegisters& XhciStorage::Registers() noexcept
{
    return *m_registerOwner;
}
Usb::XhciController& XhciStorage::Controller() noexcept
{
    return *m_controllerOwner;
}
Input::UsbDevice& XhciStorage::Device(const Foundation::Size index) noexcept
{
    return *m_deviceOwners[index];
}
XhciProgress& XhciStorage::Progress() noexcept
{
    return *m_progressOwner;
}
} // namespace Asteria::User::Component
