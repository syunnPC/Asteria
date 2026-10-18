// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_USB_XHCIMEMORY_HPP_INCLUDED
#define ASTERIA_USER_USB_XHCIMEMORY_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::User::Usb
{
using Foundation::Size;
using Foundation::UInt8;
using Foundation::UInt16;
using Foundation::UInt32;
using Foundation::UInt64;

/// Non-owning, contiguous, little-endian DMA mapping. The owner pins all mappings
/// until Halt succeeds. CPU/device ranges must be disjoint and exclusive to this
/// controller; coherent memory or the accessor's cache maintenance is required.
struct DmaMemory
{
    UInt64 Address;
    UInt8* Cpu;
    Size Bytes;
};
[[nodiscard]] inline bool ValidMemory(const DmaMemory memory, const Size bytes, const UInt64 alignment,
                                      const bool address64) noexcept
{
    return (alignment != 0U) && ((alignment & (alignment - 1U)) == 0U) && (memory.Cpu != nullptr) &&
           (memory.Bytes != 0U) && (memory.Bytes >= bytes) && (memory.Bytes <= 65536U) && (memory.Address != 0U) &&
           ((memory.Address & (alignment - 1U)) == 0U) && (memory.Address <= (~UInt64{0U} - memory.Bytes)) &&
           (address64 || ((memory.Address + memory.Bytes - 1U) <= 0xFFFFFFFFU));
}
// Byte codecs avoid native-layout, alignment and aliasing assumptions in DMA.
[[nodiscard]] inline UInt32 Load32(const UInt8* const bytes) noexcept
{
    UInt32 value{0U};
    for (Size i{0U}; i < 4U; ++i)
    {
        value |= UInt32{bytes[i]} << (i * 8U);
    }
    return value;
}
[[nodiscard]] inline UInt64 Load64(const UInt8* const bytes) noexcept
{
    return UInt64{Load32(bytes)} | (UInt64{Load32(bytes + 4U)} << 32U);
}
inline void Store32(UInt8* const bytes, const UInt32 value) noexcept
{
    for (Size i{0U}; i < 4U; ++i)
    {
        bytes[i] = static_cast<UInt8>((value >> (i * 8U)) & 0xFFU);
    }
}
inline void Store64(UInt8* const bytes, const UInt64 value) noexcept
{
    Store32(bytes, static_cast<UInt32>(value & 0xFFFFFFFFU));
    Store32(bytes + 4U, static_cast<UInt32>(value >> 32U));
}
inline void Clear(const DmaMemory memory) noexcept
{
    for (Size i{0U}; i < memory.Bytes; ++i)
    {
        memory.Cpu[i] = 0U;
    }
}
struct Trb
{
    UInt64 Parameter;
    UInt32 Status;
    UInt32 Control;
};
class XhciRing final
{
public:
    /// Requires an exclusive validated 16..4096-TRB mapping in one 64KiB window.
    void Initialize(DmaMemory memory) noexcept;
    /// One producer, at most one outstanding TD; caller does not reuse before
    /// its terminal event (or quiescence). Reserved Link TRB toggles the cycle.
    void Begin() noexcept;
    [[nodiscard]] UInt64 Push(Trb trb, bool chain = false) noexcept;
    /// After PublishDma, make the first TRB visible, then PublishDma again
    /// before ringing a doorbell. Hardware cannot consume a partial batch.
    void Commit() noexcept;
    [[nodiscard]] UInt64 Dequeue() const noexcept
    {
        return (m_memory.Address + m_index * 16U) | m_cycle;
    }

private:
    DmaMemory m_memory{};
    Size m_index{0U};
    UInt32 m_cycle{1U};
    UInt8* m_gate{nullptr};
    UInt32 m_gateControl{0U};
    bool m_batch{false};
};
} // namespace Asteria::User::Usb
#endif
