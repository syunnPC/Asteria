// SPDX-License-Identifier: Apache-2.0
#include "User/Session/SessionFrame.hpp"

#include <cstdio>
#include <initializer_list>
#include <cstring>

#include "Abi/IpcCanonicalFrame.hpp"
#include "Support/TestContext.hpp"

// The session protocol codec: a valid message of every method round-trips through the encoder and
// the decoder; every header, attachment and word rule refuses a frame that breaks exactly that rule
// (the encoder writes nothing, the decoder leaves the message untouched); the frame sizes stay
// inside the kernel's IPC limits.

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Foundation::Size;
using Foundation::UInt32;
using Foundation::UInt64;
using Foundation::UInt8;
using User::System::CapabilityHandle;
namespace S = User::Session;
namespace C = User::Component;

constexpr Size DeliveryBytes{C::EchoDeliveryHeaderBytes + S::SessionKernelFrameLimit};
constexpr UInt64 Transaction{0x1234U};

UInt8 g_frame[S::SessionKernelFrameLimit]{};
UInt8 g_scratch[S::SessionKernelFrameLimit]{};
UInt8 g_delivery[DeliveryBytes]{};

constexpr CapabilityHandle Handles[4U]{{0x11U, 0xA1U}, {0x12U, 0xA2U}, {0x13U, 0xA3U}, {0x14U, 0xA4U}};

[[nodiscard]] constexpr UInt64 Text8(const char* const text) noexcept
{
    UInt64 word{0U};
    for (Size index{0U}; (index < 8U) && (text[index] != '\0'); ++index)
    {
        word |= static_cast<UInt64>(static_cast<UInt8>(text[index])) << (index * 8U);
    }
    return word;
}

[[nodiscard]] Size WriteEvents(UInt8* frame, UInt64 transaction, UInt64 epoch, const S::SessionEvent* events,
                               Size count) noexcept
{
    return S::WriteSessionEvents(frame, transaction, epoch, events, count, 0U, 0U);
}

/// One valid event of each kind.
[[nodiscard]] S::SessionEvent BaseEvent(const S::SessionEventKind kind) noexcept
{
    using K = S::SessionEventKind;
    S::SessionEvent event{.Kind = kind, .Words = {}};
    switch (kind)
    {
    case K::Welcome:
        event.Words[0U] = 1U;
        event.Words[1U] = 0x5U;
        event.Words[2U] = 2U;
        event.Words[3U] = S::PackSessionPair(1920U, 1080U);
        break;
    case K::PoolReady:
        event.Words[0U] = 2U;

        break;
    case K::PoolRefused:
        event.Words[0U] = 1U;
        event.Words[1U] = static_cast<UInt64>(S::SessionRefusal::AddressBudget);
        break;
    case K::PoolReleased:
    case K::PoolDestroyed:
        event.Words[0U] = 2U;
        break;
    case K::ImageReturned:
        event.Words[0U] = 0x81U;
        break;
    case K::ImageRefused:
        event.Words[0U] = 8U;
        event.Words[1U] = static_cast<UInt64>(S::SessionRefusal::Range);
        break;
    case K::FrameShown:
        event.Words[0U] = 4U;
        event.Words[1U] = 77U;
        event.Words[2U] = 123456U;
        break;
    case K::Arrange:
        event.Words[0U] = 1U;
        event.Words[1U] = 9U;
        event.Words[2U] = S::PackSessionPair(640U, 400U);
        event.Words[3U] = S::SessionArrangeMaximized | S::SessionArrangeResizing;
        break;
    case K::CloseAsked:
    case K::PointerLeave:
        event.Words[0U] = 3U;
        break;
    case K::KeyFocus:
        event.Words[0U] = 2U;
        event.Words[1U] = 1U;
        break;
    case K::PointerEnter:
    case K::PointerMotion:
    case K::PointerButtons:
        event.Words[0U] = 1U;
        event.Words[1U] = S::PackSessionPosition(-5, 700);
        event.Words[2U] = kind == K::PointerEnter ? 0U : 7U;
        break;
    case K::Wheel:
        event.Words[0U] = 1U;
        event.Words[1U] = 0xFFFF'FFFFU;
        break;
    case K::Key:
        event.Words[0U] = 4U;
        event.Words[1U] = S::SessionKeyUp;
        event.Words[2U] = 0xE7U;
        event.Words[3U] = 0xFFU;
        break;
    case K::RequestRefused:
        event.Words[0U] = S::SessionSetSubtitleMethod;
        event.Words[1U] = static_cast<UInt64>(S::SessionRefusal::Epoch);
        event.Words[2U] = 3U;
        break;
    }
    return event;
}

