// SPDX-License-Identifier: Apache-2.0

#include "Include/stdlib.h"

#include "LibcHooks.hpp"
#include "Include/errno.h"
#include "Include/string.h"

namespace Asteria::User::Libc
{
namespace
{
// A private placement argument gives explicit metadata lifetimes without
// defining/interposing the host's standard placement-new overload.
struct Placement
{
    void* Address;
};
}
}

[[nodiscard]] inline void* operator new(size_t, Asteria::User::Libc::Placement place) noexcept
{
    return place.Address;
}

namespace Asteria::User::Libc
{
namespace
{
constexpr size_t PageBytes{4096U};
constexpr size_t Alignment{alignof(max_align_t)};
constexpr size_t BinCount{64U};
struct Region;
struct alignas(max_align_t) Block
{
    size_t Bytes;
    size_t Requested;
    Block* Previous;
    Block* Next;
    Block* FreePrevious;
    Block* FreeNext;
    Region* Owner;
    bool Available;
};
struct alignas(max_align_t) Region
{
    size_t Pages;
};
static_assert(sizeof(Block) % Alignment == 0U);
static_assert(sizeof(Region) % Alignment == 0U);
Block* g_bins[BinCount]{};

[[nodiscard]] size_t Bin(size_t bytes) noexcept
{
    size_t result{0U};
    while (bytes > 1U)
    {
        bytes >>= 1U;
        ++result;
    }
    return result;
}

void Remove(Block& block) noexcept
{
    if (block.FreePrevious != nullptr)
    {
        block.FreePrevious->FreeNext = block.FreeNext;
    }
    else
    {
        g_bins[Bin(block.Bytes)] = block.FreeNext;
    }
    if (block.FreeNext != nullptr)
    {
        block.FreeNext->FreePrevious = block.FreePrevious;
    }
    block.FreePrevious = nullptr;
    block.FreeNext = nullptr;
}

void Insert(Block& block) noexcept
{
    const size_t bin = Bin(block.Bytes);
    block.Available = true;
    block.FreePrevious = nullptr;
    block.FreeNext = g_bins[bin];
    if (block.FreeNext != nullptr)
    {
        block.FreeNext->FreePrevious = &block;
    }
    g_bins[bin] = &block;
}

[[nodiscard]] Block* Find(const size_t bytes) noexcept
{
    for (size_t bin{Bin(bytes)}; bin < BinCount; ++bin)
    {
        for (Block* block{g_bins[bin]}; block != nullptr; block = block->FreeNext)
        {
            if (block->Bytes >= bytes)
            {
                Remove(*block);
                return block;
            }
        }
    }
    return nullptr;
}

[[nodiscard]] Block* Grow(const size_t bytes) noexcept
{
    const auto& provider = GetHooks().Pages;
    if (provider.Reserve == nullptr || bytes > SIZE_MAX - sizeof(Region) - (PageBytes - 1U))
    {
        return nullptr;
    }
    const size_t pages = (bytes + sizeof(Region) + PageBytes - 1U) / PageBytes;
    void* const memory = provider.Reserve(provider.Context, pages);
    if (memory == nullptr)
    {
        return nullptr;
    }
    // Memory-owner boundary: the provider guarantees suitable alignment and lifetime.
    auto* const region = ::new (Placement{.Address = memory}) Region{.Pages = pages};
    void* const blockStorage = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(memory) + sizeof(Region));
    auto* const block = ::new (Placement{.Address = blockStorage}) Block{.Bytes = pages * PageBytes - sizeof(Region),
                                                                         .Requested = 0U,
                                                                         .Previous = nullptr,
                                                                         .Next = nullptr,
                                                                         .FreePrevious = nullptr,
                                                                         .FreeNext = nullptr,
                                                                         .Owner = region,
                                                                         .Available = true};
    return block;
}

void Split(Block& block, const size_t bytes) noexcept
{
    if (block.Bytes - bytes < sizeof(Block) + Alignment * 2U)
    {
        return;
    }
    void* const tailStorage = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(&block) + bytes);
    auto* const tail = ::new (Placement{.Address = tailStorage}) Block{.Bytes = block.Bytes - bytes,
                                                                       .Requested = 0U,
                                                                       .Previous = &block,
                                                                       .Next = block.Next,
                                                                       .FreePrevious = nullptr,
                                                                       .FreeNext = nullptr,
                                                                       .Owner = block.Owner,
                                                                       .Available = true};
    if (tail->Next != nullptr && tail->Next->Available)
    {
        Block* const next = tail->Next;
        Remove(*next);
        tail->Bytes += next->Bytes;
        tail->Next = next->Next;
    }
    if (tail->Next != nullptr)
    {
        tail->Next->Previous = tail;
    }
    block.Next = tail;
    block.Bytes = bytes;
    Insert(*tail);
}

