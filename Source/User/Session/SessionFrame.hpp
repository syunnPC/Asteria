// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_SESSION_SESSION_FRAME_HPP_INCLUDED
#define ASTERIA_USER_SESSION_SESSION_FRAME_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "User/Component/EchoFrame.hpp"
#include "User/Graphics/Surface.hpp"
#include "User/System/SystemCallClient.hpp"

namespace Asteria::User::Session
{
/// The session compositor's protocol :
/// canonical asynchronous AIPC frames (Documentation/Architecture/
/// IpcWireFormat.md) built field by field, as the display and input codecs
/// are. A frame is the 88-byte header, its capability attachments (32 bytes
/// each), then a payload of a fixed number of little-endian 64-bit words per
/// method; the header's payload and fixed-prefix sizes both carry the
/// payload's size (a real 16-bit field: the Events payload is exactly the
/// kernel's 256-byte limit). Every unused word, slot and byte is zero, so each
/// accepted message has one encoding. Decoding is failure-atomic and checks
/// everything the bytes alone can tell; rights, epochs, table state and pool
/// sizes are the compositor's.
///
///  client protocol (frame bytes = 88 + 32 per attachment + 8
/// per word):
///
/// | Method | Words | Attachments | Bytes |
/// | --- | --- | --- | --- |
/// | 1 Credit | epoch, maximum events 1-7, 0, 0 | the client's event Endpoint (Invoke), retained | 152 |
/// | 2 CreatePool | pool 1-2, total pages, Frame count 1-16, first attached Frame, four words of sixteen 16-bit page counts | 1-4 Frames (Map) | 280 |
/// | 3 AddPoolFrames | pool, first attached Frame, 0, 0 | 1-4 Frames (Map) | 248 |
/// | 4 DestroyPool | pool, 0, 0, 0 | none | 120 |
/// | 5 DefineImage | pool, image 1-8, offset, width, height << 32, stride, format | none | 136 |
/// | 6 ForgetImage | image, 0, 0, 0 | none | 120 |
/// | 7 CreateSurface | surface 1-4, role 1 (top-level), 0, 0 | none | 120 |
/// | 8 DestroySurface | surface, 0, 0, 0 | none | 120 |
/// | 9 Present | surface, image 0-8, serial, flags, accepted Arrange serial, count 0-16, 16 rectangles | none | 264 |
/// | 10 SetSizeHints | surface, minimum, maximum, increment, base (each width, height << 32) | none | 128 |
/// | 11 SetSubtitle | surface, length 0-64, eight words of printable UTF-8 | none | 168 |
/// | 12 Events | epoch, kinds (bytes 0-6) and count (byte 7), flags, dropped, seven events of four words | none | 344 |
/// | 13 DisplayEpochChanged | epoch, output width, height << 32, format, features | the new minted Endpoint (Invoke) or none | 152 or 120 |
///
///  session-manager methods (the method numbers are this
/// codec's):
///
/// | Method | Words | Attachments | Bytes |
/// | --- | --- | --- | --- |
/// | 14 RegisterClient | launch serial (0: a system launch), class 1-2, name length 1-16, 0, two words of UTF-8 | reply (Invoke) | 168 |
/// | 15 DropClient | client, 0, 0, 0 | reply (Invoke) | 152 |
/// | 16 UserLaunch | serial, 0, 0, 0 | none | 120 |
/// | 17 FaceChanged | broker epoch, 0, 0, 0 | the broker's new session face (Invoke) | 152 |
/// | 18 Reply | transaction answered, answer 1-5, value, 0 | none, or a registration's minted Endpoint (Invoke, Duplicate) | 120 or 152 |
/// | 19 Ready | compositor epoch, 0, 0, 0 | none | 120 |
/// | 20 NewTerminal | compositor epoch, 0, 0, 0 | none | 120 |
///
/// Methods 1-11 travel on a client's minted Endpoint, one way: every outcome
/// comes back as an event in the answer to a Credit (12, on the client's
/// event Endpoint). 13 is the CM's, on the client's event Endpoint, after a
/// compositor restart. 14-17 are the CM's (Manage) on the compositor's main
/// Endpoint, 14 and 15 answered by 18; 19 and 20 are the compositor's, on the
/// CM's request Endpoint. A client is named by one word: its slot 1-4 in bits
/// 0-7 and its registration's generation (1 to 2^24 - 1) in bits 8-31.
///
/// A pool's Frames are copies with Map alone (0x0010): a read-only mapping
/// needs Map alone, so the compositor never holds Write on a
/// client's Frame. CreatePool lists every Frame's page count (1-512, at most
/// 8,192 in all) and their total; counts beyond the pool's Frames
/// are zero; a message's attachments are the pool's Frames first, first + 1,
/// and so on.
inline constexpr Foundation::UInt64 SessionInterfaceLow{0x4153'5452'5345'5301ULL};  // "ASTRSES", version 1
inline constexpr Foundation::UInt64 SessionInterfaceHigh{0x5355'5246'4143'4501ULL}; // "SURFACE", 1
inline constexpr Foundation::UInt64 SessionProtocolVersion{1U};

inline constexpr Foundation::UInt32 SessionCreditMethod{1U};
inline constexpr Foundation::UInt32 SessionCreatePoolMethod{2U};
inline constexpr Foundation::UInt32 SessionAddPoolFramesMethod{3U};
inline constexpr Foundation::UInt32 SessionDestroyPoolMethod{4U};
inline constexpr Foundation::UInt32 SessionDefineImageMethod{5U};
inline constexpr Foundation::UInt32 SessionForgetImageMethod{6U};
inline constexpr Foundation::UInt32 SessionCreateSurfaceMethod{7U};
inline constexpr Foundation::UInt32 SessionDestroySurfaceMethod{8U};
inline constexpr Foundation::UInt32 SessionPresentMethod{9U};
inline constexpr Foundation::UInt32 SessionSetSizeHintsMethod{10U};
inline constexpr Foundation::UInt32 SessionSetSubtitleMethod{11U};
inline constexpr Foundation::UInt32 SessionEventsMethod{12U};
inline constexpr Foundation::UInt32 SessionEpochChangedMethod{13U};
inline constexpr Foundation::UInt32 SessionRegisterClientMethod{14U};
inline constexpr Foundation::UInt32 SessionDropClientMethod{15U};
inline constexpr Foundation::UInt32 SessionUserLaunchMethod{16U};
inline constexpr Foundation::UInt32 SessionFaceChangedMethod{17U};
inline constexpr Foundation::UInt32 SessionReplyMethod{18U};
inline constexpr Foundation::UInt32 SessionReadyMethod{19U};
inline constexpr Foundation::UInt32 SessionNewTerminalMethod{20U};
/// Root-owned bounded liveness Query: epoch and three zero words, reply (Invoke).
inline constexpr Foundation::UInt32 SessionQueryMethod{21U};
inline constexpr Foundation::UInt32 SessionLastMethod{21U};
/// The last of the client's requests (methods 1 to this).
inline constexpr Foundation::UInt32 SessionLastClientMethod{SessionSetSubtitleMethod};

/// The compositor's tables.
inline constexpr Foundation::UInt64 SessionMaximumClients{4U};
inline constexpr Foundation::UInt64 SessionMaximumPools{2U};
inline constexpr Foundation::UInt64 SessionMaximumImages{8U};
inline constexpr Foundation::UInt64 SessionMaximumSurfaces{4U};
inline constexpr Foundation::UInt64 SessionMaximumPoolFrames{16U};
/// A Frame of at most 512 pages, a pool of at most one MemoryObject's 8,192.
inline constexpr Foundation::UInt64 SessionMaximumFramePages{512U};
inline constexpr Foundation::UInt64 SessionMaximumPoolPages{8192U};
inline constexpr Foundation::UInt64 SessionPageBytes{4096U};
inline constexpr Foundation::UInt64 SessionMaximumPoolBytes{SessionMaximumPoolPages * SessionPageBytes};
/// Image width, height and stride (pixels); an image offset's alignment; an
/// output's side (Composition.cpp's limit).
inline constexpr Foundation::UInt64 SessionMaximumExtent{8192U};
inline constexpr Foundation::UInt64 SessionImageAlignment{64U};
inline constexpr Foundation::UInt64 SessionMaximumOutputExtent{32768U};
inline constexpr Foundation::Size SessionMaximumEvents{7U};
inline constexpr Foundation::Size SessionMaximumDamage{16U};
inline constexpr Foundation::Size SessionMaximumAttachments{4U};
inline constexpr Foundation::Size SessionMaximumNameBytes{16U};
inline constexpr Foundation::Size SessionMaximumSubtitleBytes{64U};
inline constexpr Foundation::UInt64 SessionMaximumGeneration{0xFF'FFFFU};
inline constexpr Foundation::UInt64 SessionToplevelRole{1U};
/// Present's flags: bit 0 asks for FrameShown.
inline constexpr Foundation::UInt64 SessionWantFrameShown{1U};
/// Arrange's flags.
inline constexpr Foundation::UInt64 SessionArrangeFocused{1U};
inline constexpr Foundation::UInt64 SessionArrangeMaximized{2U};
inline constexpr Foundation::UInt64 SessionArrangeResizing{4U};
/// The Events header's flags: bit 0, more is pending than this answer holds.
inline constexpr Foundation::UInt64 SessionEventsMorePending{1U};
/// Key events' actions.
inline constexpr Foundation::UInt64 SessionKeyDown{1U};
inline constexpr Foundation::UInt64 SessionKeyUp{2U};

inline constexpr Foundation::Size SessionMaximumWords{32U};
inline constexpr Foundation::Size SessionEventWords{4U};
inline constexpr Foundation::Size SessionMaximumPayloadBytes{SessionMaximumWords * 8U};
inline constexpr Foundation::Size SessionMaximumFrameBytes{Component::EchoHeaderBytes + SessionMaximumPayloadBytes};
/// The kernel's frame, payload and delivery limits (MessageCodec,
/// IpcWireFormat.md; the pager's DeliveryCapacityBytes).
inline constexpr Foundation::Size SessionKernelPayloadLimit{256U};
inline constexpr Foundation::Size SessionKernelFrameLimit{504U};
inline constexpr Foundation::Size SessionDeliveryCapacity{552U};
static_assert(SessionMaximumPayloadBytes <= SessionKernelPayloadLimit);
static_assert((Component::EchoHeaderBytes + (SessionMaximumAttachments * Component::EchoAttachmentBytes) + (8U * 8U)) ==
              280U);
static_assert(SessionMaximumFrameBytes <= SessionKernelFrameLimit);
static_assert((SessionMaximumFrameBytes + Component::EchoDeliveryHeaderBytes) <= SessionDeliveryCapacity);
static_assert((4U + (SessionMaximumEvents * SessionEventWords)) == SessionMaximumWords);

/// The Map right (CapabilityRight::Map) of a pool Frame's attachment and the Duplicate right a
/// registration's minted Endpoint carries besides Invoke.
inline constexpr Foundation::UInt16 SessionMapRight{0x0010U};
inline constexpr Foundation::UInt16 SessionDuplicateRight{0x0100U};

/// The sealed selectors of the compositor's Endpoints: the main Endpoint, and a client's (or'ed
/// with its client word), and the main Endpoint's WaitSet tag.
inline constexpr Foundation::UInt64 SessionSealedSelector{0x5345'5353'4D41'494EULL};
inline constexpr Foundation::UInt64 SessionClientSelector{0x5345'5343'0000'0000ULL};
inline constexpr Foundation::UInt64 SessionServiceWaitTag{0x5345'5353U};

/// An event's kind (one byte per event in header word 1), in order.
enum class SessionEventKind : Foundation::UInt8
{
    Welcome = 1U,
    PoolReady = 2U,
    PoolRefused = 3U,
    PoolReleased = 4U,
    PoolDestroyed = 5U,
    ImageReturned = 6U,
    ImageRefused = 7U,
    FrameShown = 8U,
    Arrange = 9U,
    KeyFocus = 10U,
    CloseAsked = 11U,
    PointerEnter = 12U,
    PointerLeave = 13U,
    PointerMotion = 14U,
    PointerButtons = 15U,
    Wheel = 16U,
    Key = 17U,
    RequestRefused = 18U,
};
inline constexpr Foundation::UInt8 SessionLastEventKind{18U};

/// Why a request, a pool or an image was refused.
enum class SessionRefusal : Foundation::UInt64
{
    /// An unknown or busy pool, image or surface slot; a request out of order.
    State = 1U,
    /// A table of the client's or the compositor's is full.
    Exhausted = 2U,
    /// The image does not lie inside its pool.
    Range = 3U,
    /// The image's format is not the output's.
    Format = 4U,
    /// The kernel refused a mapping (FrameRangeExceeded: a page count lied).
    MapFailed = 5U,
    /// The compositor's address or page-table budget is spent.
    AddressBudget = 6U,
    /// The request names an epoch that is not the compositor's.
    Epoch = 7U,
};
inline constexpr Foundation::UInt64 SessionLastRefusal{7U};

/// A Reply's answer.
enum class SessionAnswer : Foundation::UInt64
{
    Done = 1U,
    Refused = 2U,
    Exhausted = 3U,
    Busy = 4U,
    Restarted = 5U,
};

/// A client's registered class, which its windows' trusted titles show.
inline constexpr Foundation::UInt64 SessionClassSystemModule{1U};
inline constexpr Foundation::UInt64 SessionClassVolumeBundle{2U};

/// One decoded frame. Words beyond the method's count are zero; the
/// attachments are the receiver's handles, in frame order.
struct SessionMessage
{
    Foundation::UInt32 Method;
    Foundation::UInt64 Transaction;
    Foundation::UInt64 Words[SessionMaximumWords];
    Foundation::Size WordCount;
    Foundation::Size AttachmentCount;
    System::CapabilityHandle Attachments[SessionMaximumAttachments];
};

/// One event of an Events frame.
struct SessionEvent
{
    SessionEventKind Kind;
    Foundation::UInt64 Words[SessionEventWords];
};

/// The payload words of a method; zero for an unknown method.
[[nodiscard]] Foundation::Size SessionWordCount(Foundation::UInt32 method) noexcept;

/// Whether the words (SessionWordCount(method) of them) and the attachment
/// count form a valid message of the method: the stateless rules the
/// decoder applies, which the encoder applies too, so neither side can
/// produce or accept a frame the other refuses.
[[nodiscard]] bool IsSessionMessage(Foundation::UInt32 method, const Foundation::UInt64* words,
                                    Foundation::Size attachments) noexcept;

/// Whether one event's words are valid for its kind.
[[nodiscard]] bool IsSessionEvent(const SessionEvent& event) noexcept;

/// Whether a word names a client: slot 1-4 in bits 0-7, generation 1 to
/// 2^24 - 1 in bits 8-31, nothing above.
[[nodiscard]] constexpr bool IsSessionClient(const Foundation::UInt64 client) noexcept
{
    const Foundation::UInt64 slot = client & 0xFFU;
    const Foundation::UInt64 generation = client >> 8U;
    return (slot >= 1U) && (slot <= SessionMaximumClients) && (generation >= 1U) &&
           (generation <= SessionMaximumGeneration);
}

/// Writes a frame of method into frame (at most SessionKernelFrameLimit
/// bytes): attachments are the sender's handles, given the rights the method
/// requires (Map for Frames, Invoke for Endpoints, Invoke|Duplicate for a
/// registration's minted Endpoint). Returns the frame's size, or zero,
/// writing nothing, when the message is not valid (IsSessionMessage), a
/// Reply's word 0 is not its transaction, or transaction is zero.
[[nodiscard]] Foundation::Size WriteSessionFrame(Foundation::UInt8* frame, Foundation::UInt32 method,
                                                 Foundation::UInt64 transaction, const Foundation::UInt64* words,
                                                 const System::CapabilityHandle* attachments,
                                                 Foundation::Size attachmentCount) noexcept;

/// Decodes a kernel delivery (its 48-byte header, then the frame of
/// deliveryBytes - 48 bytes): the canonical header, the attachment records
/// (selector, infinite deadline, the method's rights, copy, type) and the
/// method's words. message is written only on success.
[[nodiscard]] bool ReadSessionFrame(const Foundation::UInt8* delivery, Foundation::Size deliveryBytes,
                                    SessionMessage& message) noexcept;

/// Writes an Events frame of count (1-7) events with its header's flags
/// (SessionEventsMorePending) and dropped-entry count. Zero, writing
/// nothing, for an invalid event, count, flag or epoch.
[[nodiscard]] Foundation::Size WriteSessionEvents(Foundation::UInt8* frame, Foundation::UInt64 transaction,
                                                  Foundation::UInt64 epoch, const SessionEvent* events,
                                                  Foundation::Size count, Foundation::UInt64 flags,
                                                  Foundation::UInt64 dropped) noexcept;

/// The number of events of a decoded Events message (zero for another).
[[nodiscard]] Foundation::Size SessionEventCount(const SessionMessage& message) noexcept;

/// Event index of a decoded Events message; false for another message or index.
[[nodiscard]] bool GetSessionEvent(const SessionMessage& message, Foundation::Size index, SessionEvent& event) noexcept;

/// A damage rectangle's word: x, y, width, height as 16-bit fields from bit 0.
[[nodiscard]] constexpr Foundation::UInt64 PackSessionDamage(const Foundation::UInt32 x, const Foundation::UInt32 y,
                                                             const Foundation::UInt32 width,
                                                             const Foundation::UInt32 height) noexcept
{
    return (static_cast<Foundation::UInt64>(x & 0xFFFFU)) | (static_cast<Foundation::UInt64>(y & 0xFFFFU) << 16U) |
           (static_cast<Foundation::UInt64>(width & 0xFFFFU) << 32U) |
           (static_cast<Foundation::UInt64>(height & 0xFFFFU) << 48U);
}

[[nodiscard]] constexpr Graphics::Rect UnpackSessionDamage(const Foundation::UInt64 word) noexcept
{
    return Graphics::Rect{.X = static_cast<Foundation::Int32>(word & 0xFFFFU),
                          .Y = static_cast<Foundation::Int32>((word >> 16U) & 0xFFFFU),
                          .Width = static_cast<Foundation::UInt32>((word >> 32U) & 0xFFFFU),
                          .Height = static_cast<Foundation::UInt32>(word >> 48U)};
}

/// A pair of 32-bit fields (width and height, or x and y) in one word.
[[nodiscard]] constexpr Foundation::UInt64 PackSessionPair(const Foundation::UInt32 low,
                                                           const Foundation::UInt32 high) noexcept
{
    return static_cast<Foundation::UInt64>(low) | (static_cast<Foundation::UInt64>(high) << 32U);
}

/// A signed position pair (surface-local pointer coordinates may be negative
/// under an implicit grab).
[[nodiscard]] constexpr Foundation::UInt64 PackSessionPosition(const Foundation::Int32 x,
                                                               const Foundation::Int32 y) noexcept
{
    return PackSessionPair(static_cast<Foundation::UInt32>(x), static_cast<Foundation::UInt32>(y));
}

/// A CreatePool's page count of Frame index (0-15), from its words.
[[nodiscard]] constexpr Foundation::UInt32 SessionPoolFramePages(const Foundation::UInt64* const words,
                                                                 const Foundation::Size index) noexcept
{
    if ((words == nullptr) || (index >= SessionMaximumPoolFrames))
    {
        return 0U;
    }
    return static_cast<Foundation::UInt32>((words[4U + (index >> 2U)] >> ((index & 3U) * 16U)) & 0xFFFFU);
}

/// Byte index of a text held in words from firstWord on (SetSubtitle's from
/// word 2, RegisterClient's from word 4).
[[nodiscard]] constexpr Foundation::UInt8 SessionTextByte(const Foundation::UInt64* const words,
                                                          const Foundation::Size firstWord,
                                                          const Foundation::Size index) noexcept
{
    return static_cast<Foundation::UInt8>(words[firstWord + (index >> 3U)] >> ((index & 7U) * 8U));
}

/// Whether an image of width x height pixels with stride pixels per row at
/// offset bytes lies inside a pool of poolBytes: offset + stride x height x 4
/// <= poolBytes, all in 64 bits.
[[nodiscard]] constexpr bool SessionImageFits(const Foundation::UInt64 offset, const Foundation::UInt32 width,
                                              const Foundation::UInt32 height, const Foundation::UInt32 stride,
                                              const Foundation::UInt64 poolBytes) noexcept
{
    if ((width == 0U) || (height == 0U) || (stride < width) || (stride > SessionMaximumExtent) ||
        (height > SessionMaximumExtent) || (offset > poolBytes))
    {
        return false;
    }
    const Foundation::UInt64 bytes = static_cast<Foundation::UInt64>(stride) * height * 4U;
    return bytes <= (poolBytes - offset);
}
} // Namespace Asteria::User::Session

#endif // ASTERIA_USER_SESSION_SESSION_FRAME_HPP_INCLUDED