/// A valid message of method: its words and attachment count.
void BaseMessage(const UInt32 method, UInt64 (&words)[S::SessionMaximumWords], Size& attachments) noexcept
{
    for (UInt64& word : words)
    {
        word = 0U;
    }
    attachments = 0U;
    switch (method)
    {
    case S::SessionQueryMethod:
        words[0U] = 7U;
        attachments = 1U;
        break;
    case S::SessionCreditMethod:
        words[0U] = 5U;
        words[1U] = 7U;
        attachments = 1U;
        break;
    case S::SessionCreatePoolMethod:
        // 2,025 pages (a 1920x1080 image) in three 512-page Frames and a
        // fourth of 489: the per-Frame counts requires.
        words[0U] = 1U;
        words[1U] = 2025U;
        words[2U] = 4U;
        words[4U] = 512U | (512U << 16U) | (512ULL << 32U) | (489ULL << 48U);
        attachments = 4U;
        break;
    case S::SessionAddPoolFramesMethod:
        words[0U] = 2U;
        words[1U] = 12U;
        attachments = 4U;
        break;
    case S::SessionDestroyPoolMethod:
        words[0U] = 2U;
        break;
    case S::SessionDefineImageMethod:
        words[0U] = 1U;
        words[1U] = 8U;
        words[2U] = 64U;
        words[3U] = S::PackSessionPair(640U, 400U);
        words[4U] = 640U;
        words[5U] = 2U;
        break;
    case S::SessionForgetImageMethod:
        words[0U] = 8U;
        break;
    case S::SessionCreateSurfaceMethod:
        words[0U] = 4U;
        words[1U] = S::SessionToplevelRole;
        break;
    case S::SessionDestroySurfaceMethod:
        words[0U] = 1U;
        break;
    case S::SessionPresentMethod:
        words[0U] = 1U;
        words[1U] = 2U;
        words[2U] = 99U;
        words[3U] = S::SessionWantFrameShown;
        words[4U] = 7U;
        words[5U] = 2U;
        words[6U] = S::PackSessionDamage(0U, 0U, 8U, 16U);
        words[7U] = S::PackSessionDamage(8184U, 8176U, 8U, 16U);
        break;
    case S::SessionSetSizeHintsMethod:
        words[0U] = 1U;
        words[1U] = S::PackSessionPair(64U, 32U);
        words[2U] = S::PackSessionPair(640U, 400U);
        words[3U] = S::PackSessionPair(8U, 16U);
        words[4U] = S::PackSessionPair(0U, 0U);
        break;
    case S::SessionSetSubtitleMethod:
        words[0U] = 2U;
        words[1U] = 11U;
        words[2U] = Text8("~/src/ker");
        words[3U] = Text8("nel");
        break;
    case S::SessionEventsMethod:
    {
        const S::SessionEvent key = BaseEvent(S::SessionEventKind::Key);
        const S::SessionEvent arrange = BaseEvent(S::SessionEventKind::Arrange);
        words[0U] = 3U;
        words[1U] = (2ULL << 56U) | static_cast<UInt64>(S::SessionEventKind::Key) |
                    (static_cast<UInt64>(S::SessionEventKind::Arrange) << 8U);
        for (Size index{0U}; index < 4U; ++index)
        {
            words[4U + index] = key.Words[index];
            words[8U + index] = arrange.Words[index];
        }
        break;
    }
    case S::SessionEpochChangedMethod:
        words[0U] = 9U;
        words[1U] = S::PackSessionPair(1920U, 1080U);
        words[2U] = 2U;
        attachments = 1U;
        break;
    case S::SessionRegisterClientMethod:
        words[0U] = 42U;
        words[1U] = S::SessionClassVolumeBundle;
        words[2U] = 8U;
        words[4U] = Text8("terminal");
        attachments = 1U;
        break;
    case S::SessionDropClientMethod:
        words[0U] = 3U | (17U << 8U);
        attachments = 1U;
        break;
    case S::SessionFaceChangedMethod:
        words[0U] = 6U;
        attachments = 1U;
        break;
    case S::SessionReplyMethod:
        words[0U] = Transaction;
        words[1U] = static_cast<UInt64>(S::SessionAnswer::Done);
        words[2U] = 2U | (5U << 8U);
        attachments = 1U;
        break;
    case S::SessionUserLaunchMethod:
    case S::SessionReadyMethod:
    case S::SessionNewTerminalMethod:
        words[0U] = 7U;
        break;
    default:
        break;
    }
}

[[nodiscard]] Size Encode(const UInt32 method, const UInt64* const words, const Size attachments) noexcept
{
    return S::WriteSessionFrame(g_frame, method, Transaction, words, Handles, attachments);
}

/// The frame as the kernel delivers it: a 48-byte delivery header first.
[[nodiscard]] Size Deliver(const Size size) noexcept
{
    std::memset(g_delivery, 0, sizeof(g_delivery));
    std::memcpy(g_delivery + C::EchoDeliveryHeaderBytes, g_frame, size);
    return C::EchoDeliveryHeaderBytes + size;
}

/// Decodes the delivery; on failure checks the message was left untouched.
[[nodiscard]] bool Decode(const Size bytes, S::SessionMessage& message, bool& untouched) noexcept
{
    S::SessionMessage before{};
    std::memset(&before, 0xA5, sizeof(before));
    std::memcpy(&message, &before, sizeof(message));
    const bool decoded = S::ReadSessionFrame(g_delivery, bytes, message);
    untouched = decoded || (std::memcmp(&before, &message, sizeof(message)) == 0);
    return decoded;
}

void TestRoundTrips(TestContext& runner) noexcept
{
    for (UInt32 method{S::SessionCreditMethod}; method <= S::SessionLastMethod; ++method)
    {
        UInt64 words[S::SessionMaximumWords]{};
        Size attachments{0U};
        BaseMessage(method, words, attachments);
        const Size size = Encode(method, words, attachments);
        const Size expected =
            C::EchoHeaderBytes + (attachments * C::EchoAttachmentBytes) + (S::SessionWordCount(method) * 8U);
        S::SessionMessage message{};
        bool untouched{false};
        const bool decoded = (size != 0U) && Decode(Deliver(size), message, untouched);
        bool same = decoded && (message.Method == method) && (message.Transaction == Transaction) &&
                    (message.WordCount == S::SessionWordCount(method)) && (message.AttachmentCount == attachments);
        for (Size index{0U}; same && (index < S::SessionMaximumWords); ++index)
        {
            same = message.Words[index] == words[index];
        }
        for (Size index{0U}; same && (index < S::SessionMaximumAttachments); ++index)
        {
            const CapabilityHandle handle = index < attachments ? Handles[index] : CapabilityHandle{};
            same = (message.Attachments[index].Selector == handle.Selector) &&
                   (message.Attachments[index].Authenticator == handle.Authenticator);
        }
        if (!ASTERIA_EXPECT(runner, "a valid message round-trips through the encoder and decoder", size == expected,
                            decoded, same, S::IsSessionMessage(method, words, attachments)))
        {
            std::fprintf(stderr, "  method %u\n", method);
        }
    }
    UInt64 words[S::SessionMaximumWords]{};
    Size attachments{0U};
    BaseMessage(S::SessionEpochChangedMethod, words, attachments);
    S::SessionMessage message{};
    bool untouched{false};
    ASTERIA_EXPECT(runner, "EpochChanged may come without a new Endpoint (restarts spent)",
                   Decode(Deliver(Encode(S::SessionEpochChangedMethod, words, 0U)), message, untouched),
                   message.AttachmentCount == 0U);
    BaseMessage(S::SessionReplyMethod, words, attachments);
    words[2U] = 0U;
    words[3U] = 0U;
    words[1U] = static_cast<UInt64>(S::SessionAnswer::Refused);
    ASTERIA_EXPECT(runner, "a refusal Reply carries no Endpoint and zero values",
                   Decode(Deliver(Encode(S::SessionReplyMethod, words, 0U)), message, untouched));
}

void TestSizes(TestContext& runner) noexcept
{
    UInt64 words[S::SessionMaximumWords]{};
    Size attachments{0U};
    BaseMessage(S::SessionCreatePoolMethod, words, attachments);
    const Size pool = Encode(S::SessionCreatePoolMethod, words, 4U);
    BaseMessage(S::SessionPresentMethod, words, attachments);
    const Size present = Encode(S::SessionPresentMethod, words, 0U);
    BaseMessage(S::SessionEventsMethod, words, attachments);
    const Size events = Encode(S::SessionEventsMethod, words, 0U);
    ASTERIA_EXPECT(runner, "the largest frames fit the 504-byte frame and 256-byte payload limits",
                   pool == (88U + 128U + 64U), present == (88U + 176U), events == (88U + 256U),
                   events <= S::SessionKernelFrameLimit, g_frame[12U] == 0U, g_frame[13U] == 1U, g_frame[14U] == 0U,
                   g_frame[15U] == 1U);
}

