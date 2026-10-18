// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_DIAGNOSTICS_DIAGNOSTIC_CHANNEL_HPP_INCLUDED
#define ASTERIA_KERNEL_DIAGNOSTICS_DIAGNOSTIC_CHANNEL_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Kernel/Object/IKernelObject.hpp"

namespace Asteria::Kernel::Diagnostics
{
/// Receives validated lines of user diagnostic text.
/// WriteLine emits one complete line (the text plus a line terminator) in one
/// pass; implementations frame the line so that it cannot be mistaken for
/// kernel output. The text holds only printable ASCII
/// (0x20..0x7E) and may be empty. Calls are bounded and allocation-free; they
/// may spin on the device's diagnostic lock.
class IDiagnosticLineSink
{
public:
    virtual void WriteLine(const Foundation::UInt8* text, Foundation::Size lengthBytes) noexcept = 0;

protected:
    constexpr IDiagnosticLineSink() noexcept = default;
    ~IDiagnosticLineSink() noexcept = default;
};

enum class DiagnosticWriteError : Foundation::UInt8
{
    None,
    Empty,
    TooLong,
    InvalidByte,
    UnterminatedLine,
};

/// A capability-addressable output channel for user diagnostics (for example
/// the TAP report of a guest test program). Holders of a Write right emit
/// complete lines through the channel's sink; the kernel validates every byte
/// before any output, so a rejected write emits nothing.
class DiagnosticChannel final : public Object::IKernelObject
{
public:
    constexpr DiagnosticChannel(IDiagnosticLineSink& sink, const Object::ResourceDomainId chargeOwner) noexcept
        : m_sink{sink}, m_chargeOwner{chargeOwner}
    {
    }

    DiagnosticChannel(const DiagnosticChannel&) = delete;
    DiagnosticChannel& operator=(const DiagnosticChannel&) = delete;
    DiagnosticChannel(DiagnosticChannel&&) = delete;
    DiagnosticChannel& operator=(DiagnosticChannel&&) = delete;

    [[nodiscard]] Object::ObjectType GetObjectType() const noexcept override;
    [[nodiscard]] Object::ResourceDomainId GetChargeOwner() const noexcept override;

    /// Validates `text` (1..DebugWriteMaximumBytes bytes of printable ASCII
    /// and LF, ending with LF) and writes each line through the sink.
    [[nodiscard]] DiagnosticWriteError Write(const Foundation::UInt8* text, Foundation::Size sizeBytes) noexcept;

    /// The validation Write applies, without writing.
    [[nodiscard]] static DiagnosticWriteError Validate(const Foundation::UInt8* text,
                                                       Foundation::Size sizeBytes) noexcept;

private:
    IDiagnosticLineSink& m_sink;
    Object::ResourceDomainId m_chargeOwner;
};

static_assert(sizeof(DiagnosticWriteError) == 1U);
} // namespace Asteria::Kernel::Diagnostics

#endif // ASTERIA_KERNEL_DIAGNOSTICS_DIAGNOSTIC_CHANNEL_HPP_INCLUDED
