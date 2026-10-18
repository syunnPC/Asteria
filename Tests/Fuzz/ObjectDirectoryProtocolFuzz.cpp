// SPDX-License-Identifier: Apache-2.0
// Fuzz harness for the Object Directory v1 message profile
// (ObjectDirectoryProtocol).
//
// Input layout: one selector byte followed by a canonical IPC frame. The
// selector (modulo 8) chooses one of the eight Parse* functions; the frame is
// decoded with MessageCodec first, as a receiving service would.
//
// Properties checked for every input:
// - Parsing never reads outside the envelope (sanitizers report violations).
// - A rejected message leaves the parse output byte-for-byte unchanged.
// - An accepted message is canonical for its method: the matching Build*
//   function rebuilds exactly the decoded envelope from the parsed value (it
//   writes every envelope byte), and encoding the rebuilt envelope
//   reproduces exactly the input frame.

#include "Executive/ObjectDirectory/ObjectDirectoryProtocol.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
using Asteria::Executive::ObjectDirectory::BindIfAbsentRequest;
using Asteria::Executive::ObjectDirectory::DirectoryMessageContext;
using Asteria::Executive::ObjectDirectory::DirectoryName;
using Asteria::Executive::ObjectDirectory::DirectoryNameView;
using Asteria::Executive::ObjectDirectory::DirectoryStatus;
using Asteria::Executive::ObjectDirectory::MutationReply;
using Asteria::Executive::ObjectDirectory::ObjectDirectoryProtocol;
using Asteria::Executive::ObjectDirectory::ObjectDirectoryProtocolError;
using Asteria::Executive::ObjectDirectory::RemoveIfVersionRequest;
using Asteria::Executive::ObjectDirectory::ReplaceIfVersionRequest;
using Asteria::Executive::ObjectDirectory::ResolveReply;
using Asteria::Executive::ObjectDirectory::ResolveRequest;
using Asteria::Foundation::Size;
using Asteria::Foundation::UInt8;
using Asteria::Kernel::Ipc::InterfaceIdentifier;
using Asteria::Kernel::Ipc::MessageCodec;
using Asteria::Kernel::Ipc::MessageCodecError;
using Asteria::Kernel::Ipc::MessageEnvelope;
using Asteria::Kernel::Ipc::ReadOnlyByteView;
using Asteria::Kernel::Ipc::WritableByteView;
using Asteria::Kernel::Object::CapabilityHandle;
using Asteria::Kernel::Object::CapabilityRight;
using Asteria::Kernel::Object::CapabilityRights;
using Asteria::Kernel::Object::MonotonicTime;

template <typename TOutput>
using ParseFunction = ObjectDirectoryProtocolError (*)(const MessageEnvelope&, TOutput&) noexcept;
template <typename TOutput>
using BuildFunction = ObjectDirectoryProtocolError (*)(const TOutput&, MessageEnvelope&) noexcept;

[[noreturn]] void Violation(const char* const property)
{
    static_cast<void>(std::fprintf(stderr, "FUZZ PROPERTY VIOLATED: %s\n", property));
    std::abort();
}

void Require(const bool condition, const char* const property)
{
    if (!condition)
    {
        Violation(property);
    }
}

MessageEnvelope g_message{};
MessageEnvelope g_rebuilt{};
UInt8 g_frame[MessageCodec::MaximumWireSizeBytes];

// Fills an object with a byte pattern, so any write shows in a comparison.
template <typename T> void FillPattern(T& object, const UInt8 value) noexcept
{
    UInt8 pattern[sizeof(T)];
    for (UInt8& byte : pattern)
    {
        byte = value;
    }
    std::memcpy(&object, pattern, sizeof(T));
}