void TestHeaderRules(TestContext& runner) noexcept
{
    UInt64 words[S::SessionMaximumWords]{};
    Size attachments{0U};
    BaseMessage(S::SessionCreditMethod, words, attachments);
    const Size size = Encode(S::SessionCreditMethod, words, 1U);
    S::SessionMessage message{};
    bool untouched{false};
    // Every header bit but the attachment count's (checked below), the
    // Method's (another method may share the layout) and the transaction's (any nonzero value is
    // valid): canonical bytes, sizes and interface.
    Size refused{0U};
    for (Size byte{0U}; byte < C::EchoHeaderBytes; ++byte)
    {
        if ((byte == 81U) || ((byte >= 32U) && (byte < 36U)) || ((byte >= 40U) && (byte < 48U)))
        {
            continue;
        }
        for (UInt32 bit{0U}; bit < 8U; ++bit)
        {
            const Size bytes = Deliver(size);
            g_delivery[C::EchoDeliveryHeaderBytes + byte] ^= static_cast<UInt8>(1U << bit);
            refused += (!Decode(bytes, message, untouched) && untouched) ? 1U : 0U;
        }
    }
    ASTERIA_EXPECT(runner, "every fixed header bit is checked", refused == ((C::EchoHeaderBytes - 1U - 12U) * 8U));
    const Size bytes = Deliver(size);
    ASTERIA_EXPECT(runner, "a delivery one byte short or long is refused", !Decode(bytes - 1U, message, untouched),
                   untouched, !Decode(bytes + 1U, message, untouched), untouched,
                   !S::ReadSessionFrame(nullptr, bytes, message),
                   !S::ReadSessionFrame(g_delivery, C::EchoDeliveryHeaderBytes + C::EchoHeaderBytes - 1U, message));
    g_delivery[C::EchoDeliveryHeaderBytes + 40U] = 0U;
    g_delivery[C::EchoDeliveryHeaderBytes + 41U] = 0U;
    ASTERIA_EXPECT(runner, "a zero transaction is refused", !Decode(bytes, message, untouched), untouched);
    static_cast<void>(Deliver(size));
    g_delivery[C::EchoDeliveryHeaderBytes + 81U] = 2U;
    ASTERIA_EXPECT(runner, "an attachment count the size does not hold is refused", !Decode(bytes, message, untouched));
    static_cast<void>(Deliver(size));
    g_delivery[C::EchoDeliveryHeaderBytes + 32U] = 0U;
    ASTERIA_EXPECT(runner, "method zero is refused", !Decode(bytes, message, untouched));
    g_delivery[C::EchoDeliveryHeaderBytes + 32U] = S::SessionLastMethod + 1U;
    ASTERIA_EXPECT(runner, "an unknown method is refused", !Decode(bytes, message, untouched));
}

void TestAttachmentRules(TestContext& runner) noexcept
{
    struct Case
    {
        UInt32 Method;
        Size Attachment;
        Size Offset;
        UInt8 Value;
        const char* Rule;
    };
    static constexpr Case Cases[]{
        {S::SessionCreditMethod, 0U, 0U, 0U, "a zero selector"},
        {S::SessionCreditMethod, 0U, 16U, 0xFEU, "a finite deadline"},
        {S::SessionCreditMethod, 0U, 24U, 0x06U, "an Endpoint with more than Invoke"},
        {S::SessionCreditMethod, 0U, 26U, 1U, "a moved Endpoint"},
        {S::SessionCreditMethod, 0U, 27U, 8U, "a Frame where an Endpoint belongs"},
        {S::SessionCreditMethod, 0U, 28U, 1U, "a nonzero reserved byte"},
        {S::SessionCreditMethod, 0U, 31U, 1U, "a nonzero last reserved byte"},
        {S::SessionCreatePoolMethod, 3U, 24U, 0x18U, "a pool Frame with Write (requires Map alone)"},
        {S::SessionCreatePoolMethod, 1U, 24U, 0x14U, "a pool Frame with Read"},
        {S::SessionCreatePoolMethod, 0U, 27U, 9U, "an Endpoint where a Frame belongs"},
        {S::SessionAddPoolFramesMethod, 2U, 25U, 1U, "a pool Frame with Duplicate"},
        {S::SessionReplyMethod, 0U, 25U, 0U, "a minted Endpoint without Duplicate"},
        {S::SessionFaceChangedMethod, 0U, 24U, 0x0AU, "a face with Write"},
    };
    for (const Case& entry : Cases)
    {
        UInt64 words[S::SessionMaximumWords]{};
        Size attachments{0U};
        BaseMessage(entry.Method, words, attachments);
        const Size bytes = Deliver(Encode(entry.Method, words, attachments));
        g_delivery[C::EchoDeliveryHeaderBytes + C::EchoHeaderBytes + (entry.Attachment * 32U) + entry.Offset] =
            entry.Value;
        S::SessionMessage message{};
        bool untouched{false};
        if (!ASTERIA_EXPECT(runner, "an attachment record that breaks a rule is refused",
                            !Decode(bytes, message, untouched), untouched))
        {
            std::fprintf(stderr, "  rule: %s\n", entry.Rule);
        }
    }
    UInt64 words[S::SessionMaximumWords]{};
    Size attachments{0U};
    BaseMessage(S::SessionCreditMethod, words, attachments);
    const CapabilityHandle invalid[1U]{{0U, 1U}};
    ASTERIA_EXPECT(runner, "the encoder refuses a missing or invalid attachment",
                   S::WriteSessionFrame(g_frame, S::SessionCreditMethod, Transaction, words, nullptr, 1U) == 0U,
                   S::WriteSessionFrame(g_frame, S::SessionCreditMethod, Transaction, words, invalid, 1U) == 0U,
                   S::WriteSessionFrame(g_frame, S::SessionCreditMethod, 0U, words, Handles, 1U) == 0U,
                   S::WriteSessionFrame(nullptr, S::SessionCreditMethod, Transaction, words, Handles, 1U) == 0U,
                   S::WriteSessionFrame(g_frame, S::SessionCreditMethod, Transaction, words, Handles, 5U) == 0U,
                   S::WriteSessionFrame(g_frame, 0U, Transaction, words, Handles, 1U) == 0U,
                   S::WriteSessionFrame(g_frame, 18U, Transaction, words, Handles, 1U) == 0U);
}

/// One broken rule: method's valid message with one word (or the attachment
/// count) changed.
struct Mutation
{
    UInt32 Method;
    Size Word;
    UInt64 Value;
    Size Attachments;
    const char* Rule;
};