[[nodiscard]] void* Allocate(const size_t size, const size_t alignment) noexcept
{
    if (size > SIZE_MAX - sizeof(Block) - sizeof(Block*) - alignment - Alignment)
    {
        return nullptr;
    }
    const size_t bytes = (size + sizeof(Block) + sizeof(Block*) + alignment - 1U + Alignment - 1U) & ~(Alignment - 1U);
    Block* block = Find(bytes);
    if (block == nullptr)
    {
        block = Grow(bytes);
    }
    if (block == nullptr)
    {
        return nullptr;
    }
    Split(*block, bytes);
    block->Available = false;
    block->Requested = size;
    const uintptr_t start = reinterpret_cast<uintptr_t>(block) + sizeof(Block) + sizeof(Block*);
    const uintptr_t address = (start + alignment - 1U) & ~(alignment - 1U);
    void* const result = reinterpret_cast<void*>(address);
    static_cast<void>(
        ASTERIA_LIBC_NAME(memcpy)(reinterpret_cast<void*>(address - sizeof(Block*)), &block, sizeof(block)));
    return result;
}

[[nodiscard]] Block* Header(void* const pointer) noexcept
{
    Block* block{nullptr};
    static_cast<void>(
        ASTERIA_LIBC_NAME(memcpy)(&block, static_cast<unsigned char*>(pointer) - sizeof(block), sizeof(block)));
    return block;
}

void Join(Block& left, Block& right) noexcept
{
    Remove(right);
    left.Bytes += right.Bytes;
    left.Next = right.Next;
    if (left.Next != nullptr)
    {
        left.Next->Previous = &left;
    }
}

[[nodiscard]] bool Resize(void* const pointer, const size_t size, size_t& oldSize) noexcept
{
    Block* const block = Header(pointer);
    oldSize = block->Requested;
    const size_t offset = reinterpret_cast<uintptr_t>(pointer) - reinterpret_cast<uintptr_t>(block);
    if (size > SIZE_MAX - offset - (Alignment - 1U))
    {
        return false;
    }
    const size_t bytes = (size + offset + Alignment - 1U) & ~(Alignment - 1U);
    if (bytes > block->Bytes)
    {
        if (block->Next == nullptr || !block->Next->Available || bytes - block->Bytes > block->Next->Bytes)
        {
            return false;
        }
        Join(*block, *block->Next);
    }
    Split(*block, bytes);
    block->Requested = size;
    return true;
}

void Deallocate(Block* block) noexcept
{
    if (block->Next != nullptr && block->Next->Available)
    {
        Join(*block, *block->Next);
    }
    if (block->Previous != nullptr && block->Previous->Available)
    {
        Block* const previous = block->Previous;
        // Join expects its right operand in the free list.
        Remove(*previous);
        Insert(*block);
        Join(*previous, *block);
        block = previous;
    }
    if (block->Previous == nullptr && block->Next == nullptr)
    {
        const auto& provider = GetHooks().Pages;
        Region* const owner = block->Owner;
        const size_t pages = owner->Pages;
        if (provider.Release(provider.Context, owner, pages))
        {
            return;
        }
    }
    Insert(*block);
}

[[nodiscard]] bool ValidAlignment(const size_t alignment) noexcept
{
    return alignment >= sizeof(void*) && (alignment & (alignment - 1U)) == 0U;
}
}
}

