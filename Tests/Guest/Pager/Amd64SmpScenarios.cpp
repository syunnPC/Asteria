// SPDX-License-Identifier: Apache-2.0
#include "Guest/Pager/Amd64SmpScenarios.hpp"
#include "Guest/Pager/Amd64SmpProtocol.h"
#include "Abi/PagerProtocol.hpp"
#include "User/Pager/PagerWords.hpp"
#include "User/Loader/ExecutableFormats.hpp"
#include "Inventory/ReportText.hpp"

#if defined(ASTERIA_TARGET_AMD64)
extern "C" const Asteria::Foundation::UInt8 AsteriaAmd64SmpImageBytes[];
extern "C" const Asteria::Foundation::UInt8 AsteriaAmd64SmpImageEnd[];
namespace Asteria::User::Pager
{
namespace
{
constexpr Foundation::Size Workers{ASTERIA_SMP_WORKERS};
constexpr Foundation::UInt64 AllWorkers{(1ULL << Workers) - 1U};
constexpr Foundation::UInt64 PageBytes{4096U};
constexpr Foundation::UInt8 Permissions{0x0BU};
constexpr Foundation::UInt64 Tag{ASTERIA_SMP_TAG};
constexpr Foundation::Size TokenOffset{48U + 88U};
constexpr Foundation::Size FaultOffset{TokenOffset + 32U};
constexpr Foundation::UInt64 MaximumWaits{4000U};
constinit LoadedImage g_image{};
constinit System::CapabilityHandle g_threads[Workers]{};
constinit ComponentMapping g_pages{};
constinit ComponentRecord* g_component{nullptr};
constinit volatile Foundation::UInt64* g_control{nullptr};
constinit System::CapabilityHandle g_channel{};
constinit System::CapabilityHandle g_wait{};
constinit char g_resolutionLine[160U]{};
static_assert(Workers * 256U <= Abi::ComponentStackPages * PageBytes);
static_assert(static_cast<Foundation::UInt64>(Abi::SystemCallNumber::ThreadExit) == 15U);

[[nodiscard]] bool Pause(ScenarioContext& context, const Foundation::UInt64 microseconds,
                         Abi::SystemCallError& error) noexcept
{
    const auto waited = context.Client->Wait(g_wait, microseconds);
    return waited.Error == Abi::SystemCallError::TimedOut || Check(waited, error);
}

// Keep launch construction separate from mapping/teardown at O0 too;
// both frames fit the target's 512-byte budget.
[[nodiscard]] bool StartWorkersDomain(ScenarioContext& context, Abi::SystemCallError& error) noexcept
{
    g_component = context.Supervisor->Allocate();
    const ComponentBuild build{.Image = &g_image,
                               .Tag = Tag,
                               .Behavior = Abi::ComponentBehaviorReport,
                               .Foreign = {},
                               .Report = context.Report,
                               .Directory = {},
                               .DriverService = {},
                               .DriverGeneration = 0U,
                               .ServiceFlags = 0U,
                               .Deferred = true,
                               .FaultChannel = g_channel};
    return g_component != nullptr && context.Supervisor->Start(*g_component, build, error);
}

[[nodiscard]] bool CreateWorkers(ScenarioContext& context, Abi::SystemCallError& error) noexcept
{
    for (Foundation::Size index{0U}; index < Workers; ++index)
    {
        context.Calls->ClearRequest();
        auto* const request = context.Calls->GetRequest();
        StoreHandle(request + Abi::ObjectCreateThreadDomainOffset, g_component->Domain);
        StoreWord(request + Abi::ObjectCreateThreadEntryOffset, g_image.Layout.Entry);
        StoreWord(request + Abi::ObjectCreateThreadStackOffset, Abi::ComponentStackStart + (index + 1U) * 256U);
        StoreWord(request + Abi::ObjectCreateThreadArgumentOffset, index);
        if (!context.Calls->Create(context.Management, Abi::ObjectTypeThread, Abi::ObjectCreateThreadRequestSizeBytes,
                                   g_threads[index], error))
        {
            return false;
        }
    }
    g_component->Thread = g_threads[0U];
    g_component->State = ComponentState::Running;
    for (Foundation::UInt64 attempt{0U}; attempt < MaximumWaits; ++attempt)
    {
        if (__atomic_load_n(g_control + ASTERIA_SMP_STARTED / 8U, __ATOMIC_ACQUIRE) == AllWorkers)
        {
            // Every worker spins through multiple slices before its faults.
            return Pause(context, 100'000U, error);
        }
        if (!Pause(context, 1000U, error))
        {
            return false;
        }
    }
    return false;
}

[[nodiscard]] bool Resolve(ScenarioContext& context, const Foundation::UInt64 address,
                           const System::CapabilityHandle token, Abi::SystemCallError& error) noexcept
{
    context.Calls->ClearRequest();
    auto* const request = context.Calls->GetRequest();
    StoreHandle(request + Abi::FaultResolveAddressSpaceOffset, g_component->Space);
    StoreHandle(request + Abi::FaultResolveMemoryObjectOffset, g_pages.Memory);
    StoreHandle(request + Abi::FaultResolveFrameOffset, g_pages.Frame);
    StoreWord(request + Abi::FaultResolveVirtualStartOffset, address);
    StoreWord(request + Abi::FaultResolvePageCountOffset, 1U);
    const auto page = (address - Abi::ComponentWindowStart) / PageBytes;
    StoreWord(request + Abi::FaultResolveMemoryObjectPageOffset, page);
    StoreWord(request + Abi::FaultResolveFramePageOffset, page);
    request[Abi::FaultResolvePermissionsOffset] = Permissions;
    Abi::SystemCallResponse resolved{};
    for (Foundation::UInt64 attempt{0U}; attempt < MaximumWaits; ++attempt)
    {
        // Processor scheduling: another home may still own a transition receipt for this shared
        // root. A refused map retains its token; yield so that home can finish before retrying it.
        resolved = context.Client->FaultResolve(token, Abi::FaultResolveKind::Map, AddressOf(request));
        if (resolved.Error != Abi::SystemCallError::WouldBlock)
        {
            break;
        }
        if (!Pause(context, 1000U, error))
        {
            return false;
        }
    }
    if (!Check(resolved, error))
    {
        Inventory::ReportText text{g_resolutionLine, sizeof(g_resolutionLine)};
        text.Text("# smp: rejected resolution ");
        text.Decimal(static_cast<Foundation::UInt64>(resolved.Error));
        const System::CapabilityHandle handles[]{g_component->Space, g_pages.Memory, g_pages.Frame};
        for (const auto handle : handles)
        {
            const auto queried = context.Client->QueryCapability(handle);
            text.Character(' ');
            text.Decimal(static_cast<Foundation::UInt64>(queried.Error));
            text.Character(':');
            text.Decimal(queried.Value0);
        }
        text.Character('\n');
        WriteLine(context, g_resolutionLine, text.Bytes());
        return false;
    }
    return context.Calls->Close(token, error);
}

[[nodiscard]] bool ServeFaults(ScenarioContext& context, const bool unmap, Abi::SystemCallError& error) noexcept
{
    Foundation::UInt64 delivered{0U};
    const Foundation::Size readyWord = (unmap ? ASTERIA_SMP_AFTER_UNMAP : ASTERIA_SMP_READ) / 8U;
    Foundation::UInt64 resolvedPages{0U};
    for (Foundation::UInt64 attempt{0U}; attempt < MaximumWaits; ++attempt)
    {
        if (__atomic_load_n(g_control + readyWord, __ATOMIC_ACQUIRE) == AllWorkers)
        {
            return unmap ? delivered > 0U : delivered == Workers && resolvedPages == AllWorkers;
        }
        const auto waited = context.Client->Wait(g_wait, 10'000U);
        if (waited.Error == Abi::SystemCallError::TimedOut)
        {
            continue;
        }
        if (!Check(waited, error))
        {
            return false;
        }
        const auto received = context.Client->Receive(g_channel, AddressOf(context.Delivery), sizeof(context.Delivery));
        if (received.Error == Abi::SystemCallError::WouldBlock)
        {
            continue;
        }
        if (!Check(received, error) || received.Value1 != FaultOffset + Abi::PagerFaultPayloadBytes)
        {
            return false;
        }
        const auto* const fault = context.Delivery + FaultOffset;
        const auto address = LoadWord(fault);
        if (LoadWord(fault + Abi::PagerFaultTagOffset) != Tag ||
            LoadWord(fault + Abi::PagerFaultIdentityOffset) == 0U || fault[24U] != 1U || fault[25U] != 1U ||
            (unmap ? address != Abi::ComponentWindowStart
                   : address <= Abi::ComponentWindowStart || address > Abi::ComponentWindowStart + Workers * PageBytes))
        {
            return false;
        }
        if (!unmap)
        {
            const auto bit = 1ULL << ((address - Abi::ComponentWindowStart) / PageBytes - 1U);
            if ((resolvedPages & bit) != 0U)
            {
                return false;
            }
            resolvedPages |= bit;
        }
        // Hold one token while other processors defer behind it, including
        // two continuations on the same home. Receipts must belong to threads.
        if (delivered == 0U && !Pause(context, 100'000U, error))
        {
            return false;
        }
        const System::CapabilityHandle token{LoadWord(context.Delivery + TokenOffset),
                                             LoadWord(context.Delivery + TokenOffset + 8U)};
        if (!Resolve(context, address, token, error))
        {
            return false;
        }
        ++delivered;
    }
    return false;
}

[[nodiscard]] bool ReapWorkers(ScenarioContext& context, Abi::SystemCallError& error) noexcept
{
    __atomic_store_n(g_control + ASTERIA_SMP_PHASE / 8U, 3U, __ATOMIC_RELEASE);
    for (Foundation::Size index{1U}; index < Workers; ++index)
    {
        bool reaped{false};
        for (Foundation::UInt64 attempt{0U}; attempt < MaximumWaits && !reaped; ++attempt)
        {
            const auto destroyed = context.Client->DestroyObject(g_threads[index], Abi::ObjectTypeThread);
            if (destroyed.Error == Abi::SystemCallError::WouldBlock)
            {
                if (!Pause(context, 1000U, error))
                {
                    return false;
                }
                continue;
            }
            if (!Check(destroyed, error) || destroyed.Value0 != ASTERIA_SMP_EXIT ||
                !context.Calls->Close(g_threads[index], error))
            {
                return false;
            }
            reaped = true;
        }
        if (!reaped)
        {
            return false;
        }
    }
    return context.Supervisor->Supervise(*g_component, error) == PagerServeError::None &&
           g_component->ExitCode == ASTERIA_SMP_EXIT;
}
} // Namespace

bool RunAmd64SmpScenarios(ScenarioContext& context, Abi::SystemCallError& error) noexcept
{
    Foundation::UInt32 a{1U};
    Foundation::UInt32 b{0U};
    Foundation::UInt32 c{0U};
    Foundation::UInt32 d{0U};
    __asm__ __volatile__("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    if (((b >> 16U) & 0xFFU) < 4U)
    {
        return true;
    }
    g_image.Bytes = AsteriaAmd64SmpImageBytes;
    g_image.Size = static_cast<Foundation::Size>(AsteriaAmd64SmpImageEnd - AsteriaAmd64SmpImageBytes);
    if (Loader::DescribeExecutable(g_image.Bytes, g_image.Size, context.Launch->Component.Image, g_image.Layout) !=
        Loader::LoaderError::None)
    {
        return false;
    }
    context.Calls->ClearRequest();
    if (!context.Calls->Create(context.Management, Abi::ObjectTypeFaultChannel,
                               Abi::ObjectCreateFaultChannelRequestSizeBytes, g_channel, error) ||
        !context.Calls->Create(context.Management, Abi::ObjectTypeWaitSet, Abi::ObjectCreateWaitSetRequestSizeBytes,
                               g_wait, error) ||
        !Check(context.Client->BindWaitSet(g_wait, g_channel, Tag), error))
    {
        return false;
    }
    if (!StartWorkersDomain(context, error))
    {
        return false;
    }
    const ComponentMapping* controlMapping{nullptr};
    for (Foundation::Size index{0U}; index < g_image.Layout.SegmentCount; ++index)
    {
        if (g_image.Layout.Segments[index].Writable)
        {
            controlMapping = &g_component->Mappings[index];
        }
    }
    if (controlMapping == nullptr ||
        !context.Calls->Map(context.OwnSpace, *controlMapping, context.WindowStart, Permissions, error))
    {
        return false;
    }
    g_control =
        reinterpret_cast<volatile Foundation::UInt64*>(static_cast<Foundation::UIntPointer>(context.WindowStart));
    g_control[ASTERIA_SMP_WINDOW / 8U] = Abi::ComponentWindowStart;
    g_pages = {.Frame = {}, .Memory = {}, .VirtualStart = Abi::ComponentWindowStart, .Pages = Workers + 1U};
    if (!context.Calls->CreateMemory(context.Management, Abi::ObjectTypeFrame, g_pages.Pages, 0U, g_pages.Frame,
                                     error) ||
        !context.Calls->CreateMemory(context.Management, Abi::ObjectTypeMemoryObject, g_pages.Pages, Permissions,
                                     g_pages.Memory, error))
    {
        return false;
    }
    const ComponentMapping shared{g_pages.Frame, g_pages.Memory, g_pages.VirtualStart, 1U};
    if (!context.Calls->Map(g_component->Space, shared, shared.VirtualStart, Permissions, error) ||
        !CreateWorkers(context, error))
    {
        return false;
    }
    __atomic_store_n(g_control + ASTERIA_SMP_PHASE / 8U, 1U, __ATOMIC_RELEASE);
    if (!ServeFaults(context, false, error) ||
        !Check(context.Client->UnmapMemory(g_component->Space, shared.VirtualStart, 1U), error))
    {
        return false;
    }
    __atomic_store_n(g_control + ASTERIA_SMP_PHASE / 8U, 2U, __ATOMIC_RELEASE);
    if (!ServeFaults(context, true, error) || !ReapWorkers(context, error) ||
        !Check(context.Client->UnmapMemory(context.OwnSpace, context.WindowStart, controlMapping->Pages), error) ||
        !context.Supervisor->Teardown(*g_component, error) ||
        !context.Calls->Destroy(g_pages.Memory, Abi::ObjectTypeMemoryObject, error) ||
        !context.Calls->Destroy(g_pages.Frame, Abi::ObjectTypeFrame, error) ||
        !context.Calls->Destroy(g_wait, Abi::ObjectTypeWaitSet, error) ||
        !context.Calls->Destroy(g_channel, Abi::ObjectTypeFaultChannel, error))
    {
        return false;
    }
    constexpr char line[]{
        "# smp: spinners preempted, four homes faulted and resolved, sibling read faulted after unmap, drained\n"};
    WriteLine(context, line, sizeof(line) - 1U);
    return true;
}
} // Namespace Asteria::User::Pager
#else
namespace Asteria::User::Pager
{
bool RunAmd64SmpScenarios(ScenarioContext&, Abi::SystemCallError&) noexcept
{
    return true;
}
} // Namespace Asteria::User::Pager
#endif
