// SPDX-License-Identifier: Apache-2.0
#include "Guest/SessionProbe.hpp"
#include "Abi/ComponentBootstrap.hpp"
#include "User/Session/SessionFaceFrame.hpp"
#include "User/Session/Palette.hpp"
#include "User/Graphics/Blit.hpp"
#include "User/System/DeviceAccess.hpp"
#if defined(ASTERIA_TARGET_IA64)
#include "User/Platform/Ia64/System/Ia64SystemCallTransport.hpp"
#else
#include "User/Platform/Amd64/System/Amd64SystemCallTransport.hpp"
#endif
namespace Asteria::Test
{
namespace
{
namespace Component = Asteria::User::Component;
namespace Graphics = Asteria::User::Graphics;
namespace Session = Asteria::User::Session;
namespace System = Asteria::User::System;
constinit Session::SessionFaceMessage g_message{};
constinit Session::SessionFaceMessage g_answer{};
alignas(8) constinit Foundation::UInt8
    g_delivery[Component::EchoDeliveryHeaderBytes + Component::EchoMaximumFrameBytes]{};
alignas(8) constinit Foundation::UInt8 g_frame[Component::EchoMaximumFrameBytes]{};
constinit Foundation::UInt64 g_words[32U]{};
bool g_stamped{false};
[[nodiscard]] bool Snapshot(const Session::SessionFaceMessage& message) noexcept
{
    if (message.Method != Session::FaceSnapshots || message.Words[1U] != 1U || message.Words[4U] != 2U ||
        message.Words[5U] != (96U | (72ULL << 32U)) || message.Words[7U] != 42U)
    {
        return false;
    }
    g_stamped = true;
    return true;
}
[[nodiscard]] bool Send(const System::SystemCallClient& client, const System::CapabilityHandle target,
                        const Foundation::UInt32 method, const Foundation::UInt64 transaction,
                        const System::CapabilityHandle* handles, const Foundation::Size count) noexcept
{
    const auto bytes = Session::WriteSessionFaceFrame(g_frame, method, transaction, g_words, handles, count);
    return bytes != 0U && client.Send(target, System::AddressOf(g_frame), bytes).Error == Abi::SystemCallError::None;
}
[[nodiscard]] Foundation::UInt64 Run(const System::SystemCallClient& client,
                                     const Foundation::UInt64 (&block)[Abi::ComponentBlockWords]) noexcept
{
    const System::CapabilityHandle domain{block[Abi::ComponentAccountWord], block[Abi::ComponentAccountWord + 1U]};
    const System::CapabilityHandle space{block[Abi::ComponentAccountSpaceWord],
                                         block[Abi::ComponentAccountSpaceWord + 1U]};
    const System::CapabilityHandle face{block[Abi::ComponentInputServiceWord],
                                        block[Abi::ComponentInputServiceWord + 1U]};
    const System::CapabilityHandle reply{block[Abi::ComponentReplyWord], block[Abi::ComponentReplyWord + 1U]};
    const System::CapabilityHandle receive{block[Abi::ComponentReceiveWord], block[Abi::ComponentReceiveWord + 1U]};
    const System::CapabilityHandle waitSet{block[Abi::ComponentWaitSetWord], block[Abi::ComponentWaitSetWord + 1U]};
    const auto epoch = block[Abi::ComponentInputEpochWord];
    const auto address = block[Abi::ComponentWindowStartWord];
    const auto frame = System::CreateMemory(client, domain, Abi::ObjectTypeFrame, ProbePages);
    const auto memory = System::CreateMemory(client, domain, Abi::ObjectTypeMemoryObject, ProbePages);
    if (frame.Error != Abi::SystemCallError::None || memory.Error != Abi::SystemCallError::None ||
        System::MapWindow(client, space, {memory.Value0, memory.Value1}, {frame.Value0, frame.Value1}, address,
                          ProbePages) != Abi::SystemCallError::None)
    {
        return 0xC0DE00F0U;
    }
    const auto format = static_cast<Graphics::PixelFormat>(block[Abi::ComponentMountFlagsWord]);
    Graphics::MemorySurface output{
        .Pixels = reinterpret_cast<Foundation::UInt32*>(static_cast<Foundation::UIntPointer>(address)),
        .Width = ProbeWidth,
        .Height = ProbeHeight,
        .Stride = ProbeWidth,
        .Format = format};
    g_words[0U] = epoch;
    g_words[1U] = ProbePages;
    g_words[2U] = 1U;
    g_words[4U] = ProbeWidth | (static_cast<Foundation::UInt64>(ProbeHeight) << 32U);
    g_words[5U] = ProbeWidth;
    g_words[6U] = static_cast<Foundation::UInt64>(format);
    g_words[8U] = ProbePages;
    const System::CapabilityHandle pixels{frame.Value0, frame.Value1};
    if (!Send(client, face, Session::FaceCreateSurface, 1U, &pixels, 1U))
    {
        return 0xC0DE00F1U;
    }
    for (auto& word : g_words)
    {
        word = 0U;
    }
    g_words[0U] = epoch;
    g_words[1U] = 7U;
    if (!Send(client, face, Session::FaceCredit, 2U, &reply, 1U))
    {
        return 0xC0DE00F8U;
    }
    for (;;)
    {
        if (client.Wait(waitSet, 0U).Error != Abi::SystemCallError::None)
        {
            return 0xC0DE00F2U;
        }
        const auto received = client.Receive(receive, System::AddressOf(g_delivery), sizeof(g_delivery));
        if (received.Error == Abi::SystemCallError::WouldBlock)
        {
            continue;
        }
        if (received.Error != Abi::SystemCallError::None ||
            !Session::ReadSessionFaceFrame(g_delivery, static_cast<Foundation::Size>(received.Value1), g_message))
        {
            return 0xC0DE00F3U;
        }
        if (g_message.Method == Session::FaceSnapshots)
        {
            if (!Snapshot(g_message))
            {
                return 0xC0DE00F9U;
            }
            continue;
        }
        if (g_message.Method != Session::FacePresent)
        {
            return 0xC0DE00F3U;
        }
        const auto serial = g_message.Words[1U];
        // One complete, deterministic image. Window pixels differ from the
        // Desktop and exercise both direct-copy and cursor-overlap tiles.
        Graphics::FillRect(output, Graphics::Bounds(output), Session::Desktop);
        for (Foundation::UInt32 y{0U}; y < ProbeWindowHeight; ++y)
        {
            for (Foundation::UInt32 x{0U}; x < ProbeWindowWidth; ++x)
            {
                const Graphics::Color color{.Red = static_cast<Foundation::UInt8>(x + serial * 17U),
                                            .Green = static_cast<Foundation::UInt8>(y * 2U + serial * 11U),
                                            .Blue = static_cast<Foundation::UInt8>((x ^ y) + serial * 23U)};
                output.Pixels[(static_cast<Foundation::Size>(y) + 32U) * ProbeWidth + x + 48U] =
                    Graphics::PackColor(format, color);
            }
        }
        for (auto& word : g_words)
        {
            word = 0U;
        }
        g_words[0U] = epoch;
        g_words[1U] = serial;
        g_words[2U] = 1U;
        g_words[4U] = Session::PackSessionDamage(0U, 0U, ProbeWidth, ProbeHeight);
        if (!Send(client, face, Session::FacePresent, serial + 10U, &reply, 1U))
        {
            return 0xC0DE00F4U;
        }
        for (;;)
        {
            if (client.Wait(waitSet, 0U).Error != Abi::SystemCallError::None)
            {
                return 0xC0DE00F5U;
            }
            const auto answer = client.Receive(receive, System::AddressOf(g_delivery), sizeof(g_delivery));
            if (answer.Error == Abi::SystemCallError::WouldBlock)
            {
                continue;
            }
            if (answer.Error != Abi::SystemCallError::None ||
                !Session::ReadSessionFaceFrame(g_delivery, static_cast<Foundation::Size>(answer.Value1), g_answer))
            {
                return 0xC0DE00F6U;
            }
            if (g_answer.Method == Session::FaceSnapshots)
            {
                if (!Snapshot(g_answer))
                {
                    return 0xC0DE00F9U;
                }
                continue;
            }
            if (g_answer.Method != Component::DisplayReplyMethod || g_answer.Words[1U] != Session::SessionPresented)
            {
                return 0xC0DE00F6U;
            }
            break;
        }
        if (serial == 2U && !g_stamped)
        {
            return 0xC0DE00FAU;
        }
        for (Foundation::Size i{0U}; i < 10U; ++i)
        {
            g_words[i] = g_answer.Words[i];
        }
        g_words[0U] = g_message.Transaction;
        if (!Send(client, g_message.Attachments[0U], Component::DisplayReplyMethod, g_message.Transaction, nullptr, 0U))
        {
            return 0xC0DE00F7U;
        }
        static_cast<void>(client.CloseCapability(g_message.Attachments[0U]));
    }
}
} // namespace
} // namespace Asteria::Test
#if defined(ASTERIA_TARGET_IA64)
extern "C" void AsteriaIa64ComponentImageMain(const Asteria::Foundation::UInt64 address) noexcept
#else
extern "C" void AsteriaAmd64ComponentImageMain(const Asteria::Foundation::UInt64 address) noexcept
#endif
{
    namespace Abi = Asteria::Abi;
    namespace Foundation = Asteria::Foundation;
    namespace Test = Asteria::Test;
    namespace User = Asteria::User;
#if defined(ASTERIA_TARGET_IA64)
    const User::System::SystemCallClient client{User::Platform::Ia64::System::GetIa64SystemCallTransport()};
#else
    const User::System::SystemCallClient client{User::Platform::Amd64::System::GetAmd64SystemCallTransport()};
#endif
    const auto& block = *reinterpret_cast<const Foundation::UInt64(*)[Abi::ComponentBlockWords]>(
        static_cast<Foundation::UIntPointer>(address));
    const auto code = Test::Run(client, block);
    for (;;)
    {
        static_cast<void>(client.ExitThread(code));
    }
}
