// SPDX-License-Identifier: Apache-2.0
#include "LibcTestSupport.hpp"

#include <cstddef>
#include <cstring>

#include "User/Libc/LibcHooks.hpp"
#include "User/Libc/Include/stdlib.h"
#include "User/Libc/Include/string.h"
#include "User/Libc/Include/errno.h"

namespace Asteria::Test::Libc
{
namespace
{
namespace Core = Asteria::User::Libc;
constexpr size_t PageCount{512U};
alignas(4096U) unsigned char g_pages[PageCount * 4096U]{};
bool g_reserved[PageCount]{};
size_t g_livePages{0U};
bool g_denyReserve{false};
bool g_denyRelease{false};
bool g_modifyError{false};
size_t g_locks{0U};
size_t g_unlocks{0U};
int g_error{0};

[[nodiscard]] void* Reserve(void*, const size_t pages) noexcept
{
    if (g_modifyError)
    {
        g_error = EAGAIN;
    }
    if (g_denyReserve || pages > PageCount)
    {
        return nullptr;
    }
    for (size_t start{0U}; start <= PageCount - pages; ++start)
    {
        size_t count{0U};
        while (count < pages && !g_reserved[start + count])
        {
            ++count;
        }
        if (count == pages)
        {
            for (size_t index{0U}; index < pages; ++index)
            {
                g_reserved[start + index] = true;
            }
            g_livePages += pages;
            return g_pages + start * 4096U;
        }
        start += count;
    }
    return nullptr;
}

[[nodiscard]] bool Release(void*, void* const base, const size_t pages) noexcept
{
    if (g_denyRelease)
    {
        return false;
    }
    const auto start = static_cast<size_t>(static_cast<unsigned char*>(base) - g_pages) / 4096U;
    for (size_t index{0U}; index < pages; ++index)
    {
        g_reserved[start + index] = false;
    }
    g_livePages -= pages;
    return true;
}
void Lock(void*) noexcept
{
    ++g_locks;
}
void Unlock(void*) noexcept
{
    ++g_unlocks;
}
[[nodiscard]] int* Error() noexcept
{
    return &g_error;
}

struct Allocation
{
    unsigned char* Pointer;
    size_t Size;
    unsigned int Tag;
};
Allocation g_model[128U]{};
void* g_fragments[512U]{};

[[nodiscard]] unsigned char Byte(const unsigned int tag, const size_t offset) noexcept
{
    return static_cast<unsigned char>((tag + offset * 17U + offset / 13U) & 255U);
}
void Fill(Allocation& allocation, const size_t start) noexcept
{
    for (size_t offset{start}; offset < allocation.Size; ++offset)
    {
        allocation.Pointer[offset] = Byte(allocation.Tag, offset);
    }
}
[[nodiscard]] bool Valid(const Allocation& allocation, const bool full) noexcept
{
    if (allocation.Pointer == nullptr)
    {
        return allocation.Size == 0U;
    }
    if (reinterpret_cast<std::uintptr_t>(allocation.Pointer) % alignof(std::max_align_t) != 0U)
    {
        return false;
    }
    for (size_t offset{0U}; offset < allocation.Size; ++offset)
    {
        if (allocation.Pointer[offset] != Byte(allocation.Tag, offset))
        {
            return false;
        }
        if (!full && offset == 7U && allocation.Size > 16U)
        {
            offset = allocation.Size - 9U;
        }
    }
    return true;
}

void Model(TestContext& context, Random& random) noexcept
{
    for (size_t step{0U}; step < 12000U && context.GetFailureCount() < 10U; ++step)
    {
        const size_t index{static_cast<size_t>(random.Next() % 128U)};
        Allocation& slot = g_model[index];
        ASTERIA_EXPECT(context, "model payload survives previous operations", Valid(slot, true));
        const size_t requested{static_cast<size_t>(random.Next() % 16384U)};
        const auto operation = random.Next() % 3U;
        if (slot.Pointer == nullptr)
        {
            void* const pointer = operation == 0U ? asteria_libc_calloc(requested, 1U) : asteria_libc_malloc(requested);
            if (pointer != nullptr)
            {
                if (operation == 0U)
                {
                    bool zero{true};
                    for (size_t offset{0U}; offset < requested; ++offset)
                    {
                        zero = zero && static_cast<unsigned char*>(pointer)[offset] == 0U;
                    }
                    ASTERIA_EXPECT(context, "calloc clears all requested bytes", zero);
                }
                slot = Allocation{.Pointer = static_cast<unsigned char*>(pointer),
                                  .Size = requested,
                                  .Tag = static_cast<unsigned int>(random.Next())};
                Fill(slot, 0U);
            }
            else
            {
                ASTERIA_EXPECT(context, "provider exhaustion sets ENOMEM", g_error == ENOMEM);
            }
        }
        else if (operation == 0U)
        {
            asteria_libc_free(slot.Pointer);
            slot = Allocation{.Pointer = nullptr, .Size = 0U, .Tag = 0U};
        }
        else
        {
            const size_t oldSize{slot.Size};
            auto* const resized = static_cast<unsigned char*>(asteria_libc_realloc(slot.Pointer, requested));
            if (requested == 0U)
            {
                ASTERIA_EXPECT(context, "realloc zero releases allocation", resized == nullptr);
                slot = Allocation{.Pointer = nullptr, .Size = 0U, .Tag = 0U};
            }
            else if (resized != nullptr)
            {
                slot.Pointer = resized;
                slot.Size = requested < oldSize ? requested : oldSize;
                ASTERIA_EXPECT(context, "realloc preserves prefix", Valid(slot, true));
                slot.Size = requested;
                Fill(slot, oldSize < requested ? oldSize : requested);
            }
            else
            {
                ASTERIA_EXPECT(context, "failed realloc preserves the original allocation", g_error == ENOMEM,
                               Valid(slot, true));
            }
        }
        bool snapshot{g_livePages <= PageCount && g_locks == g_unlocks};
        for (const Allocation& allocation : g_model)
        {
            snapshot = snapshot && Valid(allocation, false);
        }
        if (!ASTERIA_EXPECT(context, "seeded allocation model snapshots and lock counts", snapshot))
        {
            static_cast<void>(std::fprintf(stderr, "step=%zu index=%zu operation=%llu size=%zu livePages=%zu\n", step,
                                           index, static_cast<unsigned long long>(operation), requested, g_livePages));
        }
    }
    for (Allocation& slot : g_model)
    {
        asteria_libc_free(slot.Pointer);
        slot = Allocation{.Pointer = nullptr, .Size = 0U, .Tag = 0U};
    }
    ASTERIA_EXPECT(context, "all pages return to provider after random model", g_livePages == 0U);
}

void Boundaries(TestContext& context) noexcept
{
    ASTERIA_EXPECT(context, "checked allocation arithmetic", asteria_libc_malloc(SIZE_MAX) == nullptr,
                   g_error == ENOMEM, asteria_libc_calloc(SIZE_MAX, 2U) == nullptr, g_error == ENOMEM);
    void* output{g_pages};
    ASTERIA_EXPECT(context, "posix_memalign validates and preserves output",
                   asteria_libc_posix_memalign(&output, 3U, 100U) == EINVAL, output == g_pages);
    g_modifyError = true;
    g_denyReserve = true;
    g_error = EDOM;
    ASTERIA_EXPECT(context, "POSIX memalign preserves errno even when the provider changes it",
                   asteria_libc_posix_memalign(&output, 64U, 123456U) == ENOMEM, output == g_pages, g_error == EDOM);
    g_modifyError = false;
    g_denyReserve = false;
    for (size_t alignment{1U}; alignment <= 65536U; alignment *= 2U)
    {
        void* const pointer = asteria_libc_aligned_alloc(alignment, alignment * 2U);
        ASTERIA_EXPECT(context, "aligned allocation", pointer != nullptr,
                       reinterpret_cast<std::uintptr_t>(pointer) % alignment == 0U);
        asteria_libc_free(pointer);
        if (alignment < sizeof(void*))
        {
            continue;
        }
        ASTERIA_EXPECT(context, "posix alignment with unrestricted size",
                       asteria_libc_posix_memalign(&output, alignment, 111U) == 0,
                       reinterpret_cast<std::uintptr_t>(output) % alignment == 0U);
        asteria_libc_free(output);
    }
    ASTERIA_EXPECT(context, "aligned_alloc rejects size not divisible by alignment",
                   asteria_libc_aligned_alloc(64U, 65U) == nullptr, g_error == EINVAL);
    char* const duplicate = asteria_libc_strdup("hello");
    char* const truncated = asteria_libc_strndup("abcdef", 3U);
    ASTERIA_EXPECT(context, "string allocation uses page provider", duplicate != nullptr, truncated != nullptr,
                   std::strcmp(duplicate, "hello") == 0, std::strcmp(truncated, "abc") == 0);
    asteria_libc_free(duplicate);
    asteria_libc_free(truncated);
    ASTERIA_EXPECT(context, "alignment and strings release pages", g_livePages == 0U);
    auto* const original = static_cast<unsigned char*>(asteria_libc_malloc(64U));
    ASTERIA_EXPECT(context, "exhaustion fixture allocates", original != nullptr);
    if (original != nullptr)
    {
        original[0U] = 0xA5U;
        g_denyReserve = true;
        ASTERIA_EXPECT(context, "failed realloc is atomic", asteria_libc_realloc(original, 100000U) == nullptr,
                       original[0U] == 0xA5U, g_error == ENOMEM);
        g_denyReserve = false;
        asteria_libc_free(original);
    }
    asteria_libc_free(nullptr);
}

void Fragmentation(TestContext& context) noexcept
{
    g_denyRelease = true;
    void* const seed = asteria_libc_malloc(128U * 1024U);
    ASTERIA_EXPECT(context, "large-block path reserves region", seed != nullptr);
    asteria_libc_free(seed);
    g_denyReserve = true;
    size_t count{0U};
    for (; count < 512U; ++count)
    {
        g_fragments[count] = asteria_libc_malloc(1024U);
        if (g_fragments[count] == nullptr)
        {
            break;
        }
    }
    ASTERIA_EXPECT(context, "retained region exhausts predictably", count > 100U, count < 512U, g_error == ENOMEM);
    for (size_t index{0U}; index < count; index += 2U)
    {
        asteria_libc_free(g_fragments[index]);
    }
    ASTERIA_EXPECT(context, "non-adjacent free blocks cannot satisfy larger request",
                   asteria_libc_malloc(4096U) == nullptr);
    for (size_t index{1U}; index < count; index += 2U)
    {
        asteria_libc_free(g_fragments[index]);
    }
    void* const joined = asteria_libc_malloc(128U * 1024U);
    ASTERIA_EXPECT(context, "coalesced blocks restore original large capacity", joined != nullptr);
    g_denyRelease = false;
    asteria_libc_free(joined);
    g_denyReserve = false;
    ASTERIA_EXPECT(context, "successful release removes retained region", g_livePages == 0U);
}
} // namespace
} // namespace Asteria::Test::Libc

int main()
{
    using Asteria::Test::Libc::TestContext;
    namespace Test = Asteria::Test::Libc;
    namespace Core = Asteria::User::Libc;
    TestContext context{"LibcAllocator"};
    const Core::Hooks hooks{.Pages = {.Context = nullptr, .Reserve = Test::Reserve, .Release = Test::Release},
                            .HeapLock = {.Context = nullptr, .Lock = Test::Lock, .Unlock = Test::Unlock},
                            .ConversionLock = {.Context = nullptr, .Lock = nullptr, .Unlock = nullptr},
                            .ErrnoLocation = Test::Error,
                            .AssertFail = nullptr};
    ASTERIA_EXPECT(context, "hooks installed once", Core::InstallHooks(hooks), !Core::InstallHooks(hooks));
    Test::Random random;
    Test::Model(context, random);
    Test::Boundaries(context);
    Test::Fragmentation(context);
    return context.Finish();
}