extern "C" void* ASTERIA_LIBC_NAME(malloc)(const size_t size) noexcept
{
    const auto& lock = Asteria::User::Libc::GetHooks().HeapLock;
    Asteria::User::Libc::Lock(lock);
    void* const result = Asteria::User::Libc::Allocate(size, Asteria::User::Libc::Alignment);
    Asteria::User::Libc::Unlock(lock);
    if (result == nullptr)
    {
        Asteria::User::Libc::SetError(ENOMEM);
    }
    return result;
}

extern "C" void ASTERIA_LIBC_NAME(free)(void* const pointer) noexcept
{
    if (pointer == nullptr)
    {
        return;
    }
    const auto& lock = Asteria::User::Libc::GetHooks().HeapLock;
    Asteria::User::Libc::Lock(lock);
    Asteria::User::Libc::Deallocate(Asteria::User::Libc::Header(pointer));
    Asteria::User::Libc::Unlock(lock);
}

extern "C" void* ASTERIA_LIBC_NAME(calloc)(const size_t count, const size_t size) noexcept
{
    if (size != 0U && count > SIZE_MAX / size)
    {
        Asteria::User::Libc::SetError(ENOMEM);
        return nullptr;
    }
    void* const result = ASTERIA_LIBC_NAME(malloc)(count * size);
    if (result != nullptr)
    {
        static_cast<void>(ASTERIA_LIBC_NAME(memset)(result, 0, count * size));
    }
    return result;
}

extern "C" void* ASTERIA_LIBC_NAME(realloc)(void* const pointer, const size_t size) noexcept
{
    if (pointer == nullptr)
    {
        return ASTERIA_LIBC_NAME(malloc)(size);
    }
    if (size == 0U)
    {
        ASTERIA_LIBC_NAME(free)(pointer);
        return nullptr;
    }
    const auto& lock = Asteria::User::Libc::GetHooks().HeapLock;
    size_t oldSize{0U};
    Asteria::User::Libc::Lock(lock);
    const bool resized = Asteria::User::Libc::Resize(pointer, size, oldSize);
    Asteria::User::Libc::Unlock(lock);
    if (resized)
    {
        return pointer;
    }
    void* const result = ASTERIA_LIBC_NAME(malloc)(size);
    if (result != nullptr)
    {
        static_cast<void>(ASTERIA_LIBC_NAME(memcpy)(result, pointer, oldSize));
        ASTERIA_LIBC_NAME(free)(pointer);
    }
    return result;
}

extern "C" void* ASTERIA_LIBC_NAME(aligned_alloc)(const size_t alignment, const size_t size) noexcept
{
    if (alignment == 0U || (alignment & (alignment - 1U)) != 0U || size % alignment != 0U)
    {
        Asteria::User::Libc::SetError(EINVAL);
        return nullptr;
    }
    const auto& lock = Asteria::User::Libc::GetHooks().HeapLock;
    Asteria::User::Libc::Lock(lock);
    void* const result = Asteria::User::Libc::Allocate(
        size, alignment < Asteria::User::Libc::Alignment ? Asteria::User::Libc::Alignment : alignment);
    Asteria::User::Libc::Unlock(lock);
    if (result == nullptr)
    {
        Asteria::User::Libc::SetError(ENOMEM);
    }
    return result;
}

extern "C" int ASTERIA_LIBC_NAME(posix_memalign)(void** const result, const size_t alignment,
                                                 const size_t size) noexcept
{
    if (!Asteria::User::Libc::ValidAlignment(alignment))
    {
        return EINVAL;
    }
    const int savedError = *ASTERIA_LIBC_NAME(__asteria_errno_location)();
    const auto& lock = Asteria::User::Libc::GetHooks().HeapLock;
    Asteria::User::Libc::Lock(lock);
    void* const memory = Asteria::User::Libc::Allocate(size, alignment);
    Asteria::User::Libc::Unlock(lock);
    Asteria::User::Libc::SetError(savedError);
    if (memory == nullptr)
    {
        return ENOMEM;
    }
    *result = memory;
    return 0;
}
