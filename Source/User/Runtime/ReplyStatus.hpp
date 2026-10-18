// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_RUNTIME_REPLY_STATUS_HPP_INCLUDED
#define ASTERIA_USER_RUNTIME_REPLY_STATUS_HPP_INCLUDED

#include "User/Runtime/Status.hpp"

namespace Asteria::User::Runtime
{
/// No wire layout: the caller keeps diagnostics beside its handle, never in Status.
/// SocketFailed has two diagnostic words (driver kind/reason or zero/kernel error).
struct DecodedReply
{
    Status Outcome;
    Foundation::UInt64 DiagnosticDetail;
    Foundation::UInt64 DiagnosticExtra;
};

enum class CommitState : Foundation::UInt32
{
    Committed = 1U,
    NotCommitted = 2U,
};

/// Pure, bounded, nonblocking mappings of already decoded wire words.
/// Unknown kinds/refusal reasons are invalid decoder arguments. Open-ended failure
/// reasons never select an outcome. Success's detail/extra remain values in the frame.
[[nodiscard]] DecodedReply DecodeFileReply(Foundation::UInt64 kind, Foundation::UInt64 detail) noexcept;
[[nodiscard]] DecodedReply DecodeDatagramReply(Foundation::UInt64 kind, Foundation::UInt64 detail,
                                               Foundation::UInt64 extra) noexcept;
[[nodiscard]] DecodedReply DecodeConsoleReply(Foundation::UInt64 kind, Foundation::UInt64 detail) noexcept;
[[nodiscard]] DecodedReply DecodeBlockReply(Foundation::UInt64 kind, Foundation::UInt64 detail) noexcept;
/// The generation associated with Committed remains in the frame, outside CommitState.
[[nodiscard]] Foundation::Result<CommitState, Status> DecodeCommitState(Foundation::UInt64 kind,
                                                                        Foundation::UInt64 detail) noexcept;
} // namespace Asteria::User::Runtime

#endif
