// SPDX-License-Identifier: Apache-2.0
// Native status boundary and diagnostic-independence properties for arbitrary wire words.
#include "User/Runtime/ReplyStatus.hpp"
#include "User/Runtime/FileStatus.hpp"
#include "User/Runtime/KernelStatus.hpp"
#include "User/Runtime/GenericStatus.hpp"
#include "User/Runtime/DatagramStatus.hpp"
#include "User/Component/BlockFrame.hpp"
#include "User/Component/FsFrame.hpp"
#include "User/Component/SocketFrame.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace
{
namespace Runtime = Asteria::User::Runtime;
namespace Component = Asteria::User::Component;
using Asteria::Foundation::UInt32;
using Asteria::Foundation::UInt64;

void Require(const bool condition, const char* const property)
{
    if (!condition)
    {
        static_cast<void>(std::fprintf(stderr, "FUZZ PROPERTY VIOLATED: %s\n", property));
        std::abort();
    }
}

[[nodiscard]] UInt64 Word(const std::uint8_t* const bytes, const std::size_t count) noexcept
{
    UInt64 word{0U};
    for (std::size_t index{0U}; index < count; ++index)
    {
        word |= static_cast<UInt64>(bytes[index]) << (index * 8U);
    }
    return word;
}

template <typename TCode> void CheckEnum(const Runtime::Status status, const UInt32 number, const UInt32 last)
{
    const auto result = status.TryAs<TCode>();
    const bool known = status.IsFailure() && (status.GetFacility().Number() == number) && (status.Code() >= 1U) &&
                       (status.Code() <= last);
    Require(result.HasValue() == known, "enum conversion requires the right facility and a known nonzero code");
    if (known)
    {
        Require(Runtime::Status{result.GetValue()} == status, "a known enum round trips to its complete encoding");
    }
    else
    {
        Require(result.GetError() == status, "an unknown code remains available unchanged");
    }
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* const data, const std::size_t size)
{
    if (size != 28U)
    {
        return 0;
    }
    // Word(4) has proved the narrowing range, before constructing an integer boundary value.
    const UInt32 raw = static_cast<UInt32>(Word(data, 4U));
    const UInt64 kind = Word(data + 4U, 8U);
    const UInt64 detail = Word(data + 12U, 8U);
    const UInt64 extra = Word(data + 20U, 8U);
    const auto result = Runtime::Status::FromRaw(raw);
    const bool valid = (raw == 0U) || (((raw >> 31U) == 1U) && ((raw & 0x00FF'FFFFU) != 0U));
    Require(result.HasValue() == valid, "the raw constructor rejects exactly the reserved encodings");
    if (valid)
    {
        const Runtime::Status status = result.GetValue();
        Require(status.Raw() == raw, "unknown raw statuses round trip without data loss");
        CheckEnum<Runtime::FileStatus>(status, 5U, Runtime::LastFileStatus);
        CheckEnum<Runtime::GenericStatus>(status, 0U, Runtime::LastGenericStatus);
        CheckEnum<Runtime::DatagramStatus>(status, 6U, Runtime::LastDatagramStatus);
    }
    else
    {
        Require(result.GetError() == Runtime::GenericStatus::InvalidArgument, "invalid raw data has a native error");
    }
    const auto kernel = Runtime::FromKernelError(detail);
    const bool fitsKernelCode = detail <= 0x00FF'FFFFULL;
    Require(kernel.HasValue() == fitsKernelCode,
            "kernel decoding rejects width overflow without rejecting future codes");
    if (fitsKernelCode)
    {
        const UInt32 expected = (detail == 0U) ? 0U : (0x8100'0000U | static_cast<UInt32>(detail));
        Require(kernel.GetValue().Raw() == expected, "known and future kernel codes preserve identity");
        CheckEnum<Runtime::KernelStatus>(kernel.GetValue(), 1U, Runtime::LastKernelStatus);
    }
    // Only a closed SocketRefused reason may select a status from detail.
    const auto file = Runtime::DecodeFileReply(kind, detail);
    Require(file.Outcome == Runtime::DecodeFileReply(kind, extra).Outcome, "file diagnostics never select outcomes");
    Require(file.DiagnosticDetail == (file.Outcome.IsFailure() ? detail : 0U),
            "file failure diagnostics remain 64 bits");
    const auto block = Runtime::DecodeBlockReply(kind, detail);
    Require(block.Outcome == Runtime::DecodeBlockReply(kind, extra).Outcome, "block diagnostics never select outcomes");
    const auto socket = Runtime::DecodeDatagramReply(kind, detail, extra);
    if (kind != Component::SocketRefused)
    {
        Require(socket.Outcome == Runtime::DecodeDatagramReply(kind, extra, detail).Outcome,
                "open-ended socket diagnostics never select outcomes");
    }
    Require(socket.DiagnosticDetail == (socket.Outcome.IsFailure() ? detail : 0U), "socket detail is lossless");
    Require(socket.DiagnosticExtra == (socket.Outcome.IsFailure() ? extra : 0U), "socket extra is lossless");
    return 0;
}

extern "C" void AsteriaFuzzSeeds(void (*const emit)(const std::uint8_t* data, std::size_t size))
{
    const std::uint8_t seeds[][28U]{
        {},
        {1U, 0U, 0U, 0x85U, 9U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0xFFU},
        {5U, 0U, 0U, 0x86U, 4U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 17U},
        {13U, 0U, 0U, 0x80U, 2U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 2U},
        {0U, 0U, 0U, 0xFFU, 0xFFU},
    };
    for (const auto& seed : seeds)
    {
        emit(seed, sizeof(seed));
    }
}