template <typename TOutput>
void CheckParse(const std::uint8_t* const frame, const std::size_t frameSize, const ParseFunction<TOutput> parse,
                const BuildFunction<TOutput> build)
{
    static TOutput sentinel{};
    static TOutput output{};
    static bool filled{false};
    if (!filled)
    {
        FillPattern(sentinel, 0xA5U);
        filled = true;
    }
    std::memcpy(&output, &sentinel, sizeof(TOutput));
    if (parse(g_message, output) != ObjectDirectoryProtocolError::None)
    {
        Require(std::memcmp(&output, &sentinel, sizeof(TOutput)) == 0,
                "a rejected message leaves the output unchanged");
        return;
    }

    FillPattern(g_rebuilt, 0x5AU);
    Require(build(output, g_rebuilt) == ObjectDirectoryProtocolError::None, "an accepted message rebuilds");
    Require(std::memcmp(&g_rebuilt, &g_message, sizeof(MessageEnvelope)) == 0,
            "an accepted message is the rebuild of its parsed value");
    const auto encoded =
        MessageCodec::Encode(g_rebuilt, WritableByteView{.Data = g_frame, .SizeBytes = sizeof(g_frame)});
    Require(encoded.HasValue() && (encoded.GetValue() == frameSize) && (std::memcmp(g_frame, frame, frameSize) == 0),
            "the rebuilt message encodes to the input frame");
}

// Seed values after the protocol unit tests.
[[nodiscard]] DirectoryName MakeName(const char* const text) noexcept
{
    UInt8 bytes[DirectoryName::MaximumSizeBytes]{};
    Size size{0U};
    while ((size < sizeof(bytes)) && (text[size] != '\0'))
    {
        bytes[size] = static_cast<UInt8>(text[size]);
        ++size;
    }
    const auto result = DirectoryName::Create(DirectoryNameView{.Data = bytes, .SizeBytes = size});
    Require(result.HasValue(), "seed name is valid");
    return result.GetValue();
}

[[nodiscard]] DirectoryMessageContext MakeContext(const bool traced) noexcept
{
    DirectoryMessageContext context{
        .Trace = {},
        .Deadline = traced ? MonotonicTime{0x1020'3040'5060'7080ULL} : MonotonicTime::Infinite(),
        .TransactionIdentifier = 0x8070'6050'4030'2010ULL,
    };
    if (traced)
    {
        context.Trace = {
            .TraceIdentifierHigh = 0x1122'3344'5566'7788ULL,
            .TraceIdentifierLow = 0x99AA'BBCC'DDEE'FF00ULL,
            .SpanIdentifier = 0x8877'6655'4433'2211ULL,
        };
    }
    return context;
}

[[nodiscard]] CapabilityRights PublishedRights() noexcept
{
    return CapabilityRights{CapabilityRight::Invoke}.With(CapabilityRight::Read).With(CapabilityRight::Write);
}

constexpr CapabilityHandle ReplyEndpoint{0x6162'6364'6566'6768ULL, 0x7172'7374'7576'7778ULL};
constexpr CapabilityHandle TargetEndpoint{0x3132'3334'3536'3738ULL, 0x4142'4344'4546'4748ULL};
constexpr InterfaceIdentifier ServiceInterface{0x0102'0304'0506'0708ULL, 0x1112'1314'1516'1718ULL};

UInt8 g_seed[1U + MessageCodec::MaximumWireSizeBytes];

// Encodes the envelope behind a selector byte, applies an optional one-byte
// patch to the frame, and emits the input.
void EmitFrame(void (*const emit)(const std::uint8_t* data, std::size_t size), const UInt8 selector,
               const MessageEnvelope& message, const Size patchOffset, const UInt8 patchValue)
{
    g_seed[0] = selector;
    const auto encoded =
        MessageCodec::Encode(message, WritableByteView{.Data = g_seed + 1U, .SizeBytes = sizeof(g_seed) - 1U});
    Require(encoded.HasValue(), "seed message encodes");
    if (patchOffset < encoded.GetValue())
    {
        g_seed[1U + patchOffset] = patchValue;
    }
    emit(g_seed, 1U + encoded.GetValue());
}

template <typename TValue>
void EmitBuilt(void (*const emit)(const std::uint8_t* data, std::size_t size), const UInt8 selector,
               const TValue& value, const BuildFunction<TValue> build)
{
    MessageEnvelope message{};
    Require(build(value, message) == ObjectDirectoryProtocolError::None, "seed value builds");
    constexpr Size noPatch{~Size{0U}};
    EmitFrame(emit, selector, message, noPatch, 0U);
}
} // namespace

