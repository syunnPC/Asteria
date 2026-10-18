// SPDX-License-Identifier: Apache-2.0
#include "LibcTestSupport.hpp"

#include <pthread.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstring>

#include "User/Libc/LibcHooks.hpp"
#include "User/Libc/Include/stdio.h"
#include "User/Libc/Include/stdlib.h"
#include "User/Libc/Include/errno.h"
#include "User/Libc/Include/assert.h"

namespace Asteria::Test::Libc
{
namespace
{
pthread_mutex_t g_heapMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t g_conversionMutex = PTHREAD_MUTEX_INITIALIZER;
thread_local int t_error{0};

[[nodiscard]] int* Error() noexcept
{
    return &t_error;
}
void Lock(void* const mutex) noexcept
{
    if (pthread_mutex_lock(static_cast<pthread_mutex_t*>(mutex)) != 0)
    {
        std::abort();
    }
}
void Unlock(void* const mutex) noexcept
{
    if (pthread_mutex_unlock(static_cast<pthread_mutex_t*>(mutex)) != 0)
    {
        std::abort();
    }
}
void AssertFailure(const char* const expression, const char* const file, const unsigned int line,
                   const char* const function) noexcept
{
    std::_Exit(std::strcmp(expression, "false") == 0 && file != nullptr && line != 0U && function != nullptr ? 42 : 99);
}
struct Worker
{
    int Index;
    bool Success;
};
void* Work(void* const argument) noexcept
{
    auto& worker = *static_cast<Worker*>(argument);
    worker.Success = true;
    for (size_t step{0U}; step < 1000U; ++step)
    {
        const int savedError{worker.Index + 100};
        ASTERIA_LIBC_ERRNO = savedError;
        char own[80U]{};
        char host[80U]{};
        const double value = worker.Index % 2 == 0 ? 0x1p-1074 : 0x1.fffffffffffffp1023;
        const int ownCount = asteria_libc_snprintf(own, sizeof(own), "%.17g", value);
        const int hostCount = std::snprintf(host, sizeof(host), "%.17g", value);
        char* end{nullptr};
        const double parsed = asteria_libc_strtod(own, &end);
        worker.Success =
            worker.Success && ownCount == hostCount && std::strcmp(own, host) == 0 && *end == '\0' && parsed == value;
        // Parsing an inexact subnormal is permitted to set ERANGE in this thread.
        if (worker.Index % 2 != 0)
        {
            worker.Success = worker.Success && ASTERIA_LIBC_ERRNO == savedError;
        }
        worker.Success = worker.Success && asteria_libc_malloc(1U) == nullptr && ASTERIA_LIBC_ERRNO == ENOMEM;
        asteria_libc_srand(static_cast<unsigned int>(worker.Index));
        worker.Success = worker.Success && asteria_libc_rand() >= 0;
    }
    return nullptr;
}
} // namespace
} // namespace Asteria::Test::Libc

int main()
{
    namespace Core = Asteria::User::Libc;
    namespace Test = Asteria::Test::Libc;
    Test::TestContext context{"LibcHooks"};
    Core::Hooks hooks{
        .Pages = {.Context = nullptr, .Reserve = nullptr, .Release = nullptr},
        .HeapLock = {.Context = &Test::g_heapMutex, .Lock = Test::Lock, .Unlock = Test::Unlock},
        .ConversionLock = {.Context = &Test::g_conversionMutex, .Lock = Test::Lock, .Unlock = Test::Unlock},
        .ErrnoLocation = Test::Error,
        .AssertFail = Test::AssertFailure};
    Core::Hooks invalid{hooks};
    invalid.ConversionLock.Unlock = nullptr;
    ASTERIA_EXPECT(context, "partial lock hooks cannot be installed", !Core::InstallHooks(invalid));
    ASTERIA_EXPECT(context, "thread and failure hooks install once", Core::InstallHooks(hooks),
                   !Core::InstallHooks(hooks));
    pthread_t threads[4U]{};
    Test::Worker workers[4U]{};
    size_t started{0U};
    ASTERIA_LIBC_ERRNO = 77;
    for (size_t index{0U}; index < 4U; ++index)
    {
        workers[index] = Test::Worker{.Index = static_cast<int>(index), .Success = false};
        const int result = pthread_create(&threads[index], nullptr, Test::Work, &workers[index]);
        if (!ASTERIA_EXPECT(context, "worker starts", result == 0))
        {
            break;
        }
        ++started;
    }
    for (size_t index{0U}; index < started; ++index)
    {
        ASTERIA_EXPECT(context, "conversion workspace and errno remain isolated between threads",
                       pthread_join(threads[index], nullptr) == 0, workers[index].Success);
    }
    ASTERIA_EXPECT(context, "thread errors do not alter caller errno", ASTERIA_LIBC_ERRNO == 77);
    const pid_t child = fork();
    ASTERIA_EXPECT(context, "failure-hook child starts", child >= 0);
    if (child == 0)
    {
        assert(false);
        std::_Exit(17);
    }
    if (child > 0)
    {
        int status{0};
#ifdef NDEBUG
        constexpr int Expected{17};
#else
        constexpr int Expected{42};
#endif
        ASTERIA_EXPECT(context, "assert uses the hook, or compiles away under NDEBUG",
                       waitpid(child, &status, 0) == child, WIFEXITED(status), WEXITSTATUS(status) == Expected);
    }
    return context.Finish();
}
