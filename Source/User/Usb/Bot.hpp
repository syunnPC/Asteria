// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_USB_BOT_HPP_INCLUDED
#define ASTERIA_USER_USB_BOT_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::User::Usb
{
using Foundation::Size;
using Foundation::UInt8;
using Foundation::UInt16;
using Foundation::UInt32;
using Foundation::UInt64;

struct FrameHandle
{
    UInt64 Selector{0U};
    UInt64 Authenticator{0U};
};
struct FrameBuffer
{
    FrameHandle Handle{};
    UInt64 Offset{0U};
    UInt32 Length{0U};
};
enum class PipeStatus : UInt8
{
    Ok,
    Short,
    Stall,
    Error,
    Cancelled
};
enum class PipeOperation : UInt8
{
    BulkIn,
    BulkOut,
    ClassControl,
    ClearHalt
};
struct PipeRequest
{
    PipeOperation Operation{};
    UInt16 Pipe{0U};
    UInt64 Token{0U};
    FrameBuffer Buffer{};
    UInt32 Length{0U};
    bool UseFrame{false};
    UInt8 Setup[8U]{};
    UInt8 Inline[64U]{};
};
struct PipeCompletion
{
    UInt64 Token{0U};
    PipeStatus Status{PipeStatus::Error};
    UInt32 ActualLength{0U};
    // IN inline data only; borrowed during Complete. Frame data never appears here.
    const UInt8* Inline{nullptr};
    Size InlineLength{0U};
};
struct BotBinding
{
    UInt16 ControlPipe{0U};
    UInt16 InPipe{1U};
    UInt16 OutPipe{2U};
    UInt8 Interface{0U};
};
/// Serialized nonblocking seam. Submit copies metadata/inline bytes; false
/// retains nothing. Accepted work completes exactly once, after DMA/unmapping
/// is quiet. ClearHalt resets device halt AND host toggle/dequeue. Abort names
/// the original token and yields its one completion (a racing success is legal).
/// No synchronous completion or callback/reentry. Handle bounds/rights are
/// checked by the adapter; never expose a device address to this library.
class IMassStoragePipe
{
public:
    virtual ~IMassStoragePipe() = default;
    [[nodiscard]] virtual bool Submit(const PipeRequest& request) noexcept = 0;
    [[nodiscard]] virtual bool Abort(UInt16 pipe, UInt64 token) noexcept = 0;
};
struct ScsiCommand
{
    UInt8 Bytes[16U]{};
    UInt8 Length{0U};
    bool In{true};
    UInt32 TransferLength{0U};
    bool UseFrame{false};
    FrameBuffer Buffer{};
    UInt8 Inline[64U]{};
};
struct Csw
{
    UInt32 Residue{0U};
    UInt8 Status{0U};
};
class BotCodec final
{
public:
    [[nodiscard]] static bool Cbw(const ScsiCommand& command, UInt32 tag, UInt8* bytes, Size length) noexcept;
    [[nodiscard]] static bool DecodeCsw(const UInt8* bytes, Size length, UInt32 tag, UInt32 transferLength,
                                        Csw& output) noexcept;
};
enum class BotStatus : UInt8
{
    Passed,
    CommandFailed,
    TransportError,
    Cancelled
};
struct BotResult
{
    BotStatus Status{BotStatus::TransportError};
    UInt32 RelevantLength{0U};
    bool StateLost{false};
    UInt8 MaxLun{0U};
    UInt8 Inline[64U]{};
};
/// One command/result at a time, LUN zero only, no allocation or waiting.
/// Step submits <=1 request; three submission refusals exhaust the operation.
/// Owner and its scratch live in component storage. Accepted transfers may NAK
/// indefinitely: the component imposes a policy deadline using Cancel; borrows
/// remain live until completion. Successful reset recovery invalidates state.
class Bot final
{
public:
    explicit constexpr Bot(BotBinding binding = {}) noexcept : m_binding{binding}
    {
    }
    Bot(const Bot&) = delete;
    Bot& operator=(const Bot&) = delete;
    [[nodiscard]] bool GetMaxLun() noexcept;
    [[nodiscard]] bool Begin(const ScsiCommand& command) noexcept;
    void Step(IMassStoragePipe& pipe) noexcept;
    [[nodiscard]] bool Complete(const PipeCompletion& completion) noexcept;
    [[nodiscard]] bool Cancel(IMassStoragePipe& pipe) noexcept;
    [[nodiscard]] bool TakeResult(BotResult& output) noexcept;
    [[nodiscard]] bool IsOutstanding() const noexcept
    {
        return m_outstanding;
    }
    [[nodiscard]] bool IsFaulted() const noexcept
    {
        return m_faulted;
    }

private:
    enum class Phase : UInt8
    {
        Idle,
        Lun,
        Cbw,
        Data,
        Csw,
        ClearData,
        ClearCsw,
        Reset,
        ClearIn,
        ClearOut,
        Done
    };
    void Recover(BotStatus status) noexcept;
    void Finish(BotStatus status) noexcept;
    void Build() noexcept;
    BotBinding m_binding{};
    ScsiCommand m_command{};
    PipeRequest m_request{};
    BotResult m_result{};
    UInt64 m_token{0U};
    UInt32 m_tag{0U};
    UInt32 m_actual{0U};
    Phase m_phase{Phase::Idle};
    UInt8 m_refusals{0U};
    bool m_outstanding{false};
    bool m_retryCsw{false};
    bool m_cancelled{false};
    bool m_faulted{false};
};
}

#endif
