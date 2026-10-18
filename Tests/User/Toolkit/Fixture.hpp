// SPDX-License-Identifier: Apache-2.0
#ifndef ASTERIA_TESTS_USER_TOOLKIT_FIXTURE_HPP_INCLUDED
#define ASTERIA_TESTS_USER_TOOLKIT_FIXTURE_HPP_INCLUDED

#include "User/Toolkit/Demo.hpp"
#include "Support/TestContext.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

namespace Asteria::Test::ToolkitFixture
{
namespace Ui = User::Toolkit;
using Foundation::UInt32;
using Foundation::UInt64;
using TestSupport::TestContext;
class Host final : public Ui::IThreadHost, public Ui::IEventSource, public Ui::IPresenter
{
public:
    [[nodiscard]] bool IsUiThread() noexcept override
    {
        return std::this_thread::get_id() == m_ui;
    }
    void LockQueue() noexcept override
    {
        m_queueLock.lock();
    }
    void UnlockQueue() noexcept override
    {
        m_queueLock.unlock();
    }
    void Wait(Ui::Completion& completion) noexcept override
    {
        std::unique_lock lock{m_replyLock};
        m_reply.wait(lock,
                     [&]() noexcept
                     {
                         return __atomic_load_n(&completion.Done, __ATOMIC_ACQUIRE) != 0U;
                     });
    }
    void Finish(Ui::Completion& completion) noexcept override
    {
        {
            const std::lock_guard lock{m_replyLock};
            __atomic_store_n(&completion.Done, 1U, __ATOMIC_RELEASE);
        }
        m_reply.notify_all();
    }
    void WakeUi() noexcept override
    {
        {
            const std::lock_guard lock{m_inputLock};
            ++m_wakes;
        }
        m_input.notify_all();
    }
    [[nodiscard]] bool Poll(Ui::SourceEvent& event) noexcept override
    {
        const std::lock_guard lock{m_inputLock};
        if (m_count == 0U)
        {
            return false;
        }
        event = m_events[m_head];
        m_head = (m_head + 1U) % EventCapacity;
        --m_count;
        return true;
    }
    void WaitUntil(UInt64 deadline) noexcept override
    {
        std::unique_lock lock{m_inputLock};
        if (m_count != 0U || m_wakes != 0U)
        {
            m_wakes = 0U;
            return;
        }
        if (deadline != ~UInt64{0U})
        {
            Time = deadline;
            return;
        }
        m_input.wait(lock,
                     [&]() noexcept
                     {
                         return m_count != 0U || m_wakes != 0U;
                     });
        m_wakes = 0U;
    }
    [[nodiscard]] UInt64 Now() noexcept override
    {
        return Time;
    }
    [[nodiscard]] bool Present(UInt32 image, const Ui::Rect* damage, UInt32 count) noexcept override
    {
        if (Busy || image >= 2U || Lent[image] || count > Ui::MaximumDamage)
        {
            return false;
        }
        Lent[image] = true;
        Last = image;
        ++Presents;
        DamageCount = count;
        for (UInt32 i{}; i < count; ++i)
        {
            Rectangles[i] = damage[i];
        }
        return true;
    }
    [[nodiscard]] bool Push(Ui::Form& form, Ui::Event event) noexcept
    {
        {
            const std::lock_guard lock{m_inputLock};
            if (m_count == EventCapacity)
            {
                return false;
            }
            m_events[(m_head + m_count) % EventCapacity] = {&form, event};
            ++m_count;
        }
        m_input.notify_all();
        return true;
    }
    void Release(Ui::Form& form, UInt32 image) noexcept
    {
        if (Lent[image])
        {
            Lent[image] = false;
            static_cast<void>(form.ReturnImage(image));
        }
    }
    UInt64 Time{};
    UInt32 Last{};
    UInt32 Presents{};
    UInt32 DamageCount{};
    bool Busy{};
    bool Lent[2U]{};
    Ui::Rect Rectangles[Ui::MaximumDamage]{};

private:
    static constexpr UInt32 EventCapacity{512U};
    const std::thread::id m_ui{std::this_thread::get_id()};
    std::mutex m_queueLock{}, m_replyLock{}, m_inputLock{};
    std::condition_variable m_reply{}, m_input{};
    Ui::SourceEvent m_events[EventCapacity]{};
    UInt32 m_head{};
    UInt32 m_count{};
    UInt32 m_wakes{};
};
inline void Mouse(Ui::Application& app, Ui::Form& form, Ui::EventKind kind, int x, int y) noexcept
{
    Ui::Event event{kind};
    event.Position = {x, y};
    static_cast<void>(app.Dispatch(form, event));
}
inline void Key(Ui::Application& app, Ui::Form& form, Ui::Key key, bool shift = false, bool control = false,
                Ui::EventKind kind = Ui::EventKind::KeyDown) noexcept
{
    Ui::Event event{kind};
    event.Code = key;
    event.Shift = shift;
    event.ControlKey = control;
    static_cast<void>(app.Dispatch(form, event));
}
inline void Character(Ui::Application& app, Ui::Form& form, UInt32 scalar) noexcept
{
    Ui::Event event{Ui::EventKind::KeyPress};
    event.Scalar = scalar;
    static_cast<void>(app.Dispatch(form, event));
}
inline void Click(Ui::Application& app, Ui::Form& form, int x, int y) noexcept
{
    Mouse(app, form, Ui::EventKind::MouseDown, x, y);
    Mouse(app, form, Ui::EventKind::MouseUp, x, y);
}
inline User::Graphics::MemorySurface Surface(UInt32* pixels, UInt32 width, UInt32 height) noexcept
{
    return {pixels, width, height, width, User::Graphics::PixelFormat::Bgrx};
}
inline bool Golden(const char* name, User::Graphics::MemorySurface surface, bool record) noexcept
{
    char path[1024U]{};
    static_cast<void>(std::snprintf(path, sizeof(path), "%s/%s.ppm", ASTERIA_TOOLKIT_GOLDEN_DIR, name));
    std::FILE* file{std::fopen(path, record ? "wb" : "rb")};
    if (file == nullptr)
    {
        return false;
    }
    bool ok{true};
    if (record)
    {
        ok = std::fprintf(file, "P6\n%u %u\n255\n", surface.Width, surface.Height) > 0;
    }
    else
    {
        char magic[3U]{};
        unsigned int width{};
        unsigned int height{};
        unsigned int maximum{};
        ok = std::fscanf(file, "%2s %u %u %u", magic, &width, &height, &maximum) == 4 &&
             std::strcmp(magic, "P6") == 0 && width == surface.Width && height == surface.Height && maximum == 255U;
        if (std::fgetc(file) != '\n')
        {
            ok = false;
        }
    }
    for (UInt32 y{}; ok && y < surface.Height; ++y)
    {
        for (UInt32 x{}; ok && x < surface.Width; ++x)
        {
            const UInt32 pixel{surface.Pixels[static_cast<Foundation::Size>(y) * surface.Stride + x]};
            const unsigned char bytes[]{static_cast<unsigned char>(pixel >> 16U),
                                        static_cast<unsigned char>(pixel >> 8U), static_cast<unsigned char>(pixel)};
            if (record)
            {
                ok = std::fwrite(bytes, 1U, sizeof(bytes), file) == sizeof(bytes);
            }
            else
            {
                unsigned char expected[3U]{};
                ok = std::fread(expected, 1U, sizeof(expected), file) == sizeof(expected) &&
                     std::memcmp(expected, bytes, sizeof(bytes)) == 0;
                if (!ok)
                {
                    static_cast<void>(std::fprintf(stderr, "%s first differing pixel (%u,%u)\n", name, x, y));
                }
            }
        }
    }
    if (!record && ok && std::fgetc(file) != EOF)
    {
        ok = false;
    }
    static_cast<void>(std::fclose(file));
    return ok;
}
} // namespace Asteria::Test::ToolkitFixture
#endif
