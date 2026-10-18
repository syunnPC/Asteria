// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_USB_MASSSTORAGE_HPP_INCLUDED
#define ASTERIA_USER_USB_MASSSTORAGE_HPP_INCLUDED

#include "User/Usb/Scsi.hpp"

namespace Asteria::User::Usb
{
struct BlockDescription
{
    UInt32 BlockSize{0U};
    UInt64 BlockCount{0U};
    bool Removable{false};
    bool ReadOnly{false};
    bool MediumPresent{false};
    char Vendor[9U]{};
    char Product[17U]{};
};
enum class StorageStatus : UInt8
{
    Ok,
    Unsupported,
    NotReady,
    NoMedium,
    ReadOnly,
    MediumChanged,
    IoError,
    TransportError,
    Cancelled,
};
struct StorageResult
{
    StorageStatus Status{StorageStatus::IoError};
    UInt64 Generation{0U};
    UInt32 Blocks{0U};
    BlockDescription Description{};
};
/// Serialized LUN-zero block facade. One operation and retained result; no
/// allocation, callbacks, hardware access or blocking. Begin refusals mutate
/// nothing. Describe publishes its candidate only on success. Read/Write are
/// one READ/WRITE(10), <=min(caller limit,64KiB); larger ranges are refused.
/// Results report zero blocks on failure, never partial success. A failed
/// write can have reached media; a failed read can have changed Frame bytes.
/// For unchanged-on-failure reads, the component supplies a private staging
/// Frame and copies it to the client's Frame only on Ok. Keep owner off-stack.
/// Every operation has <=4 readiness attempts; becoming-ready retries are
/// separated by >=100ms of caller monotonic time. A component policy deadline
/// uses Cancel, retaining all buffers until the original completion/recovery.
/// Step also enforces the configured operation budget (1..60000ms, default
/// 5000ms) from its first call. Three refused deadline Aborts fault this owner
/// with its live borrow retained; the component must then quiesce the adapter.
/// Accepted Abort/control/recovery operations must have finite adapter bounds.
class MassStorage final
{
public:
    explicit constexpr MassStorage(BotBinding binding = {}, UInt32 operationMilliseconds = 5000U) noexcept
        : m_bot{binding}, m_operationMilliseconds{operationMilliseconds}
    {
    }
    MassStorage(const MassStorage&) = delete;
    MassStorage& operator=(const MassStorage&) = delete;
    [[nodiscard]] bool Describe() noexcept;
    [[nodiscard]] bool Read(UInt64 generation, UInt64 lba, UInt32 blocks, FrameBuffer buffer,
                            UInt32 maximumBytes) noexcept;
    [[nodiscard]] bool Write(UInt64 generation, UInt64 lba, UInt32 blocks, FrameBuffer buffer,
                             UInt32 maximumBytes) noexcept;
    [[nodiscard]] bool Flush(UInt64 generation) noexcept;
    void Step(IMassStoragePipe& pipe, UInt64 milliseconds) noexcept;
    [[nodiscard]] bool Complete(const PipeCompletion& completion) noexcept;
    [[nodiscard]] bool Cancel(IMassStoragePipe& pipe) noexcept;
    [[nodiscard]] bool TakeResult(StorageResult& output) noexcept;
    [[nodiscard]] UInt64 Generation() const noexcept
    {
        return m_generation;
    }
    [[nodiscard]] bool IsOutstanding() const noexcept
    {
        return m_bot.IsOutstanding();
    }
    [[nodiscard]] bool HasDescription() const noexcept
    {
        return m_valid;
    }
    [[nodiscard]] bool IsFaulted() const noexcept
    {
        return m_bot.IsFaulted() || m_exhausted || m_deadlineFault || (m_operationMilliseconds == 0U) ||
               (m_operationMilliseconds > 60000U);
    }

private:
    enum class Stage : UInt8
    {
        Idle,
        Lun,
        Inquiry,
        Ready,
        Capacity10,
        Capacity16,
        Mode6,
        Mode10,
        Io,
        Flush,
        Sense,
        Done
    };
    [[nodiscard]] bool Range(bool write, UInt64 generation, UInt64 lba, UInt32 blocks, FrameBuffer buffer,
                             UInt32 maximumBytes) noexcept;
    void Start(Stage stage) noexcept;
    void Advance() noexcept;
    void SenseResult() noexcept;
    void Finish(StorageStatus status) noexcept;
    void Invalidate() noexcept;
    Bot m_bot;
    ScsiCommand m_command{};
    BotResult m_botResult{};
    StorageResult m_result{};
    BlockDescription m_description{};
    BlockDescription m_candidate{};
    UInt64 m_generation{1U};
    UInt64 m_now{0U};
    UInt64 m_retryAt{0U};
    UInt32 m_blocks{0U};
    UInt32 m_operationMilliseconds{5000U};
    UInt64 m_operationAt{0U};
    UInt8 m_abortRefusals{0U};
    bool m_timing{false};
    bool m_cancelRequested{false};
    bool m_deadlineFault{false};
    Stage m_stage{Stage::Idle};
    Stage m_failedStage{Stage::Idle};
    UInt8 m_retries{0U};
    bool m_started{false};
    bool m_valid{false};
    bool m_exhausted{false};
};
}

#endif
