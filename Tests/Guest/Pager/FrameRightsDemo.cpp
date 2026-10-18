// SPDX-License-Identifier: Apache-2.0
#include "Guest/Pager/TestRootService.hpp"

#include "User/Pager/PagerWords.hpp"
#include "User/System/DeviceAccess.hpp"

namespace Asteria::User::Pager
{
// A writable MemoryObject of the consumer's own cannot restore Write to an attenuated Frame. Use
// one page of the existing staging window, and release every mapping and handle before image
// loading.
bool TestRootService::RunFrameRights(Abi::SystemCallError& error) noexcept
{
    ComponentMapping mapping{};
    mapping.Pages = 1U;
    if (!m_calls.CreateMemory(m_bootstrap->Management, Abi::ObjectTypeFrame, mapping.Pages, 0U, mapping.Frame, error) ||
        !m_calls.CreateMemory(m_bootstrap->Management, Abi::ObjectTypeMemoryObject, mapping.Pages,
                              System::ReadWriteUser, mapping.Memory, error) ||
        !m_calls.Map(m_bootstrap->OwnSpace, mapping, m_bootstrap->WindowStart, System::ReadWriteUser, error))
    {
        return false;
    }
    volatile Foundation::UInt64* const page =
        reinterpret_cast<volatile Foundation::UInt64*>(static_cast<Foundation::UIntPointer>(m_bootstrap->WindowStart));
    page[0U] = Abi::MappedPagePattern;
    if (!Check(m_context.Client->UnmapMemory(m_bootstrap->OwnSpace, m_bootstrap->WindowStart, mapping.Pages), error))
    {
        return false;
    }
    const auto derived =
        m_context.Client->DeriveCapability(mapping.Frame, Abi::CapabilityRightMap, ~Foundation::UInt64{0U});
    if (!Check(derived, error))
    {
        return false;
    }
    const System::CapabilityHandle owner = mapping.Frame;
    mapping.Frame = {derived.Value0, derived.Value1};
    if (m_calls.Map(m_bootstrap->OwnSpace, mapping, m_bootstrap->WindowStart, System::ReadWriteUser, error) ||
        (error != Abi::SystemCallError::RightsDenied))
    {
        return false;
    }
    error = Abi::SystemCallError::None;
    constexpr Foundation::UInt8 ReadUser{0x09U};
    if (!m_calls.Map(m_bootstrap->OwnSpace, mapping, m_bootstrap->WindowStart, ReadUser, error) ||
        (page[0U] != Abi::MappedPagePattern) ||
        !Check(m_context.Client->UnmapMemory(m_bootstrap->OwnSpace, m_bootstrap->WindowStart, mapping.Pages), error) ||
        !m_calls.Close(mapping.Frame, error) || !m_calls.Destroy(mapping.Memory, Abi::ObjectTypeMemoryObject, error) ||
        !m_calls.Destroy(owner, Abi::ObjectTypeFrame, error))
    {
        return false;
    }
    constexpr char Passed[] = "# frame rights: Map-only copy read, writable mapping refused, reclaimed\n";
    return Check(m_context.Client->DebugWrite(m_bootstrap->Diagnostic, AddressOf(Passed), sizeof(Passed) - 1U), error);
}
} // Namespace Asteria::User::Pager
