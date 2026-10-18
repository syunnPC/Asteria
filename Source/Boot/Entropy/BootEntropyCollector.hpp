// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_BOOT_ENTROPY_BOOT_ENTROPY_COLLECTOR_HPP_INCLUDED
#define ASTERIA_BOOT_ENTROPY_BOOT_ENTROPY_COLLECTOR_HPP_INCLUDED

#include "Boot/BootEntropy.hpp"
#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Security/Sha256.hpp"

namespace Asteria::Boot::Entropy
{
enum class BootEntropyError : Foundation::UInt32
{
    None,
    /// No credited source passed its health tests.
    NoHealthySource,
    /// Finish already ended this collection.
    AlreadyFinished,
};

/// A generator source is credited once it delivered this many bytes, every
/// block passing the generator health test.
inline constexpr Foundation::Size MinimumGeneratorBytes{32U};
/// Shorter generator blocks fail the generator health test.
inline constexpr Foundation::Size MinimumGeneratorBlockBytes{8U};
/// The jitter source is credited only after this many cycle-counter deltas.
inline constexpr Foundation::Size MinimumCycleCounterDeltas{1024U};
/// NIST SP 800-90B 4.4.1 repetition count test over the deltas: this many
/// equal consecutive deltas fail the jitter source.
inline constexpr Foundation::Size RepetitionCountCutoff{32U};
/// NIST SP 800-90B 4.4.2 adaptive proportion test over the deltas: within a
/// window, this many deltas equal to the window's first delta fail the source.
inline constexpr Foundation::Size AdaptiveProportionWindow{512U};
inline constexpr Foundation::Size AdaptiveProportionCutoff{384U};

/// Pools the boot entropy samples of one boot and extracts the handoff seed. Every sample is mixed,
/// framed as its source bit (one byte), its length (four bytes, little-endian) and its bytes, into
/// HKDF-SHA256 Extract (RFC 5869) with the salt "Asteria boot entropy v1"; the seed is the
/// extracted pseudorandom key. Health tests decide only which sources are credited: a generator
/// (FirmwareRng, ProcessorRng) fails when a block is shorter than MinimumGeneratorBlockBytes, has
/// all bytes equal, or begins with the same eight bytes as the source's previous block; the
/// cycle-counter jitter source fails on a zero delta or on the repetition count or adaptive
/// proportion test. Firmware state is mixed but never credited. Execution: - Single-threaded; no
/// allocation, no firmware call; every call is bounded by its input size. - One collection per
/// object: Finish ends it.
class BootEntropyCollector final
{
public:
    BootEntropyCollector() noexcept;

    BootEntropyCollector(const BootEntropyCollector&) = delete;
    BootEntropyCollector& operator=(const BootEntropyCollector&) = delete;

    /// Mixes one block of generator output. A source other than FirmwareRng
    /// or ProcessorRng is mixed but credits nothing. bytes may be null only
    /// when sizeBytes is zero.
    void AddGeneratorBlock(BootEntropySource source, const Foundation::UInt8* bytes,
                           Foundation::Size sizeBytes) noexcept;

    /// Mixes one reading of the free-running cycle counter; the jitter
    /// health tests run over the differences of consecutive readings.
    void AddCycleCounterSample(Foundation::UInt64 counter) noexcept;

    /// Mixes firmware-varying state, which is never credited. bytes may be
    /// null only when sizeBytes is zero.
    void AddFirmwareState(const Foundation::UInt8* bytes, Foundation::Size sizeBytes) noexcept;

    /// Extracts the seed. Fails with NoHealthySource unless a generator
    /// source delivered MinimumGeneratorBytes healthy bytes or the jitter
    /// source passed its tests over MinimumCycleCounterDeltas deltas. The
    /// pooled state is consumed either way: a later Finish fails with
    /// AlreadyFinished, so later input reaches no seed.
    [[nodiscard]] Foundation::Result<BootEntropy, BootEntropyError> Finish() noexcept;

private:
    struct GeneratorHealth
    {
        Foundation::UInt64 HealthyBytes;
        Foundation::UInt64 PreviousPrefix;
        bool HasPrevious;
        bool Failed;
    };

    struct JitterHealth
    {
        Foundation::UInt64 PreviousCounter;
        Foundation::UInt64 PreviousDelta;
        Foundation::UInt64 WindowReference;
        Foundation::Size Deltas;
        Foundation::Size RunLength;
        Foundation::Size WindowPosition;
        Foundation::Size WindowMatches;
        bool HasCounter;
        bool Failed;
    };

    void Mix(BootEntropySource source, const Foundation::UInt8* bytes, Foundation::Size sizeBytes) noexcept;
    void CheckDelta(Foundation::UInt64 delta) noexcept;
    [[nodiscard]] Foundation::UInt32 GetCreditedSources() const noexcept;

    Kernel::Security::HmacSha256 m_extract;
    GeneratorHealth m_firmwareRng{};
    GeneratorHealth m_processorRng{};
    JitterHealth m_jitter{};
    bool m_firmwareState{false};
    bool m_finished{false};
};
} // Namespace Asteria::Boot::Entropy

#endif // ASTERIA_BOOT_ENTROPY_BOOT_ENTROPY_COLLECTOR_HPP_INCLUDED