extern "C" void AsteriaFuzzSeeds(void (*const emit)(const std::uint8_t* data, std::size_t size))
{
    const ResolveRequest resolve{
        .Name = MakeName("svc.one"),
        .ServiceInterface = ServiceInterface,
        .RequestedDeadline = MonotonicTime{0x2122'2324'2526'2728ULL},
        .RequiredVersion = {.Major = 0x3132U, .MinimumMinor = 0x4142U, .MaximumMinor = 0x5152U, .Reserved = 0U},
        .RequiredRights = PublishedRights(),
        .ReplyEndpoint = ReplyEndpoint,
        .Context = MakeContext(true),
    };
    const BindIfAbsentRequest bind{
        .Name = MakeName("a0-b_c.d9"),
        .ServiceInterface = ServiceInterface,
        .ServiceGeneration = 0x2122'2324'2526'2728ULL,
        .Version = {.Major = 3U, .MinimumMinor = 4U, .MaximumMinor = 9U, .Reserved = 0U},
        .PublishedRights = CapabilityRights{CapabilityRight::Invoke}.With(CapabilityRight::Duplicate),
        .TargetEndpoint = TargetEndpoint,
        .TargetDeadline = MonotonicTime{0x5152'5354'5556'5758ULL},
        .ReplyEndpoint = ReplyEndpoint,
        .Context = MakeContext(false),
    };
    const ReplaceIfVersionRequest replace{
        .Name = MakeName("0123456789abcdefghijklmnopqrstuvwxyz-0123456789abcdefghijklmnopq"),
        .ExpectedVersion = {.DirectoryGeneration = 0x8192'A3B4'C5D6'E7F8ULL, .Revision = 1U},
        .ServiceInterface = ServiceInterface,
        .ServiceGeneration = 1U,
        .Version = {.Major = 1U, .MinimumMinor = 0U, .MaximumMinor = 0U, .Reserved = 0U},
        .PublishedRights = PublishedRights(),
        .TargetEndpoint = TargetEndpoint,
        .TargetDeadline = MonotonicTime::Infinite(),
        .ReplyEndpoint = ReplyEndpoint,
        .Context = MakeContext(true),
    };
    const RemoveIfVersionRequest remove{
        .Name = MakeName("x"),
        .ExpectedVersion = {.DirectoryGeneration = 2U, .Revision = 3U},
        .ReplyEndpoint = ReplyEndpoint,
        .Context = MakeContext(true),
    };
    const ResolveReply found{
        .Status = DirectoryStatus::Success,
        .Version = {.DirectoryGeneration = 0x0102'0304'0506'0708ULL, .Revision = 0x1112'1314'1516'1718ULL},
        .ServiceInterface = ServiceInterface,
        .ServiceGeneration = 0x4142'4344'4546'4748ULL,
        .GrantedDeadline = MonotonicTime{0x5152'5354'5556'5758ULL},
        .Major = 0x6162U,
        .SelectedMinor = 0x7172U,
        .GrantedRights = PublishedRights(),
        .TargetEndpoint = TargetEndpoint,
        .Context = MakeContext(true),
    };
    const ResolveReply notFound{
        .Status = DirectoryStatus::NotFound,
        .Version = {},
        .ServiceInterface = {},
        .ServiceGeneration = 0U,
        .GrantedDeadline = {},
        .Major = 0U,
        .SelectedMinor = 0U,
        .GrantedRights = {},
        .TargetEndpoint = {},
        .Context = MakeContext(false),
    };
    const MutationReply bound{
        .Status = DirectoryStatus::Success,
        .Version = {.DirectoryGeneration = 0x0102'0304'0506'0708ULL, .Revision = 0x1112'1314'1516'1718ULL},
        .Context = MakeContext(true),
    };
    const MutationReply conflict{
        .Status = DirectoryStatus::BindingChanged,
        .Version = {},
        .Context = MakeContext(false),
    };

    EmitBuilt(emit, 0U, resolve, &ObjectDirectoryProtocol::BuildResolveRequest);
    EmitBuilt(emit, 1U, bind, &ObjectDirectoryProtocol::BuildBindIfAbsentRequest);
    EmitBuilt(emit, 2U, replace, &ObjectDirectoryProtocol::BuildReplaceIfVersionRequest);
    EmitBuilt(emit, 3U, remove, &ObjectDirectoryProtocol::BuildRemoveIfVersionRequest);
    EmitBuilt(emit, 4U, found, &ObjectDirectoryProtocol::BuildResolveReply);
    EmitBuilt(emit, 4U, notFound, &ObjectDirectoryProtocol::BuildResolveReply);
    EmitBuilt(emit, 5U, bound, &ObjectDirectoryProtocol::BuildBindIfAbsentReply);
    EmitBuilt(emit, 6U, conflict, &ObjectDirectoryProtocol::BuildReplaceIfVersionReply);
    EmitBuilt(emit, 7U, bound, &ObjectDirectoryProtocol::BuildRemoveIfVersionReply);

    // Near misses: a request parsed as another method, an uppercase name
    // byte, and a nonzero reserved flags word; the last byte of the frame is
    // the last name byte of a request and a reserved rights byte of a reply.
    MessageEnvelope message{};
    Require(ObjectDirectoryProtocol::BuildBindIfAbsentRequest(bind, message) == ObjectDirectoryProtocolError::None,
            "seed value builds");
    constexpr Size noPatch{~Size{0U}};
    EmitFrame(emit, 2U, message, noPatch, 0U);
    const Size frameSize = MessageCodec::WireHeaderSizeBytes + MessageCodec::WireVariablePayloadDescriptorSizeBytes +
                           (2U * MessageCodec::WireCapabilityAttachmentSizeBytes) + message.PayloadSizeBytes;
    EmitFrame(emit, 1U, message, frameSize - 1U, static_cast<UInt8>('Z'));
    EmitFrame(emit, 1U, message,
              frameSize - message.PayloadSizeBytes + ObjectDirectoryProtocol::BindIfAbsentRequestPayloadSizeBytes - 8U,
              1U);
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* const data, const std::size_t size)
{
    if (size < 1U)
    {
        return 0;
    }
    const std::uint8_t* const frame = data + 1U;
    const std::size_t frameSize = size - 1U;
    if (MessageCodec::Decode(ReadOnlyByteView{.Data = frame, .SizeBytes = frameSize}, g_message) !=
        MessageCodecError::None)
    {
        return 0;
    }
    switch (data[0] % 8U)
    {
    case 0U:
        CheckParse<ResolveRequest>(frame, frameSize, &ObjectDirectoryProtocol::ParseResolveRequest,
                                   &ObjectDirectoryProtocol::BuildResolveRequest);
        break;
    case 1U:
        CheckParse<BindIfAbsentRequest>(frame, frameSize, &ObjectDirectoryProtocol::ParseBindIfAbsentRequest,
                                        &ObjectDirectoryProtocol::BuildBindIfAbsentRequest);
        break;
    case 2U:
        CheckParse<ReplaceIfVersionRequest>(frame, frameSize, &ObjectDirectoryProtocol::ParseReplaceIfVersionRequest,
                                            &ObjectDirectoryProtocol::BuildReplaceIfVersionRequest);
        break;
    case 3U:
        CheckParse<RemoveIfVersionRequest>(frame, frameSize, &ObjectDirectoryProtocol::ParseRemoveIfVersionRequest,
                                           &ObjectDirectoryProtocol::BuildRemoveIfVersionRequest);
        break;
    case 4U:
        CheckParse<ResolveReply>(frame, frameSize, &ObjectDirectoryProtocol::ParseResolveReply,
                                 &ObjectDirectoryProtocol::BuildResolveReply);
        break;
    case 5U:
        CheckParse<MutationReply>(frame, frameSize, &ObjectDirectoryProtocol::ParseBindIfAbsentReply,
                                  &ObjectDirectoryProtocol::BuildBindIfAbsentReply);
        break;
    case 6U:
        CheckParse<MutationReply>(frame, frameSize, &ObjectDirectoryProtocol::ParseReplaceIfVersionReply,
                                  &ObjectDirectoryProtocol::BuildReplaceIfVersionReply);
        break;
    default:
        CheckParse<MutationReply>(frame, frameSize, &ObjectDirectoryProtocol::ParseRemoveIfVersionReply,
                                  &ObjectDirectoryProtocol::BuildRemoveIfVersionReply);
        break;
    }
    return 0;
}
