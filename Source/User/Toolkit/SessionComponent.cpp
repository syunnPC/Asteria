// SPDX-License-Identifier: Apache-2.0

#include "User/Toolkit/SessionComponent.hpp"
#include "User/Toolkit/SessionAdapter.hpp"
#include "User/Toolkit/Demo.hpp"
#include "User/Session/AddressPlan.hpp"
#include "User/Session/Decorations.hpp"
#include "User/System/DeviceAccess.hpp"
#include "User/Component/DisplayFrame.hpp"
#include "Runtime/Placement.hpp"
#include "Inventory/ReportText.hpp"

namespace Asteria::User::Toolkit
{
namespace
{
// The bootstrap grants exactly one thread. Other owners may inject a worker
// host into Application; this component never exposes cross-thread calls.
class UiThread final : public IThreadHost
{
public:
    [[nodiscard]] bool IsUiThread() noexcept override
    {
        return true;
    }
    void LockQueue() noexcept override
    {
    }
    void UnlockQueue() noexcept override
    {
    }
    void Wait(Completion&) noexcept override
    {
    }
    void Finish(Completion& completion) noexcept override
    {
        completion.Done = 1U;
    }
    void WakeUi() noexcept override
    {
    }
};
alignas(Session::ClientConnection) UInt8 g_connection[sizeof(Session::ClientConnection)]{};
alignas(SessionAdapter) UInt8 g_adapter[sizeof(SessionAdapter)]{};
alignas(Application) UInt8 g_application[sizeof(Application)]{};
alignas(DemoForm) UInt8 g_demo[sizeof(DemoForm)]{};
alignas(UiThread) UInt8 g_threadStorage[sizeof(UiThread)]{};
alignas(8) UInt8 g_frame[Session::SessionKernelFrameLimit]{};
const System::SystemCallClient* g_client{};
const UInt64 (*g_block)[Abi::ComponentBlockWords]{};
Session::ClientConnection* g_transport{};
DemoForm* g_window{};
char g_line[160U]{};
[[nodiscard, gnu::noinline]] Application* MakeApplication(UiThread& thread, SessionAdapter& adapter,
                                                          FontContext& fonts) noexcept
{
    return ::new (g_application) Application{thread, adapter, Theme::Classic(&fonts)};
}
[[nodiscard, gnu::noinline]] bool Map(const System::SystemCallClient& client, const System::CapabilityHandle space,
                                      const System::CapabilityHandle memory, const System::CapabilityHandle frame,
                                      const UInt64 address, const UInt64 pages, const UInt64 offset,
                                      const UInt8 permissions) noexcept
{
    // Map-only font tail is read-only, owned workspace writable.
    alignas(8) UInt8 request[Abi::FaultResolveRequestSizeBytes]{};
    System::StoreHandle(request + Abi::FaultResolveAddressSpaceOffset, space);
    System::StoreHandle(request + Abi::FaultResolveMemoryObjectOffset, memory);
    System::StoreHandle(request + Abi::FaultResolveFrameOffset, frame);
    System::StoreWord(request + Abi::FaultResolveVirtualStartOffset, address);
    System::StoreWord(request + Abi::FaultResolvePageCountOffset, pages);
    System::StoreWord(request + Abi::FaultResolveMemoryObjectPageOffset, offset);
    System::StoreWord(request + Abi::FaultResolveFramePageOffset, offset);
    request[Abi::FaultResolvePermissionsOffset] = permissions;
    return client.MapMemory(System::AddressOf(request)).Error == Abi::SystemCallError::None;
}
[[nodiscard, gnu::noinline]] SessionFontWorkspace* Fonts(const System::SystemCallClient& client,
                                                         const UInt64 (&block)[Abi::ComponentBlockWords]) noexcept
{
    const System::CapabilityHandle domain{block[Abi::ComponentAccountWord], block[Abi::ComponentAccountWord + 1U]};
    const System::CapabilityHandle space{block[Abi::ComponentAccountSpaceWord],
                                         block[Abi::ComponentAccountSpaceWord + 1U]};
    const UInt64 fontAddress = block[Abi::ComponentWindowStartWord] + 2U * Session::AddressPoolSpan;
    const System::CapabilityHandle fontFrame{block[Abi::ComponentUiFontFrameWord],
                                             block[Abi::ComponentUiFontFrameWord + 1U]};
    const System::CapabilityHandle fontMemory{block[Abi::ComponentUiFontMemoryWord],
                                              block[Abi::ComponentUiFontMemoryWord + 1U]};
    if (!Map(client, space, fontMemory, fontFrame, fontAddress, Abi::ComponentUiFontPages,
             block[Abi::ComponentUiFontPageWord], 0x09U))
    {
        return nullptr;
    }
    const auto frame = System::CreateRamFrame(client, domain, SessionWorkspacePages);
    const auto memory = System::CreateMemory(client, domain, Abi::ObjectTypeMemoryObject, SessionWorkspacePages);
    const UInt64 address = fontAddress + Session::AddressGranule;
    if (frame.Error != Abi::SystemCallError::None || memory.Error != Abi::SystemCallError::None ||
        !Map(client, space, {memory.Value0, memory.Value1}, {frame.Value0, frame.Value1}, address,
             SessionWorkspacePages, 0U, System::ReadWriteUser))
    {
        return nullptr;
    }
    auto* const workspace =
        ::new (reinterpret_cast<void*>(static_cast<Foundation::UIntPointer>(address))) SessionFontWorkspace{};
    const UInt64 bytes = block[Abi::ComponentUiFontRegularBytesWord];
    if (bytes > 0U && bytes <= Abi::ComponentUiFontFaceBytes &&
        workspace->Face.Open({reinterpret_cast<const UInt8*>(
                                  static_cast<Foundation::UIntPointer>(fontAddress + Abi::ComponentUiFontFaceBytes)),
                              static_cast<Size>(bytes)}) == Font::Error::None)
    {
        workspace->Context.Face = &workspace->Face;
    }
    if (workspace->Context.Cache.Configure(workspace->Cache, sizeof(workspace->Cache), workspace->Entries, 64U) !=
        Font::Error::None)
    {
        return nullptr;
    }
    return workspace;
}
void Report(const System::SystemCallClient& client, const UInt64 (&block)[Abi::ComponentBlockWords], const char* text,
            const Size length) noexcept
{
    const auto bytes = Component::WriteDisplayConsoleWrite(g_frame, 1U, reinterpret_cast<const UInt8*>(text), length);
    const System::CapabilityHandle target{block[Abi::ComponentSessionReportWord],
                                          block[Abi::ComponentSessionReportWord + 1U]};
    for (UInt32 retry{}; retry < 200U; ++retry)
    {
        const auto result = client.Send(target, System::AddressOf(g_frame), bytes).Error;
        if (result != Abi::SystemCallError::WouldBlock)
        {
            return;
        }
        g_transport->WaitUntil(g_transport->Now() + Session::ClientRetryUs / 1000U);
    }
}
void EventReport(void*, const Session::SessionEvent& event) noexcept
{
    Inventory::ReportText text{g_line, sizeof(g_line)};
    text.Text("# toolkit: event ");
    text.Decimal(static_cast<UInt64>(event.Kind));
    for (const auto word : event.Words)
    {
        text.Character(' ');
        text.Decimal(word);
    }
    text.Character('\n');
    Report(*g_client, *g_block, g_line, text.Bytes());
}
void StateReport(const char* kind) noexcept
{
    Inventory::ReportText text{g_line, sizeof(g_line)};
    text.Text("# toolkit: ");
    text.Text(kind);
    if (kind[0U] == 't')
    {
        const auto entry = g_window->Entry().UiText();
        text.Character(' ');
        for (UInt32 i{}; i < entry.Length && i < 48U; ++i)
        {
            text.Hex(static_cast<UInt8>(entry.Data[i]), 2U);
        }
    }
    else if (kind[0U] == 'c')
    {
        text.Character(' ');
        text.Decimal(g_window->Check().State().Checked ? 1U : 0U);
    }
    else if (kind[0U] == 'l')
    {
        text.Character(' ');
        text.Decimal(static_cast<UInt64>(g_window->Choices().State().Selected));
    }
    text.Character('\n');
    Report(*g_client, *g_block, g_line, text.Bytes());
}
} // Namespace
UInt64 RunSessionDemo(const System::SystemCallClient& client, const UInt64 (&block)[Abi::ComponentBlockWords],
                      const Session::ClientClock clock) noexcept
{
    const UInt64 mode = block[Abi::ComponentSessionOutputModeWord];
    if (static_cast<UInt32>(mode) > MaximumAxis || (mode >> 32U) > MaximumAxis)
    {
        return 0xC0DE02E0U;
    }
    auto* const connection = ::new (g_connection) Session::ClientConnection{client};
    if (!connection->Initialize(block, clock))
    {
        return 0xC0DE02E0U;
    }
    g_client = &client;
    g_block = &block;
    g_transport = connection;
    Inventory::ReportText storage{g_line, sizeof(g_line)};
    storage.Text("# toolkit: pools ");
    storage.Decimal(static_cast<UInt32>(mode));
    storage.Character('x');
    storage.Decimal(mode >> 32U);
    storage.Text(" pages=");
    storage.Decimal(connection->PoolPages());
    storage.Text(" frames=");
    storage.Decimal(connection->PoolFrames(0U) + connection->PoolFrames(1U));
    storage.Text(" workspace-pages=");
    storage.Decimal(SessionWorkspacePages);
    storage.Character('\n');
    Report(client, block, g_line, storage.Bytes());
    auto* const fonts = Fonts(client, block);
    if (fonts == nullptr)
    {
        return 0xC0DE02E1U;
    }
    auto* const adapter = ::new (g_adapter) SessionAdapter{*connection};
    auto* const thread = ::new (g_threadStorage) UiThread{};
    auto* const application = MakeApplication(*thread, *adapter, fonts->Context);
    auto* const demo = ::new (g_demo) DemoForm{*application};
    const auto view = connection->Image(0U);
    if (view.Width <= Session::FrameExtraWidth || view.Height <= Session::FrameExtraHeight)
    {
        return 0xC0DE02E2U;
    }
    const UInt32 width = Min(DemoInitialWidth, view.Width - Session::FrameExtraWidth);
    const UInt32 height = Min(DemoInitialHeight, view.Height - Session::FrameExtraHeight);
    if (adapter->Bind(demo->Window(), width, height) != Status::Ok ||
        demo->Build(DemoColumns, DemoCellWidth, DemoRowHeight) != Status::Ok)
    {
        return 0xC0DE02E2U;
    }
    g_window = demo;
    static_cast<void>(demo->ActionButton().On(EventKind::Click,
                                              [](Control&, Event&) noexcept
                                              {
                                                  static_cast<void>(g_window->ActionButton().SetText(Utf8("Clicked")));
                                                  StateReport("button");
                                              }));
    static_cast<void>(demo->Entry().On(EventKind::TextChanged,
                                       [](Control&, Event&) noexcept
                                       {
                                           StateReport("text");
                                       }));
    static_cast<void>(demo->Check().On(EventKind::CheckedChanged,
                                       [](Control&, Event&) noexcept
                                       {
                                           StateReport("check");
                                       }));
    static_cast<void>(demo->Choices().On(EventKind::SelectedIndexChanged,
                                         [](Control&, Event&) noexcept
                                         {
                                             StateReport("list");
                                         }));
    adapter->Observe(nullptr, EventReport);
    constexpr char vegur[]{"# toolkit: font Vegur-Regular\n"};
    constexpr char fallback[]{"# toolkit: font Bold8x16-fallback\n"};
    Report(client, block, fonts->Context.Face != nullptr ? vegur : fallback,
           fonts->Context.Face != nullptr ? sizeof(vegur) - 1U : sizeof(fallback) - 1U);
    constexpr char ready[]{"# toolkit: ready\n"};
    Report(client, block, ready, sizeof(ready) - 1U);
    const auto result = application->Run();
    constexpr char closed[]{"# toolkit: closed\n"};
    Report(client, block, closed, sizeof(closed) - 1U);
    return result == Status::Ok && adapter->Healthy() ? 0U : 0xC0DE02E3U;
}
} // Namespace Asteria::User::Toolkit
