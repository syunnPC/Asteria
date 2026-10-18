// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_AMD64_SYSTEM_AMD64_SYSTEM_CALL_REGISTER_CODEC_HPP_INCLUDED
#define ASTERIA_PLATFORM_AMD64_SYSTEM_AMD64_SYSTEM_CALL_REGISTER_CODEC_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Kernel/System/SystemCallDispatcher.hpp"

namespace Asteria::Platform::Amd64::System
{
/// Internal AMD64 syscall register image; not user-shared memory.
struct Amd64SystemCallRegisterImage
{
    Foundation::UInt64 Rax;
    Foundation::UInt64 Rdi;
    Foundation::UInt64 Rsi;
    Foundation::UInt64 Rdx;
    Foundation::UInt64 R10;
    Foundation::UInt64 R8;
    Foundation::UInt64 R9;
};

/// Pure field-wise conversion between the AMD64 register convention and the
/// architecture-neutral syscall request and response types.
class Amd64SystemCallRegisterCodec final
{
public:
    Amd64SystemCallRegisterCodec() = delete;

    [[nodiscard]] static Kernel::System::SystemCallRequest
    DecodeRequest(const Amd64SystemCallRegisterImage& registers) noexcept;

    static void ApplyResponse(const Kernel::System::SystemCallResponse& response,
                              Amd64SystemCallRegisterImage& registers) noexcept;
};

static_assert(__is_standard_layout(Amd64SystemCallRegisterImage));
static_assert(__is_trivially_copyable(Amd64SystemCallRegisterImage));
static_assert(sizeof(Amd64SystemCallRegisterImage) == 56U);
static_assert(alignof(Amd64SystemCallRegisterImage) == 8U);
static_assert(__builtin_offsetof(Amd64SystemCallRegisterImage, Rax) == 0U);
static_assert(__builtin_offsetof(Amd64SystemCallRegisterImage, Rdi) == 8U);
static_assert(__builtin_offsetof(Amd64SystemCallRegisterImage, Rsi) == 16U);
static_assert(__builtin_offsetof(Amd64SystemCallRegisterImage, Rdx) == 24U);
static_assert(__builtin_offsetof(Amd64SystemCallRegisterImage, R10) == 32U);
static_assert(__builtin_offsetof(Amd64SystemCallRegisterImage, R8) == 40U);
static_assert(__builtin_offsetof(Amd64SystemCallRegisterImage, R9) == 48U);
} // namespace Asteria::Platform::Amd64::System

#endif // ASTERIA_PLATFORM_AMD64_SYSTEM_AMD64_SYSTEM_CALL_REGISTER_CODEC_HPP_INCLUDED