constexpr Size Same{~Size{0U}};

constexpr Mutation Mutations[]{
    {S::SessionCreditMethod, 0U, 0U, Same, "Credit epoch zero"},
    {S::SessionCreditMethod, 1U, 0U, Same, "Credit maximum zero"},
    {S::SessionCreditMethod, 1U, 8U, Same, "Credit maximum above seven"},
    {S::SessionCreditMethod, 2U, 1U, Same, "Credit word 2"},
    {S::SessionCreditMethod, 3U, 1U, Same, "Credit word 3"},
    {S::SessionCreditMethod, 0U, 5U, 0U, "Credit without its event Endpoint"},
    {S::SessionCreditMethod, 0U, 5U, 2U, "Credit with two Endpoints"},
    {S::SessionCreatePoolMethod, 0U, 0U, Same, "pool zero"},
    {S::SessionCreatePoolMethod, 0U, 5U, Same, "pool three"},
    {S::SessionCreatePoolMethod, 2U, 0U, Same, "no Frames"},
    {S::SessionCreatePoolMethod, 2U, 17U, Same, "seventeen Frames"},
    {S::SessionCreatePoolMethod, 2U, 3U, Same, "more attachments than Frames"},
    {S::SessionCreatePoolMethod, 1U, 7U, Same, "Frame 4 without a page count"},
    {S::SessionCreatePoolMethod, 4U, 512U | (512U << 16U) | (512ULL << 32U), Same, "Frame 3 without pages"},
    {S::SessionCreatePoolMethod, 4U, 513U | (512U << 16U) | (512ULL << 32U) | (489ULL << 48U), Same,
     "a Frame of 513 pages"},
    {S::SessionCreatePoolMethod, 5U, 1U, Same, "a page count beyond the pool's Frames"},
    {S::SessionCreatePoolMethod, 7U, 1ULL << 48U, Same, "Frame 15's count in a four-Frame pool"},
    {S::SessionCreatePoolMethod, 0U, 2U, 0U, "CreatePool without Frames"},
    {S::SessionAddPoolFramesMethod, 0U, 0U, Same, "AddPoolFrames pool zero"},
    {S::SessionAddPoolFramesMethod, 1U, 16U, Same, "AddPoolFrames index sixteen"},
    {S::SessionAddPoolFramesMethod, 1U, 13U, Same, "AddPoolFrames past Frame 15"},
    {S::SessionAddPoolFramesMethod, 2U, 1U, Same, "AddPoolFrames word 2"},
    {S::SessionAddPoolFramesMethod, 3U, 1U, Same, "AddPoolFrames word 3"},
    {S::SessionAddPoolFramesMethod, 0U, 2U, 0U, "AddPoolFrames without Frames"},
    {S::SessionDestroyPoolMethod, 0U, 3U, Same, "DestroyPool pool three"},
    {S::SessionDestroyPoolMethod, 1U, 1U, Same, "DestroyPool word 1"},
    {S::SessionDestroyPoolMethod, 0U, 2U, 1U, "DestroyPool with an attachment"},
    {S::SessionDefineImageMethod, 0U, 0U, Same, "image in pool zero"},
    {S::SessionDefineImageMethod, 1U, 0U, Same, "image zero"},
    {S::SessionDefineImageMethod, 1U, 9U, Same, "image nine"},
    {S::SessionDefineImageMethod, 2U, 32U, Same, "an offset off the 64-byte grid"},
    {S::SessionDefineImageMethod, 2U, S::SessionMaximumPoolBytes, Same, "an offset at the largest pool's end"},
    {S::SessionDefineImageMethod, 3U, S::PackSessionPair(0U, 400U), Same, "width zero"},
    {S::SessionDefineImageMethod, 3U, S::PackSessionPair(8193U, 400U), Same, "width 8,193"},
    {S::SessionDefineImageMethod, 3U, S::PackSessionPair(640U, 0U), Same, "height zero"},
    {S::SessionDefineImageMethod, 3U, S::PackSessionPair(640U, 8193U), Same, "height 8,193"},
    {S::SessionDefineImageMethod, 4U, 639U, Same, "a stride below the width"},
    {S::SessionDefineImageMethod, 4U, 8193U, Same, "a stride of 8,193"},
    {S::SessionDefineImageMethod, 4U, 0x1'0000'0280ULL, Same, "a stride with high bits"},
    {S::SessionDefineImageMethod, 3U, S::PackSessionPair(4096U, 4096U), Same, "an image larger than any pool"},
    {S::SessionDefineImageMethod, 5U, 0U, Same, "format zero"},
    {S::SessionDefineImageMethod, 5U, 3U, Same, "format three"},
    {S::SessionDefineImageMethod, 0U, 1U, 1U, "DefineImage with an attachment"},
    {S::SessionForgetImageMethod, 0U, 0U, Same, "ForgetImage image zero"},
    {S::SessionForgetImageMethod, 0U, 9U, Same, "ForgetImage image nine"},
    {S::SessionForgetImageMethod, 3U, 1U, Same, "ForgetImage word 3"},
    {S::SessionCreateSurfaceMethod, 0U, 0U, Same, "surface zero"},
    {S::SessionCreateSurfaceMethod, 0U, 5U, Same, "surface five"},
    {S::SessionCreateSurfaceMethod, 1U, 0U, Same, "role zero"},
    {S::SessionCreateSurfaceMethod, 1U, 2U, Same, "role two"},
    {S::SessionCreateSurfaceMethod, 2U, 1U, Same, "CreateSurface word 2"},
    {S::SessionDestroySurfaceMethod, 0U, 5U, Same, "DestroySurface surface five"},
    {S::SessionDestroySurfaceMethod, 3U, 1U, Same, "DestroySurface word 3"},
    {S::SessionPresentMethod, 0U, 0U, Same, "Present surface zero"},
    {S::SessionPresentMethod, 1U, 9U, Same, "Present image nine"},
    {S::SessionPresentMethod, 2U, 0U, Same, "Present serial zero"},
    {S::SessionPresentMethod, 3U, 2U, Same, "Present unknown flag"},
    {S::SessionPresentMethod, 5U, 17U, Same, "seventeen damage rectangles"},
    {S::SessionPresentMethod, 5U, 3U, Same, "a counted rectangle that is empty"},
    {S::SessionPresentMethod, 5U, 1U, Same, "an uncounted rectangle that is not zero"},
    {S::SessionPresentMethod, 6U, S::PackSessionDamage(0U, 0U, 0U, 16U), Same, "damage width zero"},
    {S::SessionPresentMethod, 6U, S::PackSessionDamage(0U, 0U, 8U, 0U), Same, "damage height zero"},
    {S::SessionPresentMethod, 7U, S::PackSessionDamage(8185U, 0U, 8U, 16U), Same, "damage past x 8,192"},
    {S::SessionPresentMethod, 7U, S::PackSessionDamage(0U, 8177U, 8U, 16U), Same, "damage past y 8,192"},
    {S::SessionPresentMethod, 0U, 1U, 1U, "Present with an attachment"},
    {S::SessionSetSizeHintsMethod, 0U, 0U, Same, "hints for surface zero"},
    {S::SessionSetSizeHintsMethod, 1U, S::PackSessionPair(8193U, 32U), Same, "a minimum width of 8,193"},
    {S::SessionSetSizeHintsMethod, 3U, S::PackSessionPair(8U, 8193U), Same, "an increment height of 8,193"},
    {S::SessionSetSizeHintsMethod, 4U, S::PackSessionPair(8193U, 0U), Same, "a base width of 8,193"},
    {S::SessionSetSizeHintsMethod, 2U, S::PackSessionPair(63U, 400U), Same, "a maximum width below the minimum"},
    {S::SessionSetSizeHintsMethod, 2U, S::PackSessionPair(640U, 31U), Same, "a maximum height below the minimum"},
    {S::SessionSetSubtitleMethod, 0U, 0U, Same, "subtitle for surface zero"},
    {S::SessionSetSubtitleMethod, 1U, 65U, Same, "a subtitle of 65 bytes"},
    {S::SessionSetSubtitleMethod, 1U, 10U, Same, "a byte after the subtitle"},
    {S::SessionSetSubtitleMethod, 2U, 0x1FU, Same, "a control byte in the subtitle"},
    {S::SessionSetSubtitleMethod, 3U, 0x7FU, Same, "DEL in the subtitle"},
    {S::SessionSetSubtitleMethod, 9U, 1ULL << 56U, Same, "the subtitle's last byte"},
    {S::SessionEventsMethod, 0U, 0U, Same, "Events epoch zero"},
    {S::SessionEventsMethod, 1U, 0x0911U, Same, "no events"},
    {S::SessionEventsMethod, 1U, (8ULL << 56U) | 0x0911U, Same, "eight events"},
    {S::SessionEventsMethod, 1U, (1ULL << 56U) | 0x0911U, Same, "a kind byte beyond the count"},
    {S::SessionEventsMethod, 1U, (2ULL << 56U) | 17U, Same, "an event without a kind"},
    {S::SessionEventsMethod, 1U, (2ULL << 56U) | 0x1311U, Same, "kind 19"},
    {S::SessionEventsMethod, 1U, (2ULL << 56U) | (1ULL << 48U) | 0x0911U, Same, "kind byte 7"},
    {S::SessionEventsMethod, 2U, 2U, Same, "Events word 3"},
    {S::SessionEventsMethod, 12U, 1U, Same, "an unused event's word"},
    {S::SessionEventsMethod, 31U, (1ULL << 56U) | 0x0911U, Same, "the last unused event word"},
    {S::SessionEventsMethod, 0U, 3U, 1U, "Events with an attachment"},
    {S::SessionEpochChangedMethod, 0U, 0U, Same, "EpochChanged epoch zero"},
    {S::SessionEpochChangedMethod, 1U, S::PackSessionPair(0U, 1080U), Same, "EpochChanged word 1"},
    {S::SessionEpochChangedMethod, 0U, 9U, 2U, "EpochChanged with two Endpoints"},
    {S::SessionRegisterClientMethod, 1U, 0U, Same, "class zero"},
    {S::SessionRegisterClientMethod, 1U, 3U, Same, "class three"},
    {S::SessionRegisterClientMethod, 2U, 0U, Same, "an empty name"},
    {S::SessionRegisterClientMethod, 2U, 33U, Same, "a name of 33 bytes"},
    {S::SessionRegisterClientMethod, 2U, 7U, Same, "a byte after the name"},
    {S::SessionRegisterClientMethod, 3U, 1U, Same, "RegisterClient word 3"},
    {S::SessionRegisterClientMethod, 4U, 0x0A, Same, "a control byte in the name"},
    {S::SessionRegisterClientMethod, 5U, 1U, Same, "the name's last word"},
    {S::SessionRegisterClientMethod, 0U, 42U, 0U, "RegisterClient without a reply Endpoint"},
    {S::SessionDropClientMethod, 0U, 0U, Same, "client zero"},
    {S::SessionDropClientMethod, 0U, 5U, Same, "client five"},
    {S::SessionDropClientMethod, 0U, 3U, Same, "generation zero"},
    {S::SessionDropClientMethod, 1U, 0x100'0000U, Same, "a generation of 25 bits"},
    {S::SessionDropClientMethod, 2U, 1U, Same, "DropClient word 2"},
    {S::SessionDropClientMethod, 0U, 3U, 0U, "DropClient without a reply Endpoint"},
    {S::SessionFaceChangedMethod, 0U, 0U, Same, "face epoch zero"},
    {S::SessionFaceChangedMethod, 3U, 1U, Same, "SetDisplayFace word 3"},
    {S::SessionFaceChangedMethod, 0U, 6U, 0U, "SetDisplayFace without the face"},
    {S::SessionReplyMethod, 0U, Transaction + 1U, Same, "a Reply to another transaction"},
    {S::SessionReplyMethod, 1U, 0U, Same, "answer zero"},
    {S::SessionReplyMethod, 1U, 6U, Same, "answer six"},
    {S::SessionReplyMethod, 1U, 2U, Same, "a minted Endpoint with a refusal"},
    {S::SessionReplyMethod, 2U, 0U, Same, "a minted Endpoint for client zero"},
    {S::SessionReplyMethod, 2U, 5U, Same, "a minted Endpoint for client five"},
    {S::SessionReplyMethod, 2U, 2U, Same, "a minted Endpoint of generation zero"},
    {S::SessionReplyMethod, 3U, 0x100'0000U, Same, "a minted Endpoint of a 25-bit generation"},
    {S::SessionReplyMethod, 0U, Transaction, 2U, "a Reply with two Endpoints"},
};

void TestMutations(TestContext& runner) noexcept
{
    for (const Mutation& mutation : Mutations)
    {
        UInt64 words[S::SessionMaximumWords]{};
        Size attachments{0U};
        BaseMessage(mutation.Method, words, attachments);
        // Encode the valid message, then break the delivered frame's bytes
        // by hand, so the decoder sees exactly one broken rule; the encoder
        // is asked for the broken message into a scratch frame.
        const Size valid = Encode(mutation.Method, words, attachments);
        Size bytes = Deliver(valid);
        const UInt64 original = words[mutation.Word];
        words[mutation.Word] = mutation.Value;
        const Size count = (mutation.Attachments == Same) ? attachments : mutation.Attachments;
        const bool encoderRefuses =
            S::WriteSessionFrame(g_scratch, mutation.Method, Transaction, words, Handles, count) == 0U;
        words[mutation.Word] = original;
        UInt8* const frame = g_delivery + C::EchoDeliveryHeaderBytes;
        if (count == attachments)
        {
            UInt8* const word = frame + C::EchoHeaderBytes + (attachments * 32U) + (mutation.Word * 8U);
            for (Size byte{0U}; byte < 8U; ++byte)
            {
                word[byte] = static_cast<UInt8>(mutation.Value >> (byte * 8U));
            }
        }
        else if (count < attachments)
        {
            // Drop the last attachments: move the payload down.
            const Size payload = S::SessionWordCount(mutation.Method) * 8U;
            std::memmove(frame + C::EchoHeaderBytes + (count * 32U), frame + C::EchoHeaderBytes + (attachments * 32U),
                         payload);
            const Size total = C::EchoHeaderBytes + (count * 32U) + payload;
            frame[81U] = static_cast<UInt8>(count);
            frame[8U] = static_cast<UInt8>(total);
            frame[9U] = static_cast<UInt8>(total >> 8U);
            bytes = C::EchoDeliveryHeaderBytes + total;
        }
        else
        {
            // Add attachments: copies of the first record (or a valid
            // Endpoint record when there is none).
            const Size payload = S::SessionWordCount(mutation.Method) * 8U;
            std::memmove(frame + C::EchoHeaderBytes + (count * 32U), frame + C::EchoHeaderBytes + (attachments * 32U),
                         payload);
            for (Size index{attachments}; index < count; ++index)
            {
                UInt8* const record = frame + C::EchoHeaderBytes + (index * 32U);
                std::memset(record, 0, 32U);
                record[0U] = static_cast<UInt8>(0x21U + index);
                std::memset(record + 16U, 0xFF, 8U);
                record[24U] = (attachments != 0U) ? frame[C::EchoHeaderBytes + 24U] : 2U;
                record[25U] = (attachments != 0U) ? frame[C::EchoHeaderBytes + 25U] : 0U;
                record[26U] = 2U;
                record[27U] = (attachments != 0U) ? frame[C::EchoHeaderBytes + 27U] : 9U;
            }
            const Size total = C::EchoHeaderBytes + (count * 32U) + payload;
            frame[81U] = static_cast<UInt8>(count);
            frame[8U] = static_cast<UInt8>(total);
            frame[9U] = static_cast<UInt8>(total >> 8U);
            bytes = C::EchoDeliveryHeaderBytes + total;
        }
        S::SessionMessage message{};
        bool untouched{false};
        if (!ASTERIA_EXPECT(runner, "a message that breaks one rule is refused by both sides", valid != 0U,
                            encoderRefuses, !Decode(bytes, message, untouched), untouched))
        {
            std::fprintf(stderr, "  rule: %s\n", mutation.Rule);
        }
    }
}

/// One broken event rule: a one-event Events frame of kind with one word changed.
struct EventMutation
{
    S::SessionEventKind Kind;
    Size Word;
    UInt64 Value;
};

void TestEvents(TestContext& runner) noexcept
{
    using K = S::SessionEventKind;
    S::SessionEvent events[S::SessionMaximumEvents]{};
    for (UInt32 kind{1U}; kind <= S::SessionLastEventKind; ++kind)
    {
        const S::SessionEvent event = BaseEvent(static_cast<K>(kind));
        for (S::SessionEvent& slot : events)
        {
            slot = event;
        }
        const Size size = WriteEvents(g_frame, Transaction, 11U, events, S::SessionMaximumEvents);
        S::SessionMessage message{};
        bool untouched{false};
        S::SessionEvent decoded{};
        const bool read = (size != 0U) && Decode(Deliver(size), message, untouched) &&
                          S::GetSessionEvent(message, S::SessionMaximumEvents - 1U, decoded);
        if (!ASTERIA_EXPECT(runner, "seven events of each kind round-trip", read, decoded.Kind == event.Kind,
                            std::memcmp(decoded.Words, event.Words, sizeof(event.Words)) == 0, message.Words[0U] == 11U,
                            !S::GetSessionEvent(message, S::SessionMaximumEvents, decoded)))
        {
            std::fprintf(stderr, "  kind %u\n", kind);
        }
    }
    static constexpr EventMutation Broken[]{
        {K::Welcome, 0U, 0U},
        {K::Welcome, 2U, 0U},
        {K::Welcome, 2U, 3U},
        {K::Welcome, 3U, S::PackSessionPair(0U, 1080U)},
        {K::Welcome, 3U, S::PackSessionPair(1920U, 32769U)},
        {K::PoolReady, 0U, 0U},
        {K::PoolReady, 1U, 8193U},
        {K::PoolReady, 2U, 1U},
        {K::PoolReady, 3U, 1U},
        {K::PoolRefused, 1U, 0U},
        {K::PoolRefused, 1U, 8U},
        {K::PoolRefused, 0U, 3U},
        {K::PoolRefused, 2U, 1U},
        {K::PoolRefused, 3U, 1U},
        {K::PoolReleased, 0U, 3U},
        {K::PoolReleased, 1U, 1U},
        {K::PoolDestroyed, 0U, 0U},
        {K::PoolDestroyed, 2U, 1U},
        {K::PoolDestroyed, 3U, 1U},
        {K::ImageReturned, 0U, 0U},
        {K::ImageReturned, 0U, 0x100U},
        {K::ImageReturned, 1U, 1U},
        {K::ImageReturned, 2U, 1U},
        {K::ImageReturned, 3U, 1U},
        {K::ImageRefused, 0U, 9U},
        {K::ImageRefused, 1U, 0U},
        {K::ImageRefused, 2U, 1U},
        {K::ImageRefused, 3U, 1U},
        {K::FrameShown, 0U, 5U},
        {K::FrameShown, 1U, 0U},
        {K::FrameShown, 3U, 1U},
        {K::Arrange, 0U, 0U},
        {K::Arrange, 1U, 0U},
        {K::Arrange, 2U, S::PackSessionPair(0U, 400U)},
        {K::Arrange, 2U, S::PackSessionPair(640U, 8193U)},
        {K::Arrange, 3U, 8U},
        {K::CloseAsked, 0U, 5U},
        {K::CloseAsked, 1U, 1U},
        {K::CloseAsked, 2U, 1U},
        {K::CloseAsked, 3U, 1U},
        {K::KeyFocus, 0U, 0U},
        {K::KeyFocus, 1U, 2U},
        {K::KeyFocus, 2U, 1U},
        {K::KeyFocus, 3U, 1U},
        {K::PointerEnter, 2U, 1U},
        {K::PointerEnter, 3U, 1U},
        {K::PointerLeave, 2U, 1U},
        {K::PointerMotion, 0U, 0U},
        {K::PointerButtons, 2U, 8U},
        {K::Wheel, 1U, 0U},
        {K::Wheel, 1U, 0x1'0000'0000ULL},
        {K::Wheel, 2U, 8U},
        {K::Wheel, 0U, 5U},
        {K::Key, 0U, 0U},
        {K::Key, 1U, 0U},
        {K::Key, 1U, 3U},
        {K::Key, 2U, 3U},
        {K::Key, 2U, 0xE8U},
        {K::Key, 3U, 0x100U},
        {K::RequestRefused, 0U, 0U},
        {K::RequestRefused, 0U, 12U},
        {K::RequestRefused, 1U, 0U},
        {K::RequestRefused, 2U, 0U},
    };
    for (const EventMutation& mutation : Broken)
    {
        S::SessionEvent event = BaseEvent(mutation.Kind);
        const bool valid = S::IsSessionEvent(event);
        const Size size = WriteEvents(g_frame, Transaction, 11U, &event, 1U);
        const Size bytes = Deliver(size);
        event.Words[mutation.Word] = mutation.Value;
        const bool encoderRefuses = WriteEvents(g_scratch, Transaction, 11U, &event, 1U) == 0U;
        UInt8* const word = g_delivery + C::EchoDeliveryHeaderBytes + C::EchoHeaderBytes + 32U + (mutation.Word * 8U);
        for (Size byte{0U}; byte < 8U; ++byte)
        {
            word[byte] = static_cast<UInt8>(mutation.Value >> (byte * 8U));
        }
        S::SessionMessage message{};
        bool untouched{false};
        if (!ASTERIA_EXPECT(runner, "an event that breaks one rule is refused by both sides", valid, size != 0U,
                            encoderRefuses, !Decode(bytes, message, untouched), untouched))
        {
            std::fprintf(stderr, "  kind %u word %zu\n", static_cast<UInt32>(mutation.Kind), mutation.Word);
        }
    }
    S::SessionEvent event = BaseEvent(K::Key);
    ASTERIA_EXPECT(runner, "the Events encoder refuses a bad count, epoch or transaction",
                   WriteEvents(g_frame, Transaction, 11U, &event, 0U) == 0U,
                   WriteEvents(g_frame, Transaction, 11U, &event, 8U) == 0U,
                   WriteEvents(g_frame, Transaction, 0U, &event, 1U) == 0U,
                   WriteEvents(g_frame, 0U, 11U, &event, 1U) == 0U,
                   WriteEvents(g_frame, Transaction, 11U, nullptr, 1U) == 0U,
                   WriteEvents(nullptr, Transaction, 11U, &event, 1U) == 0U);
    event.Kind = static_cast<K>(0U);
    ASTERIA_EXPECT(runner, "an event of kind zero or nineteen is invalid", !S::IsSessionEvent(event));
    event.Kind = static_cast<K>(19U);
    ASTERIA_EXPECT(runner, "kind nineteen", !S::IsSessionEvent(event));
    S::SessionMessage message{};
    message.Method = S::SessionCreditMethod;
    ASTERIA_EXPECT(runner, "GetSessionEvent of another method fails", !S::GetSessionEvent(message, 0U, event));
}

void TestConcreteLayout(TestContext& runner) noexcept
{
    UInt64 words[32U]{};
    words[0U] = 1U;
    words[1U] = 8192U;
    words[2U] = 16U;
    words[3U] = 12U;
    for (Size i{4U}; i < 8U; ++i)
    {
        words[i] = 512ULL | (512ULL << 16U) | (512ULL << 32U) | (512ULL << 48U);
    }
    ASTERIA_EXPECT(runner, "sixteen per-Frame counts and attached suffix",
                   Encode(S::SessionCreatePoolMethod, words, 4U) == 280U);
    words[3U] = 13U;
    ASTERIA_EXPECT(runner, "first plus attachment count bounded by Frame count",
                   Encode(S::SessionCreatePoolMethod, words, 4U) == 0U);
    words[3U] = 12U;
    words[1U] = 8191U;
    ASTERIA_EXPECT(runner, "declared total equals sum", Encode(S::SessionCreatePoolMethod, words, 4U) == 0U);
    words[1U] = 8192U;
    for (Size i{0U}; i < 16U; ++i)
    {
        const Size word = 4U + (i >> 2U);
        const Size shift = (i & 3U) * 16U;
        const UInt64 original = words[word];
        words[word] &= ~(0xFFFFULL << shift);
        ASTERIA_EXPECT(runner, "every Frame refuses zero pages", Encode(S::SessionCreatePoolMethod, words, 4U) == 0U);
        words[word] |= 513ULL << shift;
        ASTERIA_EXPECT(runner, "every Frame refuses 513 pages", Encode(S::SessionCreatePoolMethod, words, 4U) == 0U);
        words[word] = original;
    }
    const S::SessionEvent event = BaseEvent(S::SessionEventKind::Key);
    const Size size = S::WriteSessionEvents(g_frame, Transaction, 1U, &event, 1U, 1U, 65U);
    S::SessionMessage decoded{};
    bool untouched{false};
    ASTERIA_EXPECT(runner, "event count and header flags/drops", Decode(Deliver(size), decoded, untouched),
                   S::SessionEventCount(decoded) == 1U, decoded.Words[2U] == 1U, decoded.Words[3U] == 65U,
                   S::WriteSessionEvents(g_frame, Transaction, 1U, &event, 1U, 2U, 0U) == 0U);
    ASTERIA_EXPECT(runner, "client ids are slot plus bounded nonzero generation", S::IsSessionClient(257U),
                   S::IsSessionClient(0xFFFF'FF04U), !S::IsSessionClient(1U), !S::IsSessionClient(256U),
                   !S::IsSessionClient(261U), !S::IsSessionClient(0x1'0000'0101ULL));
}

void TestHelpers(TestContext& runner) noexcept
{
    const UInt64 pool = 2025U * S::SessionPageBytes;
    ASTERIA_EXPECT(runner, "an image filling its pool exactly fits; a pixel more does not",
                   S::SessionImageFits(0U, 1920U, 1080U, 1920U, 1920U * 1080U * 4U),
                   !S::SessionImageFits(64U, 1920U, 1080U, 1920U, 1920U * 1080U * 4U),
                   S::SessionImageFits(pool - (1920U * 1080U * 4U), 1920U, 1080U, 1920U, pool),
                   !S::SessionImageFits(pool + 64U, 1U, 1U, 1U, pool),
                   !S::SessionImageFits(0xFFFF'FFFF'FFFF'FFC0ULL, 1U, 1U, 1U, pool),
                   !S::SessionImageFits(0U, 0U, 1U, 1U, pool), !S::SessionImageFits(0U, 2U, 1U, 1U, pool),
                   !S::SessionImageFits(0U, 1U, 8193U, 1U, ~UInt64{0U}),
                   !S::SessionImageFits(0U, 1U, 1U, 8193U, ~UInt64{0U}), !S::SessionImageFits(0U, 1U, 0U, 1U, pool));
    // The stride, not the width, sets the rows' extent.
    ASTERIA_EXPECT(runner, "the stride counts for every row, the last included",
                   S::SessionImageFits(0U, 100U, 10U, 128U, 128U * 10U * 4U),
                   !S::SessionImageFits(0U, 100U, 10U, 128U, 128U * 10U * 4U - 1U));
    const User::Graphics::Rect unpacked = S::UnpackSessionDamage(S::PackSessionDamage(8184U, 8176U, 8U, 16U));
    ASTERIA_EXPECT(runner, "damage rectangles pack into 16-bit fields", unpacked.X == 8184, unpacked.Y == 8176,
                   unpacked.Width == 8U, unpacked.Height == 16U);
    UInt64 words[8U]{1U, 2022U, 4U, 0U, 512U | (511U << 16U) | (510ULL << 32U) | (489ULL << 48U), 0U, 0U, 0U};
    ASTERIA_EXPECT(runner, "a CreatePool's page counts read back per Frame",
                   S::SessionPoolFramePages(words, 0U) == 512U, S::SessionPoolFramePages(words, 1U) == 511U,
                   S::SessionPoolFramePages(words, 3U) == 489U, S::SessionPoolFramePages(words, 4U) == 0U,
                   S::SessionTextByte(words, 4U, 0U) == 0U, S::SessionTextByte(words, 4U, 2U) == 0xFFU);
    ASTERIA_EXPECT(
        runner, "word counts", S::SessionWordCount(0U) == 0U, S::SessionWordCount(S::SessionQueryMethod) == 4U,
        S::SessionWordCount(S::SessionLastMethod + 1U) == 0U, S::SessionWordCount(S::SessionEventsMethod) == 32U,
        S::SessionWordCount(S::SessionPresentMethod) == 22U, !S::IsSessionMessage(S::SessionCreditMethod, nullptr, 1U));
}
void SetTextByte(UInt64* words, const Size first, const Size at, const UInt8 byte) noexcept
{
    const Size word{first + at / 8U};
    const Size shift{(at % 8U) * 8U};
    words[word] = (words[word] & ~(UInt64{0xFFU} << shift)) | (static_cast<UInt64>(byte) << shift);
}
void TestUtf8(TestContext& runner) noexcept
{
    struct Case
    {
        const char* Name;
        UInt8 Bytes[4U];
        Size Count;
        bool Valid;
    };
    constexpr Case cases[]{{"Latin-1", {0xC3U, 0xA9U}, 2U, true},
                           {"box", {0xE2U, 0x94U, 0x80U}, 3U, true},
                           {"CJK", {0xE4U, 0xB8U, 0xADU}, 3U, true},
                           {"maximum", {0xF4U, 0x8FU, 0xBFU, 0xBFU}, 4U, true},
                           {"overlong two", {0xC0U, 0xAFU}, 2U, false},
                           {"overlong three", {0xE0U, 0x9FU, 0xBFU}, 3U, false},
                           {"overlong four", {0xF0U, 0x8FU, 0xBFU, 0xBFU}, 4U, false},
                           {"surrogate", {0xEDU, 0xA0U, 0x80U}, 3U, false},
                           {"above maximum", {0xF4U, 0x90U, 0x80U, 0x80U}, 4U, false},
                           {"truncated", {0xE2U, 0x94U}, 2U, false},
                           {"stray continuation", {0x80U}, 1U, false},
                           {"bad continuation", {0xC2U, 0x41U}, 2U, false},
                           {"bad lead", {0xF5U, 0x80U, 0x80U, 0x80U}, 4U, false}};
    for (const UInt32 method : {S::SessionSetSubtitleMethod, S::SessionRegisterClientMethod})
    {
        const Size first{method == S::SessionSetSubtitleMethod ? 2U : 4U};
        const Size lengthWord{method == S::SessionSetSubtitleMethod ? 1U : 2U};
        for (const auto& test : cases)
        {
            UInt64 words[S::SessionMaximumWords]{};
            Size attachments{0U};
            BaseMessage(method, words, attachments);
            for (Size at{first}; at < first + (method == S::SessionSetSubtitleMethod ? 8U : 2U); ++at)
            {
                words[at] = 0U;
            }
            words[lengthWord] = test.Count;
            // Make a valid frame of the same length, then mutate its text
            // directly to exercise decoder rejection independently of Write.
            for (Size at{0U}; at < test.Count; ++at)
            {
                SetTextByte(words, first, at, 'x');
            }
            const Size bytes{Deliver(Encode(method, words, attachments))};
            UInt8* const payload{g_delivery + C::EchoDeliveryHeaderBytes + C::EchoHeaderBytes +
                                 attachments * C::EchoAttachmentBytes};
            for (Size at{0U}; at < test.Count; ++at)
            {
                payload[first * 8U + at] = test.Bytes[at];
                SetTextByte(words, first, at, test.Bytes[at]);
            }
            S::SessionMessage message{};
            bool untouched{false};
            const bool decoded{Decode(bytes, message, untouched)};
            std::memset(g_scratch, 0xA5, sizeof(g_scratch));
            const Size encoded{S::WriteSessionFrame(g_scratch, method, Transaction, words, Handles, attachments)};
            ASTERIA_EXPECT(runner, test.Name, decoded == test.Valid, (encoded != 0U) == test.Valid, untouched,
                           test.Valid || (g_scratch[0U] == 0xA5U));
        }
    }
}
} // Namespace
} // Namespace Asteria::Test

int main()
{
    TestContext runner{"SessionFrameTests"};
    Asteria::Test::TestRoundTrips(runner);
    Asteria::Test::TestSizes(runner);
    Asteria::Test::TestHeaderRules(runner);
    Asteria::Test::TestAttachmentRules(runner);
    Asteria::Test::TestMutations(runner);
    Asteria::Test::TestEvents(runner);
    Asteria::Test::TestHelpers(runner);
    Asteria::Test::TestConcreteLayout(runner);
    Asteria::Test::TestUtf8(runner);
    return runner.Finish();
}
