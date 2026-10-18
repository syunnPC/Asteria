// SPDX-License-Identifier: Apache-2.0

#include "Boot/Entropy/BootEntropyCollector.hpp"

namespace Asteria::Boot::Entropy
{
namespace
{
using Foundation::Size;
using Foundation::UInt32;
using Foundation::UInt64;
using Foundation::UInt8;

// HKDF-Extract salt: the ASCII text "Asteria boot entropy v1".
constexpr UInt8 ExtractSalt[]{
    'A', 's', 't', 'e', 'r', 'i', 'a', ' ', 'b', 'o', 'o', 't', ' ', 'e', 'n', 't', 'r', 'o', 'p', 'y', ' ', 'v', '1',
};
constexpr Size FrameHeaderBytes{9U};

static_assert(BootEntropySeedBytes == Kernel::Security::Sha256DigestBytes);

[[nodiscard]] UInt64 LoadLittleEndian64(const UInt8* const bytes) noexcept
{
    UInt64 value{0U};
    for (Size index{0U}; index < 8U; ++index)
    {
        value |= static_cast<UInt64>(bytes[index]) << (index * 8U);
    }
    return value;
}

void StoreLittleEndian64(UInt8* const bytes, const UInt64 value) noexcept
{
    for (Size index{0U}; index < 8U; ++index)
    {
        bytes[index] = static_cast<UInt8>(value >> (index * 8U));
    }
}

[[nodiscard]] bool AllBytesEqual(const UInt8* const bytes, const Size sizeBytes) noexcept
{
    for (Size index{1U}; index < sizeBytes; ++index)
    {
        if (bytes[index] != bytes[0U])
        {
            return false;
        }
    }
    return true;
}
} // namespace

BootEntropyCollector::BootEntropyCollector() noexcept : m_extract{ExtractSalt, sizeof(ExtractSalt)}
{
}

void BootEntropyCollector::Mix(const BootEntropySource source, const UInt8* const bytes, const Size sizeBytes) noexcept
{
    UInt8 header[FrameHeaderBytes]{};
    header[0U] = static_cast<UInt8>(GetBootEntropySourceMask(source));
    StoreLittleEndian64(header + 1U, static_cast<UInt64>(sizeBytes));
    m_extract.Update(header, sizeof(header));
    m_extract.Update(bytes, sizeBytes);
}

void BootEntropyCollector::AddGeneratorBlock(const BootEntropySource source, const UInt8* const bytes,
                                             const Size sizeBytes) noexcept
{
    Mix(source, bytes, sizeBytes);
    GeneratorHealth* health{nullptr};
    switch (source)
    {
    case BootEntropySource::FirmwareRng:
        health = &m_firmwareRng;
        break;
    case BootEntropySource::ProcessorRng:
        health = &m_processorRng;
        break;
    case BootEntropySource::CycleCounterJitter:
    case BootEntropySource::FirmwareState:
        break;
    }
    if ((health == nullptr) || health->Failed)
    {
        return;
    }
    if ((sizeBytes < MinimumGeneratorBlockBytes) || AllBytesEqual(bytes, sizeBytes))
    {
        health->Failed = true;
        return;
    }
    // A continuous test on each block's first 64 bits: a generator that
    // repeats its previous output is stuck.
    const UInt64 prefix = LoadLittleEndian64(bytes);
    if (health->HasPrevious && (prefix == health->PreviousPrefix))
    {
        health->Failed = true;
        return;
    }
    health->PreviousPrefix = prefix;
    health->HasPrevious = true;
    const UInt64 needed = MinimumGeneratorBytes - health->HealthyBytes;
    health->HealthyBytes = static_cast<UInt64>(sizeBytes) >= needed
                               ? MinimumGeneratorBytes
                               : health->HealthyBytes + static_cast<UInt64>(sizeBytes);
}

void BootEntropyCollector::AddCycleCounterSample(const UInt64 counter) noexcept
{
    UInt8 bytes[8U]{};
    StoreLittleEndian64(bytes, counter);
    Mix(BootEntropySource::CycleCounterJitter, bytes, sizeof(bytes));
    if (!m_jitter.HasCounter)
    {
        m_jitter.PreviousCounter = counter;
        m_jitter.HasCounter = true;
        return;
    }
    // Modulo 2^64, so a counter that wraps still yields its true delta.
    const UInt64 delta = counter - m_jitter.PreviousCounter;
    m_jitter.PreviousCounter = counter;
    CheckDelta(delta);
}

void BootEntropyCollector::CheckDelta(const UInt64 delta) noexcept
{
    if (m_jitter.Failed)
    {
        return;
    }
    if (delta == 0U)
    {
        m_jitter.Failed = true;
        return;
    }
    // PreviousDelta starts as zero, which no accepted delta equals.
    m_jitter.RunLength = delta == m_jitter.PreviousDelta ? m_jitter.RunLength + 1U : 1U;
    m_jitter.PreviousDelta = delta;
    if (m_jitter.RunLength >= RepetitionCountCutoff)
    {
        m_jitter.Failed = true;
        return;
    }
    if (m_jitter.WindowPosition == 0U)
    {
        m_jitter.WindowReference = delta;
        m_jitter.WindowMatches = 1U;
    }
    else if (delta == m_jitter.WindowReference)
    {
        ++m_jitter.WindowMatches;
        if (m_jitter.WindowMatches >= AdaptiveProportionCutoff)
        {
            m_jitter.Failed = true;
            return;
        }
    }
    ++m_jitter.WindowPosition;
    if (m_jitter.WindowPosition == AdaptiveProportionWindow)
    {
        m_jitter.WindowPosition = 0U;
    }
    // One per reading: bounded by the caller's reading count.
    ++m_jitter.Deltas;
}

void BootEntropyCollector::AddFirmwareState(const UInt8* const bytes, const Size sizeBytes) noexcept
{
    Mix(BootEntropySource::FirmwareState, bytes, sizeBytes);
    m_firmwareState = true;
}

UInt32 BootEntropyCollector::GetCreditedSources() const noexcept
{
    UInt32 sources{0U};
    if (!m_firmwareRng.Failed && (m_firmwareRng.HealthyBytes >= MinimumGeneratorBytes))
    {
        sources |= GetBootEntropySourceMask(BootEntropySource::FirmwareRng);
    }
    if (!m_processorRng.Failed && (m_processorRng.HealthyBytes >= MinimumGeneratorBytes))
    {
        sources |= GetBootEntropySourceMask(BootEntropySource::ProcessorRng);
    }
    if (!m_jitter.Failed && (m_jitter.Deltas >= MinimumCycleCounterDeltas))
    {
        sources |= GetBootEntropySourceMask(BootEntropySource::CycleCounterJitter);
    }
    return sources;
}

Foundation::Result<BootEntropy, BootEntropyError> BootEntropyCollector::Finish() noexcept
{
    using ResultType = Foundation::Result<BootEntropy, BootEntropyError>;
    if (m_finished)
    {
        return ResultType::Failure(BootEntropyError::AlreadyFinished);
    }
    m_finished = true;
    const UInt32 credited = GetCreditedSources();
    BootEntropy entropy{};
    m_extract.Finish(entropy.Seed);
    if (credited == 0U)
    {
        return ResultType::Failure(BootEntropyError::NoHealthySource);
    }
    entropy.Sources = credited | (m_firmwareState ? GetBootEntropySourceMask(BootEntropySource::FirmwareState) : 0U);
    entropy.Reserved = 0U;
    return ResultType::Success(entropy);
}
} // namespace Asteria::Boot::Entropy
