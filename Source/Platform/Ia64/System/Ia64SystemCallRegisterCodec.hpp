// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_SYSTEM_IA64_SYSTEM_CALL_REGISTER_CODEC_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_SYSTEM_IA64_SYSTEM_CALL_REGISTER_CODEC_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Kernel/System/SystemCallDispatcher.hpp"

namespace Asteria::Platform::Ia64::System
{
constexpr Foundation::UInt64 Ia64SystemCallKnownInputNatBitsMask{0x7FU};

enum class Ia64SystemCallRegisterDecodeError : Foundation::UInt8
{
    None = 0U,
    InputContainsNat = 1U,
    InvalidNatMetadata = 2U,
};

/// Logical IA-64 general-register capture consumed by the pure syscall codec.
/// InputNatBits bit 0 describes R15 and bits 1 through 6 describe R32 through
/// R37. Native entry and return code must produce and consume this image.
struct Ia64SystemCallRegisterImage
{
    Foundation::UInt64 R15;
    Foundation::UInt64 R32;
    Foundation::UInt64 R33;
    Foundation::UInt64 R34;
    Foundation::UInt64 R35;
    Foundation::UInt64 R36;
    Foundation::UInt64 R37;
    Foundation::UInt64 InputNatBits;
    Foundation::UInt64 R8;
    Foundation::UInt64 R9;
    Foundation::UInt64 R10;
};

/// Converts between an already captured IA-64 register image and the
/// architecture-neutral syscall values. These operations execute no native
/// instructions, access no global state, and allocate no memory.
class Ia64SystemCallRegisterCodec final
{
public:
    /// Decodes R15 and R32 through R37 after validating all NaT metadata.
    /// Every failure leaves request unchanged. InvalidNatMetadata is a kernel
    /// capture invariant failure and must not be converted to a user error.
    [[nodiscard]] static Ia64SystemCallRegisterDecodeError
    DecodeRequest(const Ia64SystemCallRegisterImage& image, Kernel::System::SystemCallRequest& request) noexcept;

    /// Writes only R8, R9, and R10. Every captured request field is preserved.
    static void ApplyResponse(const Kernel::System::SystemCallResponse& response,
                              Ia64SystemCallRegisterImage& image) noexcept;

private:
    Ia64SystemCallRegisterCodec() = delete;
};

static_assert(sizeof(Ia64SystemCallRegisterDecodeError) == 1U);
static_assert(Ia64SystemCallKnownInputNatBitsMask == 0x7FU);
static_assert(sizeof(Ia64SystemCallRegisterImage) == 88U);
static_assert(alignof(Ia64SystemCallRegisterImage) == 8U);
static_assert(__builtin_offsetof(Ia64SystemCallRegisterImage, R15) == 0U);
static_assert(__builtin_offsetof(Ia64SystemCallRegisterImage, R32) == 8U);
static_assert(__builtin_offsetof(Ia64SystemCallRegisterImage, R33) == 16U);
static_assert(__builtin_offsetof(Ia64SystemCallRegisterImage, R34) == 24U);
static_assert(__builtin_offsetof(Ia64SystemCallRegisterImage, R35) == 32U);
static_assert(__builtin_offsetof(Ia64SystemCallRegisterImage, R36) == 40U);
static_assert(__builtin_offsetof(Ia64SystemCallRegisterImage, R37) == 48U);
static_assert(__builtin_offsetof(Ia64SystemCallRegisterImage, InputNatBits) == 56U);
static_assert(__builtin_offsetof(Ia64SystemCallRegisterImage, R8) == 64U);
static_assert(__builtin_offsetof(Ia64SystemCallRegisterImage, R9) == 72U);
static_assert(__builtin_offsetof(Ia64SystemCallRegisterImage, R10) == 80U);
} // namespace Asteria::Platform::Ia64::System

#endif // ASTERIA_PLATFORM_IA64_SYSTEM_IA64_SYSTEM_CALL_REGISTER_CODEC_HPP_INCLUDED
