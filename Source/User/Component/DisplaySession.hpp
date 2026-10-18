// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_COMPONENT_DISPLAY_SESSION_HPP_INCLUDED
#define ASTERIA_USER_COMPONENT_DISPLAY_SESSION_HPP_INCLUDED

#include "Abi/ComponentBootstrap.hpp"
#include "User/Graphics/Composition/PlaneStorage.hpp"
#include "User/Session/SessionFaceFrame.hpp"
#include "User/Identity/SeatFrame.hpp"

namespace Asteria::User::Component
{
/// One face per broker incarnation; the console endpoint opens it, only Manage changes trusted
/// visibility. Keep this owner in static storage. All calls are serialized by the drain.
class DisplaySession final
{
public:
    void Configure(System::CapabilityHandle domain, System::CapabilityHandle space, System::CapabilityHandle waitSet,
                   Foundation::UInt64 address, Graphics::PixelFormat format, DisplayInfo& info,
                   Graphics::Composition::PlaneStorage& planes, Graphics::Composition::DamageList& damage) noexcept;
    [[nodiscard]] System::CapabilityHandle Face() const noexcept
    {
        return m_face;
    }
    [[nodiscard]] System::CapabilityHandle Control() const noexcept
    {
        return m_control;
    }
    [[nodiscard]] System::CapabilityHandle TrustedFace() const noexcept
    {
        return m_trustedFace;
    }
    [[nodiscard]] bool Retrying() const noexcept
    {
        return Pending() || m_trustedOpenReply.Selector != 0U || m_trustedTransaction != 0U ||
               (m_credit.Selector != 0U && (m_count != 0U || m_pointerStale));
    }
    [[nodiscard]] bool Pending() const noexcept
    {
        return m_present.Selector != 0U;
    }
    [[nodiscard]] bool Dispatch(const System::SystemCallClient& client, const Session::SessionFaceMessage& message,
                                Foundation::UInt64 rights, Foundation::Size route) noexcept;
    [[nodiscard]] Foundation::UInt64 DroppedSnapshots() const noexcept
    {
        return m_dropped;
    }
    [[nodiscard]] bool Failed() const noexcept
    {
        return m_failed;
    }
    void Pointer(const Foundation::UInt64* words, Graphics::Point position) noexcept;
    void SendCredit(const System::SystemCallClient& client) noexcept;
    void Presented(const System::SystemCallClient& client) noexcept;
    [[nodiscard]] bool DispatchTrusted(const System::SystemCallClient&, const Identity::SeatMessage&,
                                       Foundation::Size) noexcept;
    [[nodiscard]] bool Trusted() const noexcept
    {
        return m_planes->Trusted().Visible();
    }

private:
    [[nodiscard]] bool Open(const System::SystemCallClient& client) noexcept;
    [[nodiscard]] bool Create(const System::SystemCallClient& client,
                              const Session::SessionFaceMessage& message) noexcept;
    [[nodiscard]] bool Add(const System::SystemCallClient& client, const Session::SessionFaceMessage& message,
                           Foundation::Size first) noexcept;
    [[nodiscard]] bool Release(const System::SystemCallClient& client) noexcept;
    [[nodiscard]] bool Reply(const System::SystemCallClient& client, System::CapabilityHandle reply,
                             Foundation::UInt64 transaction, Foundation::UInt64 kind, bool opened = false) noexcept;
    void CloseCredit(const System::SystemCallClient& client) noexcept;
    System::CapabilityHandle m_domain{};
    System::CapabilityHandle m_space{};
    System::CapabilityHandle m_waitSet{};
    System::CapabilityHandle m_face{};
    System::CapabilityHandle m_control{};
    System::CapabilityHandle m_memory{};
    Identity::ScreenEnrollment m_enrollment{};
    System::CapabilityHandle m_trustedFace{};
    System::CapabilityHandle m_trustedFeedback{};
    System::CapabilityHandle m_trustedMemory{};
    System::CapabilityHandle m_trustedFrame{};
    System::CapabilityHandle m_trustedOpenReply{};
    Foundation::UInt64 m_trustedOpenTransaction{0U};
    Foundation::UInt64 m_trustedSerial{0U};
    Foundation::UInt64 m_trustedTransaction{0U};
    bool m_trustedPresented{false};
    System::CapabilityHandle m_frames[16U]{};
    Foundation::UInt64 m_pages[16U]{};
    Foundation::UInt64 m_offsets[16U]{};
    Foundation::Size m_frameCount{0U};
    Foundation::Size m_mapped{0U};
    Foundation::UInt64 m_address{0U};
    DisplayInfo* m_info{nullptr};
    Graphics::Composition::PlaneStorage* m_planes{nullptr};
    Graphics::Composition::DamageList* m_damage{nullptr};
    Graphics::PixelSurface<const Foundation::UInt32> m_buffer{};
    Graphics::Rect m_bounds{};
    System::CapabilityHandle m_present{};
    System::CapabilityHandle m_credit{};
    Foundation::UInt64 m_presentTransaction{0U};
    Foundation::UInt64 m_creditTransaction{0U};
    Foundation::UInt64 m_serial{0U};
    static constexpr Foundation::Size SnapshotCapacity{64U};
    bool m_motion[SnapshotCapacity]{};
    Foundation::UInt64 m_snapshots[SnapshotCapacity][4U]{};
    Foundation::Size m_first{0U};
    Foundation::Size m_count{0U};
    Foundation::Size m_creditMaximum{0U};
    Foundation::Size m_frozenCount{0U};
    Foundation::Size m_creditBytes{0U};
    // Exhaustion makes the ordered route stale, never the physical cursor.
    // A frozen credit prefix still settles unchanged.
    bool m_routing{false};
    bool m_failed{false};
    bool m_pointerStale{false};
    bool m_staleCredit{false};
    Foundation::UInt64 m_dropped{0U};
    Foundation::UInt64 m_presentKind{0U};
    alignas(8) Foundation::UInt8 m_creditFrame[EchoMaximumFrameBytes]{};
    Foundation::UInt64 m_words[32U]{};
    alignas(8) Foundation::UInt8 m_frame[EchoMaximumFrameBytes]{};
};
} // Namespace Asteria::User::Component
#endif
