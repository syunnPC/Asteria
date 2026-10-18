// SPDX-License-Identifier: Apache-2.0
#include "User/Runtime/ReplyStatus.hpp"
#include "User/Runtime/ConsoleStatus.hpp"
#include "User/Runtime/DatagramStatus.hpp"
#include "User/Runtime/FileStatus.hpp"
#include "User/Runtime/GenericStatus.hpp"
#include "User/Component/FsFrame.hpp"
#include "User/Component/SocketFrame.hpp"
#include "User/Component/BlockFrame.hpp"
#include "User/Component/DisplayFrame.hpp"

#include "Support/TestContext.hpp"

namespace Asteria::Test
{
namespace
{
namespace Component = User::Component;
namespace Runtime = User::Runtime;
using Foundation::Size;
using Foundation::UInt8;
using Foundation::UInt64;
using Runtime::Status;
using Runtime::FileStatus;
using Runtime::GenericStatus;
using Runtime::DatagramStatus;
constexpr Size Header{Component::EchoDeliveryHeaderBytes};
constexpr UInt64 Detail{0xFEDC'BA98'7654'3210ULL};
constexpr UInt64 Extra{0x1234'5678'9ABC'DEF0ULL};

void TestFile(TestSupport::TestContext& context) noexcept
{
    const Status expected[]{Status{},
                            Status{},
                            Status{},
                            GenericStatus::RefusedForCredit,
                            FileStatus::Corrupt,
                            GenericStatus::StaleObject,
                            FileStatus::NotFound,
                            GenericStatus::Restarted,
                            FileStatus::IoFailed,
                            GenericStatus::NotSupported};
    UInt8 delivery[Header + Component::FsReplyBytes]{};
    for (Size index{0U}; index < 10U; ++index)
    {
        const UInt64 words[Component::FsPayloadWords]{1U, index + 1U, Detail, Extra};
        const Size bytes = Component::WriteFsFrame(delivery + Header, Component::FsReplyMethod, 1U, words, nullptr, 0U,
                                                   nullptr, nullptr);
        Component::FsMessage frame{};
        const bool read = Component::ReadFsFrame(delivery, Header + bytes, frame);
        const auto decoded = Runtime::DecodeFileReply(frame.Words[1U], frame.Words[2U]);
        ASTERIA_EXPECT(context, "every file answer encoded on the wire selects its native outcome",
                       bytes == Component::FsReplyBytes, read, decoded.Outcome == expected[index],
                       decoded.DiagnosticDetail == (expected[index].IsFailure() ? Detail : 0U),
                       decoded.DiagnosticExtra == 0U);
    }
    const auto committed = Runtime::DecodeCommitState(Component::FsCommitted, 99U);
    const auto notCommitted = Runtime::DecodeCommitState(Component::FsNotCommitted, 0U);
    ASTERIA_EXPECT(context, "both QueryCommit answers are values, not errors", committed.HasValue(),
                   committed.GetValue() == Runtime::CommitState::Committed, notCommitted.HasValue(),
                   notCommitted.GetValue() == Runtime::CommitState::NotCommitted,
                   !Runtime::DecodeCommitState(Component::FsDone, 0U).HasValue());
    for (UInt64 kind{Component::FsRefused}; kind <= Component::FsRightsDenied; ++kind)
    {
        const auto result = Runtime::DecodeCommitState(kind, Detail);
        ASTERIA_EXPECT(context, "QueryCommit propagates every failed file answer", !result.HasValue(),
                       result.GetError() == Runtime::DecodeFileReply(kind, Detail).Outcome);
    }
    ASTERIA_EXPECT(context, "v2 view rights refusal has a stable status",
                   Runtime::DecodeFileReply(Component::FsRightsDenied, Detail).Outcome == GenericStatus::AccessDenied);
    ASTERIA_EXPECT(
        context, "file failure diagnostics do not select statuses",
        Runtime::DecodeFileReply(Component::FsFailed, Component::BlockNoDevice).Outcome == FileStatus::IoFailed,
        Runtime::DecodeFileReply(Component::FsFailed, Component::BlockDriverRestarted).Outcome == FileStatus::IoFailed,
        Runtime::DecodeFileReply(Component::FsUnsupported, Component::FsFormatFat).Outcome ==
            GenericStatus::NotSupported,
        Runtime::DecodeFileReply(Component::FsUnsupported, Component::FsFormatExfat).Outcome ==
            GenericStatus::NotSupported);
}

void TestDatagram(TestSupport::TestContext& context) noexcept
{
    const Status refused[]{GenericStatus::RefusedForCredit, DatagramStatus::NoRule, DatagramStatus::NotBound,
                           DatagramStatus::IdentityRefused, DatagramStatus::Malformed};
    UInt8 delivery[Header + Component::SocketReplyBytes]{};
    for (UInt64 reason{1U}; reason <= 5U; ++reason)
    {
        const UInt64 words[4U]{1U, Component::SocketRefused, reason, Extra};
        const Size bytes = Component::WriteSocketFrame(delivery + Header, Component::SocketReplyMethod, 1U, words,
                                                       nullptr, 0U, nullptr);
        Component::SocketMessage frame{};
        const bool read = Component::ReadSocketFrame(delivery, Header + bytes, frame);
        const auto decoded = Runtime::DecodeDatagramReply(frame.Words[1U], frame.Words[2U], frame.Words[3U]);
        ASTERIA_EXPECT(context, "closed SocketRefused reasons select distinct outcomes", read,
                       decoded.Outcome == refused[reason - 1U], decoded.DiagnosticDetail == reason,
                       decoded.DiagnosticExtra == Extra);
    }
    const Status expected[]{Status{}, GenericStatus::InvalidArgument, GenericStatus::Pending, DatagramStatus::IoFailed,
                            GenericStatus::Restarted};
    for (UInt64 kind{1U}; kind <= 5U; ++kind)
    {
        const UInt64 words[4U]{1U, kind, Detail, Extra};
        const Size bytes = Component::WriteSocketFrame(delivery + Header, Component::SocketReplyMethod, 1U, words,
                                                       nullptr, 0U, nullptr);
        Component::SocketMessage frame{};
        const bool read = Component::ReadSocketFrame(delivery, Header + bytes, frame);
        const auto decoded = Runtime::DecodeDatagramReply(frame.Words[1U], frame.Words[2U], frame.Words[3U]);
        ASTERIA_EXPECT(context, "every socket reply kind maps after its real frame codec", read,
                       decoded.Outcome == expected[kind - 1U],
                       decoded.DiagnosticDetail == (decoded.Outcome.IsFailure() ? Detail : 0U),
                       decoded.DiagnosticExtra == (decoded.Outcome.IsFailure() ? Extra : 0U));
    }
    const auto kernelFailure = Runtime::DecodeDatagramReply(Component::SocketFailed, 0U, 17U);
    ASTERIA_EXPECT(context, "SocketFailed preserves a kernel error outside the datagram outcome",
                   kernelFailure.Outcome == DatagramStatus::IoFailed, kernelFailure.DiagnosticDetail == 0U,
                   kernelFailure.DiagnosticExtra == 17U);
}

void TestBlock(TestSupport::TestContext& context) noexcept
{
    const Status expected[]{Status{},
                            FileStatus::IoFailed,
                            FileStatus::Cancelled,
                            GenericStatus::TooLate,
                            GenericStatus::Restarted,
                            GenericStatus::RefusedForCredit,
                            FileStatus::IoFailed};
    UInt8 delivery[Header + Component::BlockReplyBytes]{};
    for (Size index{0U}; index < 7U; ++index)
    {
        const UInt64 words[4U]{1U, index + 1U, Detail, 0U};
        const Size bytes =
            Component::WriteBlockFrame(delivery + Header, Component::BlockReplyMethod, 1U, words, nullptr, nullptr);
        Component::BlockMessage frame{};
        const bool read = Component::ReadBlockFrame(delivery, Header + bytes, frame);
        const auto decoded = Runtime::DecodeBlockReply(frame.Words[1U], frame.Words[2U]);
        ASTERIA_EXPECT(context, "every block reply kind maps without interpreting its open-ended detail", read,
                       decoded.Outcome == expected[index],
                       decoded.DiagnosticDetail == (expected[index].IsFailure() ? Detail : 0U),
                       decoded.DiagnosticExtra == 0U);
    }
}

void TestConsole(TestSupport::TestContext& context) noexcept
{
    UInt8 delivery[Header + Component::DisplayReplyBytes]{};
    const Component::DisplayInfo info{
        .Version = Detail, .Features = 1U, .Width = 800U, .Height = 600U, .Columns = 80U, .Rows = 25U, .Epoch = 2U};
    for (UInt64 kind{1U}; kind <= 2U; ++kind)
    {
        const Size bytes = Component::WriteDisplayReply(delivery + Header, 1U, kind, info);
        Component::DisplayMessage frame{};
        const bool read = Component::ReadDisplayFrame(delivery, Header + bytes, frame);
        const auto decoded = Runtime::DecodeConsoleReply(frame.Words[1U], frame.Words[2U]);
        ASTERIA_EXPECT(context, "display Query answers retain values and its state refusal stays console-specific",
                       read, decoded.Outcome == ((kind == 1U) ? Status{} : Status{Runtime::ConsoleStatus::Refused}),
                       decoded.DiagnosticDetail == ((kind == 1U) ? 0U : Detail));
    }
}

void TestInvalid(TestSupport::TestContext& context) noexcept
{
    const UInt64 kinds[]{0U, 12U, 0x1'0000'0001ULL, 0xFFFF'FFFF'FFFF'FFFFULL};
    for (const UInt64 kind : kinds)
    {
        ASTERIA_EXPECT(context, "unknown and wide kinds reject without truncation or enum conversion",
                       Runtime::DecodeFileReply(kind, Detail).Outcome == GenericStatus::InvalidArgument,
                       Runtime::DecodeBlockReply(kind, Detail).Outcome == GenericStatus::InvalidArgument,
                       Runtime::DecodeDatagramReply(kind, Detail, Extra).Outcome == GenericStatus::InvalidArgument,
                       Runtime::DecodeConsoleReply(kind, Detail).Outcome == GenericStatus::InvalidArgument,
                       !Runtime::DecodeCommitState(kind, Detail).HasValue());
    }
    const UInt64 reasons[]{0U, 6U, 0x1'0000'0001ULL};
    for (const UInt64 reason : reasons)
    {
        ASTERIA_EXPECT(context, "unknown closed refusal reasons are rejected",
                       Runtime::DecodeDatagramReply(Component::SocketRefused, reason, 0U).Outcome ==
                           GenericStatus::InvalidArgument);
    }
}
} // namespace
} // namespace Asteria::Test

int main()
{
    Asteria::TestSupport::TestContext context{"ReplyStatusTests"};
    Asteria::Test::TestFile(context);
    Asteria::Test::TestDatagram(context);
    Asteria::Test::TestBlock(context);
    Asteria::Test::TestConsole(context);
    Asteria::Test::TestInvalid(context);
    return context.Finish();
}
