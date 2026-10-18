// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_COMPONENT_XHCI_STORAGE_HPP_INCLUDED
#define ASTERIA_USER_COMPONENT_XHCI_STORAGE_HPP_INCLUDED

#include "User/Component/XhciProgress.hpp"
#include "User/System/XhciRegisters.hpp"

namespace Asteria::User::Component
{
/// Explicit image-lifetime storage: the component ABI permits only zero-filled
/// writable segments. Vtables, references and USB initial state are constructed
/// here after entry, with no allocator, guard or shutdown destructor. One owner
/// per component address space, serialized in its single thread. Initialize is
/// called once; device DMA is recovered by Device Manager after that thread ends.
class XhciStorage final
{
public:
    constexpr XhciStorage() noexcept = default;
    XhciStorage(const XhciStorage&) = delete;
    XhciStorage& operator=(const XhciStorage&) = delete;
    XhciStorage(XhciStorage&&) = delete;
    XhciStorage& operator=(XhciStorage&&) = delete;
    /// Registers/controller fit a 17-page arena; four devices fit the remaining 15 pages of the first Frame;
    /// twelve devices fit the second 38-page Frame. Borrowed arenas are aligned, mapped and remain alive until drain.
    void Initialize(Foundation::UInt8* controller, Foundation::UInt8* firstDevices,
                    Foundation::UInt8* lastDevices) noexcept;
    [[nodiscard]] System::XhciRegisters& Registers() noexcept;
    [[nodiscard]] Usb::XhciController& Controller() noexcept;
    [[nodiscard]] Input::UsbDevice& Device(Foundation::Size index) noexcept;
    /// Constructed by Initialize in this object's own zero-filled bytes.
    [[nodiscard]] XhciProgress& Progress() noexcept;

private:
    System::XhciRegisters* m_registerOwner{nullptr};
    Usb::XhciController* m_controllerOwner{nullptr};
    Input::UsbDevice* m_deviceOwners[Usb::MaximumDevices]{};
    XhciProgress* m_progressOwner{nullptr};
    alignas(XhciProgress) Foundation::UInt8 m_progress[sizeof(XhciProgress)]{};
    static_assert(sizeof(Usb::XhciController) + sizeof(System::XhciRegisters) <= Abi::XhciControllerStatePages * 4096U);
    static_assert(Abi::XhciFirstDevices * sizeof(Input::UsbDevice) <=
                  (Abi::XhciStateFirstPages - Abi::XhciControllerStatePages) * 4096U);
    static_assert((Usb::MaximumDevices - Abi::XhciFirstDevices) * sizeof(Input::UsbDevice) <=
                  Abi::XhciStateLastPages * 4096U);
};
} // namespace Asteria::User::Component
#endif
