// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Memory/PhysicalPageAllocator.hpp"

namespace Asteria::Kernel::Memory
{
using Foundation::Size;
using Foundation::UInt64;
using Foundation::UInt16;
using Error = PhysicalPageAllocatorError;
using RunResult = Foundation::Result<PhysicalPageRun, Error>;
namespace
{
constexpr UInt64 LowEnd{(UInt64{1U} << 32U) / MappingPageSizeBytes};
[[nodiscard]] constexpr UInt64 CountBits(UInt64 bits) noexcept
{
    bits -= (bits >> 1U) & 0x5555'5555'5555'5555ULL;
    bits = (bits & 0x3333'3333'3333'3333ULL) + ((bits >> 2U) & 0x3333'3333'3333'3333ULL);
    bits = (bits + (bits >> 4U)) & 0x0F0F'0F0F'0F0F'0F0FULL;
    return (bits * 0x0101'0101'0101'0101ULL) >> 56U;
}
[[nodiscard]] Size Extreme(const UInt64 bits, const bool reverse) noexcept
{
    return static_cast<Size>(reverse ? 63U - static_cast<unsigned int>(__builtin_clzll(bits))
                                     : static_cast<unsigned int>(__builtin_ctzll(bits)));
}
[[nodiscard]] UInt64 DirtyCount(const PhysicalPageRun& run) noexcept
{
    UInt64 result{0U};
    for (const UInt64 bits : run.DirtyMask)
    {
        result += CountBits(bits);
    }
    return result;
}
} // Namespace
bool PhysicalPageAllocator::Bit(const UInt64* const bits, const Size page) noexcept
{
    return (bits[page / 64U] & (UInt64{1U} << (page % 64U))) != 0U;
}
void PhysicalPageAllocator::Set(UInt64* const bits, const Size page, const bool value) noexcept
{
    const UInt64 mask{UInt64{1U} << (page % 64U)};
    bits[page / 64U] = value ? bits[page / 64U] | mask : bits[page / 64U] & ~mask;
}
bool PhysicalPageAllocator::BitTree::Get(const Size bit) const noexcept
{
    return Bit(Data, bit);
}
void PhysicalPageAllocator::BitTree::Set(Size bit, bool value) noexcept
{
    Size words{(Bits + 63U) / 64U};
    Size offset{0U};
    for (Size level{0U}; level < MaximumLevels; ++level)
    {
        PhysicalPageAllocator::Set(Data + offset, bit, value);
        const Size word{bit / 64U};
        value = Data[offset + word] != 0U;
        if (words == 1U)
        {
            break;
        }
        offset += words;
        bit = word;
        words = (words + 63U) / 64U;
    }
}
Size PhysicalPageAllocator::BitTree::Find(Size first, const bool reverse) const noexcept
{
    if (first >= Bits)
    {
        return Bits;
    }
    Size offsets[MaximumLevels]{};
    Size lengths[MaximumLevels]{};
    lengths[0U] = Bits;
    Size levels{1U};
    for (; levels < MaximumLevels; ++levels)
    {
        const Size words{(lengths[levels - 1U] + 63U) / 64U};
        if (words == 1U)
        {
            break;
        }
        offsets[levels] = offsets[levels - 1U] + words;
        lengths[levels] = words;
    }
    for (Size level{0U}; level < levels; ++level)
    {
        if (first >= lengths[level])
        {
            return Bits;
        }
        const Size word{first / 64U};
        const Size shift{first % 64U};
        const UInt64 mask{reverse ? (~UInt64{0U} >> (63U - shift)) : (~UInt64{0U} << shift)};
        const UInt64 bits{Data[offsets[level] + word] & mask};
        if (bits != 0U)
        {
            Size selected{word * 64U + Extreme(bits, reverse)};
            for (Size down{level}; down != 0U; --down)
            {
                selected = selected * 64U + Extreme(Data[offsets[down - 1U] + selected], reverse);
            }
            return selected < Bits ? selected : Bits;
        }
        if (reverse && word == 0U)
        {
            return Bits;
        }
        first = reverse ? word - 1U : word + 1U;
    }
    return Bits;
}
void PhysicalPageAllocator::MaxTree::Set(Size leaf, const UInt16 value) noexcept
{
    Data[leaf] = value;
    Size count{Leaves};
    Size offset{0U};
    for (Size level{0U}; count > 1U && level < MaximumLevels; ++level)
    {
        const Size group{leaf / 64U};
        UInt16 maximum{0U};
        for (Size index{group * 64U}; index < count && index < (group + 1U) * 64U; ++index)
        {
            if (Data[offset + index] > maximum)
            {
                maximum = Data[offset + index];
            }
        }
        offset += count;
        count = (count + 63U) / 64U;
        leaf = group;
        Data[offset + leaf] = maximum;
    }
}
Size PhysicalPageAllocator::MaxTree::Find(const UInt16 minimum, const bool reverse) const noexcept
{
    Size offsets[MaximumLevels]{};
    Size counts[MaximumLevels]{};
    counts[0U] = Leaves;
    Size levels{1U};
    for (; counts[levels - 1U] > 1U && levels < MaximumLevels; ++levels)
    {
        offsets[levels] = offsets[levels - 1U] + counts[levels - 1U];
        counts[levels] = (counts[levels - 1U] + 63U) / 64U;
    }
    if (Data[offsets[levels - 1U]] < minimum)
    {
        return Leaves;
    }
    Size selected{0U};
    for (Size level{levels - 1U}; level != 0U; --level)
    {
        const Size begin{selected * 64U};
        const Size end{(begin + 64U < counts[level - 1U]) ? begin + 64U : counts[level - 1U]};
        for (Size step{0U}; step < end - begin; ++step)
        {
            const Size candidate{reverse ? end - 1U - step : begin + step};
            if (Data[offsets[level - 1U] + candidate] >= minimum)
            {
                selected = candidate;
                break;
            }
        }
    }
    return selected;
}
void PhysicalPageAllocator::Prepare(Segment& segment, const UInt64 base, const Size pages,
                                    Foundation::UInt8*& memory) noexcept
{
    segment.Base = base;
    segment.Pages = pages;
    const Size words{(pages + 63U) / 64U};
    UInt64** const maps[]{&segment.Managed,     &segment.Dirty,     &segment.Allocated, &segment.Claimed,
                          &segment.ClaimStarts, &segment.ClaimEnds, &segment.Retired,   &segment.CollectionDirty};
    static_assert(sizeof(maps) / sizeof(maps[0U]) == StateBitmaps);
    for (auto* const map : maps)
    {
        *map = static_cast<UInt64*>(static_cast<void*>(memory));
        memory += words * sizeof(UInt64);
    }
    for (Size order{0U}; order < Orders; ++order)
    {
        const Size bits{pages >> order};
        segment.Free[order] = {static_cast<UInt64*>(static_cast<void*>(memory)), bits};
        memory += TreeWords((bits + 63U) / 64U) * sizeof(UInt64);
    }
    const Size chunks{pages / MaximumRunPages};
    segment.DirtyChunks = {static_cast<UInt64*>(static_cast<void*>(memory)), chunks};
    memory += TreeWords((chunks + 63U) / 64U) * sizeof(UInt64);
    segment.CollectionChunks = {static_cast<UInt64*>(static_cast<void*>(memory)), chunks};
    memory += TreeWords((chunks + 63U) / 64U) * sizeof(UInt64);
    for (auto& tree : segment.Clean)
    {
        tree = {static_cast<UInt16*>(static_cast<void*>(memory)), chunks};
        memory += (TreeWords(chunks) * sizeof(UInt16) + 7U) & ~Size{7U};
    }
}
void PhysicalPageAllocator::SetFree(Segment& segment, const Size order, const Size block, const bool free) noexcept
{
    if (segment.Free[order].Get(block) == free)
    {
        return;
    }
    segment.Free[order].Set(block, free);
    segment.Counts[order] = free ? segment.Counts[order] + 1U : segment.Counts[order] - 1U;
}
void PhysicalPageAllocator::FreeRange(Segment& segment, Size first, Size pages) noexcept
{
    // Import divides large ranges into order-9 chunks; runtime runs <=512.
    for (Size step{0U}; pages != 0U && step < 2U * Orders; ++step)
    {
        Size order{0U};
        while (order < MaximumOrder && (Size{1U} << (order + 1U)) <= pages &&
               (first & ((Size{1U} << (order + 1U)) - 1U)) == 0U)
        {
            ++order;
        }
        const Size length{Size{1U} << order};
        Size block{first >> order};
        while (order < MaximumOrder && segment.Free[order].Get(block ^ 1U))
        {
            SetFree(segment, order, block ^ 1U, false);
            block >>= 1U;
            ++order;
        }
        SetFree(segment, order, block, true);
        first += length;
        pages -= length;
    }
}
void PhysicalPageAllocator::Rebuild(Segment& segment, const Size chunk, const bool cached) noexcept
{
    const Size base{chunk * MaximumRunPages};
    for (Size word{0U}; !cached && word < DirtyWords; ++word)
    {
        m_chunkDirty[word] = segment.Dirty[base / 64U + word];
    }
    bool dirtyFree{false};
    bool collectionFree{false};
    // Clean prefixes, for every aligned descendant k of a free buddy j.
    // The max trees skip arbitrarily long mixed/dirty spans in O(log64 n).
    for (Size k{0U}; k < Orders; ++k)
    {
        UInt16 prefix{0U};
        for (Size step{0U}; step < MaximumRunPages; ++step)
        {
            const Size page{MaximumRunPages - 1U - step};
            prefix = Bit(m_chunkDirty, page) ? 0U : static_cast<UInt16>(prefix + 1U);
            m_prefix[MaximumRunPages + page] =
                (page & ((Size{1U} << k) - 1U)) == 0U
                    ? static_cast<UInt16>(prefix < (Size{1U} << k) ? prefix : (Size{1U} << k))
                    : UInt16{0U};
        }
        for (Size node{MaximumRunPages - 1U}; node != 0U; --node)
        {
            m_prefix[node] =
                m_prefix[node * 2U] > m_prefix[node * 2U + 1U] ? m_prefix[node * 2U] : m_prefix[node * 2U + 1U];
        }
        for (Size j{k}; j < Orders; ++j)
        {
            UInt16 maximum{0U};
            for (Size block{0U}; block < (MaximumRunPages >> j); ++block)
            {
                if (!segment.Free[j].Get((base >> j) + block))
                {
                    continue;
                }
                const UInt16 value{m_prefix[(MaximumRunPages >> j) + block]};
                if (value > maximum)
                {
                    maximum = value;
                }
                if (k == 0U)
                {
                    for (Size page{block << j}; page < ((block + 1U) << j); ++page)
                    {
                        dirtyFree = dirtyFree || Bit(m_chunkDirty, page);
                        collectionFree = collectionFree || Bit(segment.CollectionDirty, base + page);
                    }
                }
            }
            segment.Clean[Pair(k, j)].Set(chunk, maximum);
        }
    }
    segment.DirtyChunks.Set(chunk, dirtyFree);
    segment.CollectionChunks.Set(chunk, collectionFree);
}
void PhysicalPageAllocator::Changed(Segment& segment, const Size first, const Size pages, const bool cached) noexcept
{
    Rebuild(segment, first / MaximumRunPages, cached);
    if ((first + pages - 1U) / MaximumRunPages != first / MaximumRunPages)
    {
        Rebuild(segment, (first + pages - 1U) / MaximumRunPages);
    }
}
Error PhysicalPageAllocator::Initialize(const PhysicalAddress start, const UInt64 pages, const bool dirty) noexcept
{
    if (pages == 0U || pages > MaximumManagedPages)
    {
        return Error::InvalidConfiguration;
    }
    const PhysicalPageImportRange range{start, pages};
    const auto imported = Import(&range, 1U, m_legacy, sizeof(m_legacy), 0U, 0U);
    if (!imported.HasValue())
    {
        return imported.GetError();
    }
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    m_directional = false;
    m_watermark = 0U;
    if (dirty)
    {
        // Single-range initialization makes every dirty page collectible.
        for (Size index{0U}; index < m_segmentCount; ++index)
        {
            auto& segment = m_segments[index];
            for (Size word{0U}; word < segment.Pages / 64U; ++word)
            {
                segment.CollectionDirty[word] = segment.Dirty[word];
            }
            for (Size chunk{0U}; chunk < segment.Pages / MaximumRunPages; ++chunk)
            {
                Rebuild(segment, chunk);
            }
        }
    }
    if (!dirty)
    {
        for (Size index{0U}; index < m_segmentCount; ++index)
        {
            auto& segment = m_segments[index];
            for (Size word{0U}; word < segment.Pages / 64U; ++word)
            {
                segment.Dirty[word] = 0U;
            }
            for (Size chunk{0U}; chunk < segment.Pages / MaximumRunPages; ++chunk)
            {
                Rebuild(segment, chunk);
            }
        }
        m_snapshot.CleanPages = m_snapshot.ManagedPages;
        m_snapshot.DirtyPages = 0U;
    }
    return Error::None;
}
Foundation::Result<PhysicalPageImportResult, Error>
PhysicalPageAllocator::Import(const PhysicalPageImportRange* const ranges, const Size count, void* const metadata,
                              const Size metadataBytes, const UInt64 lowOverride,
                              const UInt64 minimumLowBlocks) noexcept
{
    using Result = Foundation::Result<PhysicalPageImportResult, Error>;
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_ready)
    {
        return Result::Failure(Error::AlreadyInitialized);
    }
    if (ranges == nullptr || count == 0U || count > 4096U || metadata == nullptr ||
        (reinterpret_cast<Foundation::UIntPointer>(metadata) & 7U) != 0U)
    {
        return Result::Failure(Error::InvalidConfiguration);
    }
    UInt64 previous{0U};
    UInt64 total{0U};
    for (Size i{0U}; i < count; ++i)
    {
        const UInt64 start{ranges[i].Start.GetValue()};
        const UInt64 pages{ranges[i].Pages};
        if (start == 0U || start % MappingPageSizeBytes != 0U || start < previous || pages == 0U ||
            pages > (~UInt64{0U} - start) / MappingPageSizeBytes)
        {
            return Result::Failure(Error::InvalidConfiguration);
        }
        previous = start + pages * MappingPageSizeBytes;
        total += pages;
    }
    auto* memory = static_cast<Foundation::UInt8*>(metadata);
    Size used{0U};
    Size range{0U};
    UInt64 position{ranges[0U].Start.GetValue() / MappingPageSizeBytes};
    // Two passes over ranges per segment, at most 64 segments; no RAM scan.
    for (Size attempt{0U}; range < count && attempt < MaximumSegments; ++attempt)
    {
        const UInt64 base{position & ~UInt64{MaximumRunPages - 1U}};
        const UInt64 boundary{base < LowEnd ? LowEnd : ~UInt64{MaximumRunPages - 1U}};
        Size last{range};
        UInt64 end{position};
        for (; last < count; ++last)
        {
            const UInt64 begin{ranges[last].Start.GetValue() / MappingPageSizeBytes};
            if (begin >= boundary || (last != range && begin > end && begin - end > MergeHolePages))
            {
                break;
            }
            const UInt64 finish{begin + ranges[last].Pages};
            end = finish < boundary ? finish : boundary;
            if (finish >= boundary)
            {
                break;
            }
        }
        const UInt64 paddedEnd{(end + MaximumRunPages - 1U) & ~UInt64{MaximumRunPages - 1U}};
        const Size span{static_cast<Size>(paddedEnd - base)};
        const Size needed{MetadataBytes(span)};
        if (needed > metadataBytes - used)
        {
            break;
        }
        for (Size byte{0U}; byte < needed; ++byte)
        {
            memory[byte] = 0U;
        }
        auto& segment = m_segments[m_segmentCount++];
        Prepare(segment, base, span, memory);
        used += needed;
        for (Size index{range}; index < count; ++index)
        {
            const UInt64 begin{ranges[index].Start.GetValue() / MappingPageSizeBytes};
            if (begin >= end)
            {
                break;
            }
            const UInt64 first{begin > position ? begin : position};
            const UInt64 finish{begin + ranges[index].Pages < end ? begin + ranges[index].Pages : end};
            for (UInt64 page{first}; page < finish;)
            {
                const Size local{static_cast<Size>(page - base)};
                const Size length{static_cast<Size>((finish - page < MaximumRunPages - local % MaximumRunPages)
                                                        ? finish - page
                                                        : MaximumRunPages - local % MaximumRunPages)};
                for (Size bit{local}; bit < local + length; ++bit)
                {
                    Set(segment.Managed, bit, true);
                    Set(segment.Dirty, bit, true);
                }
                FreeRange(segment, local, length);
                segment.DirtyChunks.Set(local / MaximumRunPages, true);
                m_snapshot.ManagedPages += length;
                page += length;
            }
        }
        position = end;
        while (range < count && ranges[range].Start.GetValue() / MappingPageSizeBytes + ranges[range].Pages <= position)
        {
            ++range;
        }
        if (range < count && ranges[range].Start.GetValue() / MappingPageSizeBytes > position)
        {
            position = ranges[range].Start.GetValue() / MappingPageSizeBytes;
        }
    }
    const UInt64 low{LowBlocks()};
    const UInt64 defaultWatermark{low / 8U < 16U ? low / 8U : 16U};
    if (lowOverride != 0U && (lowOverride < minimumLowBlocks || lowOverride > low))
    {
        return Result::Failure(Error::InvalidConfiguration);
    }
    m_watermark = lowOverride != 0U ? lowOverride : defaultWatermark;
    m_snapshot.PhysicalStart = ranges[0U].Start;
    m_snapshot.AllocatablePages = m_snapshot.ManagedPages;
    m_snapshot.DirtyPages = m_snapshot.ManagedPages;
    m_directional = true;
    m_ready = true;
    m_import = {m_snapshot.ManagedPages, total - m_snapshot.ManagedPages, used, m_watermark, m_segmentCount};
    return Result::Success(m_import);
}
bool PhysicalPageAllocator::Contains(const PhysicalAddress start, const UInt64 pages) const noexcept
{
    if (pages == 0U || pages > MaximumRunPages || start.GetValue() % MappingPageSizeBytes != 0U)
    {
        return false;
    }
    const UInt64 page{start.GetValue() / MappingPageSizeBytes};
    for (Size index{0U}; index < m_segmentCount; ++index)
    {
        const auto& segment = m_segments[index];
        if (page < segment.Base || page - segment.Base >= segment.Pages ||
            pages > segment.Pages - (page - segment.Base))
        {
            continue;
        }
        for (Size bit{static_cast<Size>(page - segment.Base)}; bit < page - segment.Base + pages; ++bit)
        {
            if (!Bit(segment.Managed, bit))
            {
                return false;
            }
        }
        return true;
    }
    return false;
}
PhysicalPageAllocator::Segment* PhysicalPageAllocator::FindRun(const PhysicalPageRun& run, Size& first) noexcept
{
    if (!Contains(run.PhysicalStart, run.Pages))
    {
        return nullptr;
    }
    const UInt64 page{run.PhysicalStart.GetValue() / MappingPageSizeBytes};
    for (Size index{0U}; index < m_segmentCount; ++index)
    {
        auto& segment = m_segments[index];
        if (page >= segment.Base && page - segment.Base < segment.Pages)
        {
            first = static_cast<Size>(page - segment.Base);
            return &segment;
        }
    }
    return nullptr;
}
bool PhysicalPageAllocator::IsClean(const Segment& segment, const Size first, const Size pages) const noexcept
{
    for (Size page{first}; page < first + pages;)
    {
        const Size count{first + pages - page < 64U - page % 64U ? first + pages - page : 64U - page % 64U};
        const UInt64 mask{(~UInt64{0U} >> (64U - count)) << (page % 64U)};
        if ((segment.Dirty[page / 64U] & mask) != 0U)
        {
            return false;
        }
        page += count;
    }
    return true;
}
PhysicalPageRun PhysicalPageAllocator::MakeRun(const Segment& segment, const Size first, const Size pages,
                                               const bool cached) const noexcept
{
    const UInt64* const dirty = cached ? m_chunkDirty : segment.Dirty;
    const Size sourceFirst = cached ? first % MaximumRunPages : first;
    PhysicalPageRun run{PhysicalAddress{(segment.Base + first) * MappingPageSizeBytes}, pages, {}};
    for (Size word{0U}; word < (pages + 63U) / 64U; ++word)
    {
        const Size page{sourceFirst + word * 64U};
        UInt64 bits{dirty[page / 64U] >> (page % 64U)};
        if (page % 64U != 0U && word * 64U + 64U - page % 64U < pages)
        {
            bits |= dirty[page / 64U + 1U] << (64U - page % 64U);
        }
        const Size remaining{pages - word * 64U};
        run.DirtyMask[word] = bits & (remaining >= 64U ? ~UInt64{0U} : (~UInt64{0U} >> (64U - remaining)));
    }
    return run;
}
UInt64 PhysicalPageAllocator::LowBlocks() const noexcept
{
    UInt64 result{0U};
    for (Size index{0U}; index < m_segmentCount; ++index)
    {
        if (m_segments[index].Base + m_segments[index].Pages <= LowEnd)
        {
            result += m_segments[index].Counts[MaximumOrder];
        }
    }
    return result;
}
RunResult PhysicalPageAllocator::Allocate(const UInt64 pages, const Foundation::UInt8 alignmentOrder,
                                          const Foundation::UInt8 limitBits, const bool cleanOnly) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (!m_ready)
    {
        return RunResult::Failure(Error::NotReady);
    }
    if (pages == 0U || pages > MaximumRunPages || alignmentOrder > MaximumOrder ||
        (limitBits != 0U && limitBits != 32U))
    {
        return RunResult::Failure(Error::InvalidRun);
    }
    Size required{alignmentOrder};
    while ((Size{1U} << required) < pages)
    {
        ++required;
    }
    const bool reverse{m_directional && limitBits == 0U};
    for (Size order{required}; order < Orders; ++order)
    {
        Segment* chosen{nullptr};
        Size chosenBlock{0U};
        Size chosenFirst{0U};
        Size candidates{0U};
        bool done{false};
        for (Size step{0U}; step < m_segmentCount && !done; ++step)
        {
            auto& segment = m_segments[reverse ? m_segmentCount - 1U - step : step];
            if (segment.Counts[order] == 0U || (limitBits == 32U && segment.Base >= LowEnd))
            {
                continue;
            }
            if (m_directional && limitBits == 0U && order == MaximumOrder && segment.Base < LowEnd &&
                LowBlocks() <= m_watermark)
            {
                continue;
            }
            Size firstBlock{reverse ? segment.Free[order].Bits - 1U : 0U};
            Size chunk{segment.Pages / MaximumRunPages};
            if (cleanOnly)
            {
                chunk = segment.Clean[Pair(required, order)].Find(static_cast<UInt16>(pages), reverse);
                if (chunk == segment.Pages / MaximumRunPages)
                {
                    continue;
                }
                firstBlock = (chunk * MaximumRunPages >> order) + (reverse ? (MaximumRunPages >> order) - 1U : 0U);
            }
            Size loadedChunk{segment.Pages / MaximumRunPages};
            for (Size scan{0U}; scan < (cleanOnly ? MaximumRunPages : 4U); ++scan)
            {
                const Size block{segment.Free[order].Find(firstBlock, reverse)};
                if (block == segment.Free[order].Bits || (cleanOnly && (block << order) / MaximumRunPages != chunk))
                {
                    break;
                }
                const Size base{block << order};
                const Size currentChunk{base / MaximumRunPages};
                if (loadedChunk != currentChunk)
                {
                    for (Size word{0U}; word < DirtyWords; ++word)
                    {
                        m_chunkDirty[word] = segment.Dirty[currentChunk * DirtyWords + word];
                    }
                    loadedChunk = currentChunk;
                }
                Size selected{reverse ? base + (Size{1U} << order) - (Size{1U} << required) : base};
                auto candidate = MakeRun(segment, selected, static_cast<Size>(pages), true);
                bool clean{DirtyCount(candidate) == 0U};
                if (cleanOnly)
                {
                    const Size descendants{Size{1U} << (order - required)};
                    for (Size child{0U}; child < descendants; ++child)
                    {
                        selected = base + ((reverse ? descendants - 1U - child : child) << required);
                        candidate = MakeRun(segment, selected, static_cast<Size>(pages), true);
                        clean = DirtyCount(candidate) == 0U;
                        if (clean)
                        {
                            break;
                        }
                    }
                }
                if (!cleanOnly || clean)
                {
                    if (chosen == nullptr || clean)
                    {
                        chosen = &segment;
                        chosenBlock = block;
                        chosenFirst = selected;
                        m_choice = candidate;
                        for (Size word{0U}; word < DirtyWords; ++word)
                        {
                            m_choiceDirty[word] = m_chunkDirty[word];
                        }
                    }
                    if (clean || ++candidates == 4U)
                    {
                        done = true;
                        break;
                    }
                }
                if (reverse && block == 0U)
                {
                    break;
                }
                firstBlock = reverse ? block - 1U : block + 1U;
            }
        }
        if (chosen == nullptr)
        {
            continue;
        }
        auto& segment = *chosen;
        SetFree(segment, order, chosenBlock, false);
        Size split{order};
        while (split > required)
        {
            --split;
            chosenBlock <<= 1U;
            const Size selected{(chosenFirst >> split) & 1U};
            SetFree(segment, split, chosenBlock + (selected ^ 1U), true);
            chosenBlock += selected;
        }
        const Size first{chosenBlock << required};
        const PhysicalPageRun run{m_choice};
        FreeRange(segment, first + static_cast<Size>(pages), (Size{1U} << required) - static_cast<Size>(pages));
        for (Size page{first}; page < first + pages; ++page)
        {
            Set(segment.Allocated, page, true);
            Set(segment.CollectionDirty, page, false);
        }
        const UInt64 dirty{DirtyCount(run)};
        m_snapshot.AllocatedPages += pages;
        m_snapshot.DirtyPages -= dirty;
        m_snapshot.CleanPages -= pages - dirty;
        for (Size word{0U}; word < DirtyWords; ++word)
        {
            m_chunkDirty[word] = m_choiceDirty[word];
        }
        Changed(segment, first, static_cast<Size>(pages), true);
        return RunResult::Success(run);
    }
    return RunResult::Failure(Error::Exhausted);
}
Error PhysicalPageAllocator::Return(const PhysicalPageRun& run, const bool verified) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (!m_ready)
    {
        return Error::NotReady;
    }
    Size first{0U};
    auto* const found = FindRun(run, first);
    if (found == nullptr)
    {
        return Error::InvalidRun;
    }
    auto& segment = *found;
    for (Size page{first}; page < first + run.Pages; ++page)
    {
        if (!Bit(segment.Allocated, page))
        {
            return Error::NotAllocated;
        }
    }
    if (verified)
    {
        for (const UInt64 word : run.DirtyMask)
        {
            if (word != 0U)
            {
                return Error::Unverified;
            }
        }
    }
    for (Size page{first}; page < first + run.Pages; ++page)
    {
        Set(segment.Allocated, page, false);
        Set(segment.Dirty, page, !verified);
        Set(segment.CollectionDirty, page, !verified);
    }
    FreeRange(segment, first, static_cast<Size>(run.Pages));
    m_snapshot.AllocatedPages -= run.Pages;
    if (verified)
    {
        m_snapshot.CleanPages += run.Pages;
    }
    else
    {
        m_snapshot.DirtyPages += run.Pages;
    }
    Changed(segment, first, static_cast<Size>(run.Pages));
    return Error::None;
}
Error PhysicalPageAllocator::Release(const PhysicalPageRun& run) noexcept
{
    return Return(run, false);
}
Error PhysicalPageAllocator::ReturnVerified(const PhysicalPageRun& run) noexcept
{
    return Return(run, true);
}
RunResult PhysicalPageAllocator::ClaimDirty(const Foundation::UInt8 maximumOrder) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (!m_ready)
    {
        return RunResult::Failure(Error::NotReady);
    }
    if (maximumOrder > MaximumScrubOrder)
    {
        return RunResult::Failure(Error::InvalidRun);
    }
    for (Size index{0U}; index < m_segmentCount; ++index)
    {
        auto& segment = m_segments[index];
        const Size chunk{segment.DirtyChunks.Find(0U, false)};
        if (chunk == segment.DirtyChunks.Bits)
        {
            continue;
        }
        Size first{segment.Pages};
        for (Size word{chunk * DirtyWords}; word < (chunk + 1U) * DirtyWords; ++word)
        {
            const UInt64 bits{segment.Dirty[word] & ~segment.Allocated[word] & ~segment.Claimed[word]};
            if (bits != 0U)
            {
                first = word * 64U + Extreme(bits, false);
                break;
            }
        }
        for (Size order{0U}; order < Orders; ++order)
        {
            Size block{first >> order};
            if (!segment.Free[order].Get(block))
            {
                continue;
            }
            SetFree(segment, order, block, false);
            Size split{order};
            while (split != 0U && split > maximumOrder)
            {
                --split;
                block <<= 1U;
                const Size selected{(first >> split) & 1U};
                SetFree(segment, split, block + (selected ^ 1U), true);
                block += selected;
            }
            const Size base{block << split};
            const Size pages{Size{1U} << split};
            const PhysicalPageRun run{MakeRun(segment, base, pages)};
            for (Size page{base}; page < base + pages; ++page)
            {
                Set(segment.Claimed, page, true);
            }
            Set(segment.ClaimStarts, base, true);
            Set(segment.ClaimEnds, base + pages - 1U, true);
            const UInt64 dirty{DirtyCount(run)};
            m_snapshot.ClaimedPages += pages;
            m_snapshot.DirtyPages -= dirty;
            m_snapshot.CleanPages -= pages - dirty;
            Changed(segment, base, pages);
            return RunResult::Success(run);
        }
    }
    return RunResult::Failure(Error::Exhausted);
}
// The transitional collection facade claims a contiguous released dirty prefix in one transaction.
// Repair each touched chunk once, rather than 128 index rebuilds for one collection visit.
RunResult PhysicalPageAllocator::ClaimDirtyPrefix(const UInt64 maximumPages) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (!m_ready)
    {
        return RunResult::Failure(Error::NotReady);
    }
    if (maximumPages == 0U || maximumPages > MaximumRunPages)
    {
        return RunResult::Failure(Error::InvalidRun);
    }
    for (Size index{0U}; index < m_segmentCount; ++index)
    {
        auto& segment = m_segments[index];
        const Size chunk{segment.CollectionChunks.Find(0U, false)};
        if (chunk == segment.DirtyChunks.Bits)
        {
            continue;
        }
        Size first{segment.Pages};
        for (Size word{chunk * DirtyWords}; word < (chunk + 1U) * DirtyWords; ++word)
        {
            const UInt64 bits{segment.CollectionDirty[word] & ~segment.Allocated[word] & ~segment.Claimed[word]};
            if (bits != 0U)
            {
                first = word * 64U + Extreme(bits, false);
                break;
            }
        }
        Size pages{0U};
        for (; pages < maximumPages && first + pages < segment.Pages; ++pages)
        {
            const Size page{first + pages};
            if (!Bit(segment.CollectionDirty, page) || Bit(segment.Allocated, page) || Bit(segment.Claimed, page))
            {
                break;
            }
            for (Size order{0U}; order < Orders; ++order)
            {
                Size block{page >> order};
                if (!segment.Free[order].Get(block))
                {
                    continue;
                }
                SetFree(segment, order, block, false);
                for (Size split{order}; split != 0U;)
                {
                    --split;
                    block <<= 1U;
                    const Size selected{(page >> split) & 1U};
                    SetFree(segment, split, block + (selected ^ 1U), true);
                    block += selected;
                }
                Set(segment.Claimed, page, true);
                break;
            }
        }
        if (pages == 0U)
        {
            return RunResult::Failure(Error::Exhausted);
        }
        Set(segment.ClaimStarts, first, true);
        Set(segment.ClaimEnds, first + pages - 1U, true);
        const PhysicalPageRun run{MakeRun(segment, first, pages)};
        m_snapshot.ClaimedPages += pages;
        m_snapshot.DirtyPages -= pages;
        Changed(segment, first, pages);
        return RunResult::Success(run);
    }
    return RunResult::Failure(Error::Exhausted);
}
Error PhysicalPageAllocator::EndClaim(const PhysicalPageRun& run, const Foundation::UInt8 action) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (!m_ready)
    {
        return Error::NotReady;
    }
    Size first{0U};
    const bool facade{action >= 4U};
    const auto operation = static_cast<Foundation::UInt8>(action & 3U);
    auto* const found = FindRun(run, first);
    if (found == nullptr || (!facade && (run.Pages > MaximumScrubPages || (run.Pages & (run.Pages - 1U)) != 0U ||
                                         (first & (run.Pages - 1U)) != 0U)))
    {
        return Error::InvalidRun;
    }
    auto& segment = *found;
    if (!Bit(segment.ClaimStarts, first) || !Bit(segment.ClaimEnds, first + static_cast<Size>(run.Pages) - 1U))
    {
        return Error::NotClaimed;
    }
    for (Size page{first}; page < first + run.Pages; ++page)
    {
        if (!Bit(segment.Claimed, page) || Bit(segment.Retired, page) ||
            (!facade && ((page != first && Bit(segment.ClaimStarts, page)) ||
                         (page + 1U != first + run.Pages && Bit(segment.ClaimEnds, page)))))
        {
            return Error::NotClaimed;
        }
    }
    const UInt64 dirty{DirtyCount(MakeRun(segment, first, static_cast<Size>(run.Pages)))};
    for (Size page{first}; page < first + run.Pages; ++page)
    {
        if (operation == 2U)
        {
            Set(segment.Retired, page, true);
        }
        else
        {
            Set(segment.Claimed, page, false);
            Set(segment.ClaimStarts, page, false);
            Set(segment.ClaimEnds, page, false);
            if (operation == 0U)
            {
                Set(segment.Dirty, page, false);
                Set(segment.CollectionDirty, page, false);
            }
        }
    }
    m_snapshot.ClaimedPages -= run.Pages;
    if (operation == 2U)
    {
        m_snapshot.RetiredPages += run.Pages;
        m_snapshot.AllocatablePages -= run.Pages;
    }
    else
    {
        FreeRange(segment, first, static_cast<Size>(run.Pages));
        m_snapshot.DirtyPages += operation == 0U ? 0U : dirty;
        m_snapshot.CleanPages += operation == 0U ? run.Pages : run.Pages - dirty;
    }
    Changed(segment, first, static_cast<Size>(run.Pages));
    return Error::None;
}
Error PhysicalPageAllocator::FinishScrub(const PhysicalPageRun& run) noexcept
{
    return EndClaim(run, 0U);
}
Error PhysicalPageAllocator::AbortScrub(const PhysicalPageRun& run) noexcept
{
    return EndClaim(run, 1U);
}
Error PhysicalPageAllocator::Retire(const PhysicalPageRun& run) noexcept
{
    return EndClaim(run, 2U);
}
PhysicalPageAllocatorSnapshot PhysicalPageAllocator::GetSnapshot() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    auto result = m_snapshot;
    result.FreeLowBlocks = LowBlocks();
    return result;
}
} // Namespace Asteria::Kernel::Memory
