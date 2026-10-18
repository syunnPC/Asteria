// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_MEMORY_IA64_RUNTIME_TRANSLATION_CACHE_INSERTER_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_MEMORY_IA64_RUNTIME_TRANSLATION_CACHE_INSERTER_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Platform/Ia64/Memory/Ia64RuntimeTranslationEncoder.hpp"

namespace Asteria::Platform::Ia64::Memory
{
enum class Ia64RuntimeTranslationCacheInsertionKind : Foundation::UInt8
{
    Data,
    Instruction,
};

enum class Ia64RuntimeTranslationCacheInsertionError : Foundation::UInt8
{
    None,
    InvalidInsertionKind,
    InvalidTranslationInsertion,
    InvalidFaultingAddress,
    InvalidTranslationWord,
};

/// Inserts one already-encoded 4 KiB runtime translation into the calling processor's data or
/// instruction translation cache. Preconditions not representable by the value arguments: -
/// execution is at CPL0 with PSR.vm and PSR.ic both clear; - local interrupts, preemption,
/// migration, and processor-local re-entry are excluded; - the correct region register and RID are
/// active for FaultingAddress; - this code, its data, stack, RSE backing store, and all operands
/// are statically pinned and shared so the commit path needs no unrelated memory reference before
/// the interrupted instruction is restarted. Every operand is validated before the privileged
/// helper is called. On success the selected helper has set IFA and ITIR, executed exactly one ITC
/// insertion, and completed its matching data or instruction serialization. This operation does not
/// clear PSR.ic, switch region registers, allocate, lock, publish state, purge translations, or
/// invoke firmware or callbacks.
class Ia64RuntimeTranslationCacheInserter final
{
public:
    [[nodiscard]] static Ia64RuntimeTranslationCacheInsertionError
    Insert(const EncodedIa64RuntimeTranslation& translation, Ia64RuntimeTranslationCacheInsertionKind kind) noexcept;

private:
    Ia64RuntimeTranslationCacheInserter() = delete;
};

static_assert(sizeof(Ia64RuntimeTranslationCacheInsertionKind) == 1U);
static_assert(sizeof(Ia64RuntimeTranslationCacheInsertionError) == 1U);
} // Namespace Asteria::Platform::Ia64::Memory

#endif // ASTERIA_PLATFORM_IA64_MEMORY_IA64_RUNTIME_TRANSLATION_CACHE_INSERTER_HPP_INCLUDED
