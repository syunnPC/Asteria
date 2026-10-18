// SPDX-License-Identifier: Apache-2.0

#include "User/Runtime/ReplyStatus.hpp"

#include "User/Component/BlockFrame.hpp"
#include "User/Component/DisplayFrame.hpp"
#include "User/Component/FsFrame.hpp"
#include "User/Component/SocketFrame.hpp"
#include "User/Runtime/ConsoleStatus.hpp"
#include "User/Runtime/DatagramStatus.hpp"
#include "User/Runtime/FileStatus.hpp"
#include "User/Runtime/GenericStatus.hpp"

namespace Asteria::User::Runtime
{
DecodedReply DecodeFileReply(const Foundation::UInt64 kind, const Foundation::UInt64 detail) noexcept
{
    Status outcome{GenericStatus::InvalidArgument};
    switch (kind)
    {
    case Component::FsDone:
    case Component::FsCommitted:
    case Component::FsNotCommitted:
        outcome = Status{};
        break;
    case Component::FsRefused:
        outcome = GenericStatus::RefusedForCredit;
        break;
    case Component::FsCorrupt:
        outcome = FileStatus::Corrupt;
        break;
    case Component::FsStaleObject:
        outcome = GenericStatus::StaleObject;
        break;
    case Component::FsNotFound:
        outcome = FileStatus::NotFound;
        break;
    case Component::FsRestarted:
        outcome = GenericStatus::Restarted;
        break;
    case Component::FsFailed:
        outcome = FileStatus::IoFailed;
        break;
    case Component::FsRightsDenied:
        outcome = GenericStatus::AccessDenied;
        break;
    case Component::FsUnsupported:
        outcome = GenericStatus::NotSupported;
        break;
    default: // External kind, never converted to an enum.
        break;
    }
    return {.Outcome = outcome, .DiagnosticDetail = outcome.IsFailure() ? detail : 0U, .DiagnosticExtra = 0U};
}

DecodedReply DecodeDatagramReply(const Foundation::UInt64 kind, const Foundation::UInt64 detail,
                                 const Foundation::UInt64 extra) noexcept
{
    Status outcome{GenericStatus::InvalidArgument};
    switch (kind)
    {
    case Component::SocketDone:
        outcome = Status{};
        break;
    case Component::SocketRefused:
        // These are closed protocol reasons, unlike SocketFailed's diagnostic words.
        switch (detail)
        {
        case Component::SocketRefusedCredit:
            outcome = GenericStatus::RefusedForCredit;
            break;
        case Component::SocketRefusedNoRule:
            outcome = DatagramStatus::NoRule;
            break;
        case Component::SocketRefusedNotBound:
            outcome = DatagramStatus::NotBound;
            break;
        case Component::SocketRefusedIdentity:
            outcome = DatagramStatus::IdentityRefused;
            break;
        case Component::SocketRefusedMalformed:
            outcome = DatagramStatus::Malformed;
            break;
        default:
            break;
        }
        break;
    case Component::SocketPending:
        outcome = GenericStatus::Pending;
        break;
    case Component::SocketFailed:
        outcome = DatagramStatus::IoFailed;
        break;
    case Component::SocketRestarted:
        outcome = GenericStatus::Restarted;
        break;
    default:
        break;
    }
    return {.Outcome = outcome,
            .DiagnosticDetail = outcome.IsFailure() ? detail : 0U,
            .DiagnosticExtra = outcome.IsFailure() ? extra : 0U};
}

DecodedReply DecodeConsoleReply(const Foundation::UInt64 kind, const Foundation::UInt64 detail) noexcept
{
    Status outcome{GenericStatus::InvalidArgument};
    switch (kind)
    {
    case Component::DisplayDone:
        outcome = Status{};
        break;
    case Component::DisplayRefused:
        outcome = ConsoleStatus::Refused;
        break;
    default:
        break;
    }
    return {.Outcome = outcome, .DiagnosticDetail = outcome.IsFailure() ? detail : 0U, .DiagnosticExtra = 0U};
}

DecodedReply DecodeBlockReply(const Foundation::UInt64 kind, const Foundation::UInt64 detail) noexcept
{
    Status outcome{GenericStatus::InvalidArgument};
    switch (kind)
    {
    case Component::BlockDone:
        outcome = Status{};
        break;
    case Component::BlockMediumChanged:
    case Component::BlockFailed:
        outcome = FileStatus::IoFailed;
        break;
    case Component::BlockCancelled:
        outcome = FileStatus::Cancelled;
        break;
    case Component::BlockTooLate:
        outcome = GenericStatus::TooLate;
        break;
    case Component::BlockDriverRestarted:
        outcome = GenericStatus::Restarted;
        break;
    case Component::BlockRefused:
        outcome = GenericStatus::RefusedForCredit;
        break;
    default:
        break;
    }
    return {.Outcome = outcome, .DiagnosticDetail = outcome.IsFailure() ? detail : 0U, .DiagnosticExtra = 0U};
}

Foundation::Result<CommitState, Status> DecodeCommitState(const Foundation::UInt64 kind,
                                                          const Foundation::UInt64 detail) noexcept
{
    if (kind == Component::FsCommitted)
    {
        return Foundation::Result<CommitState, Status>::Success(CommitState::Committed);
    }
    if (kind == Component::FsNotCommitted)
    {
        return Foundation::Result<CommitState, Status>::Success(CommitState::NotCommitted);
    }
    const Status outcome = DecodeFileReply(kind, detail).Outcome;
    return Foundation::Result<CommitState, Status>::Failure(
        outcome.IsFailure() ? outcome : Status{GenericStatus::InvalidArgument});
}
} // namespace Asteria::User::Runtime
