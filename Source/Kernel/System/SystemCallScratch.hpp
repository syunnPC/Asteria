// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_SYSTEM_SYSTEM_CALL_SCRATCH_HPP_INCLUDED
#define ASTERIA_KERNEL_SYSTEM_SYSTEM_CALL_SCRATCH_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Kernel/Ipc/IpcDeliveryCodec.hpp"

namespace Asteria::Kernel::System
{
/// Processor-local persistent storage for one non-reentrant syscall dispatch.
/// Keeping the maximum IPC frame here avoids placing it on a target stack.
class SystemCallScratch final
{
public:
    constexpr SystemCallScratch() noexcept = default;

    SystemCallScratch(const SystemCallScratch&) = delete;
    SystemCallScratch& operator=(const SystemCallScratch&) = delete;
    SystemCallScratch(SystemCallScratch&&) = delete;
    SystemCallScratch& operator=(SystemCallScratch&&) = delete;

    void Clear() noexcept
    {
        for (Foundation::UInt8& value : m_wireBytes)
        {
            value = 0U;
        }
        m_message = {};
    }

    [[nodiscard]] Ipc::WritableByteView GetWritableWireBuffer() noexcept
    {
        return Ipc::WritableByteView{
            .Data = m_wireBytes,
            .SizeBytes = sizeof(m_wireBytes),
        };
    }

    [[nodiscard]] Ipc::ReadOnlyByteView GetWireBuffer(const Foundation::Size sizeBytes) const noexcept
    {
        return Ipc::ReadOnlyByteView{
            .Data = m_wireBytes,
            .SizeBytes = sizeBytes <= sizeof(m_wireBytes) ? sizeBytes : 0U,
        };
    }

    [[nodiscard]] Ipc::MessageEnvelope& GetMessage() noexcept
    {
        return m_message;
    }

private:
    Foundation::UInt8 m_wireBytes[Ipc::IpcDeliveryCodec::MaximumWireSizeBytes]{};
    Ipc::MessageEnvelope m_message{};
};

static_assert(sizeof(SystemCallScratch) == 1'048U);
static_assert(alignof(SystemCallScratch) == 8U);
} // namespace Asteria::Kernel::System

#endif // ASTERIA_KERNEL_SYSTEM_SYSTEM_CALL_SCRATCH_HPP_INCLUDED
