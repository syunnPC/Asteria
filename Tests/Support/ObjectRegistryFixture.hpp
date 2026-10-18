// SPDX-License-Identifier: Apache-2.0
#ifndef ASTERIA_TESTS_SUPPORT_OBJECT_REGISTRY_FIXTURE_HPP_INCLUDED
#define ASTERIA_TESTS_SUPPORT_OBJECT_REGISTRY_FIXTURE_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Kernel/Object/ObjectRegistry.hpp"
#include "Platform/Synchronization/AtomicReferenceCount.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

namespace Asteria::TestSupport
{
/// An ObjectRegistry with its own entries, attached at construction, as the boot runtimes attach
/// theirs from the metadata arena or runtime page. Capacity is the hosted tests' size; the kernel's
/// registry holds ObjectRegistry::MaximumObjects.
template <Foundation::Size Capacity> class SizedObjectRegistry final : public Kernel::Object::ObjectRegistry
{
public:
    static constexpr Foundation::Size EntryCapacity{Capacity};

    explicit SizedObjectRegistry(Platform::Synchronization::SpinLock& synchronization,
                                 const Foundation::UInt32 maximumReferencesPerObject =
                                     Platform::Synchronization::AtomicReferenceCount::MaximumValue) noexcept
        : ObjectRegistry{synchronization, maximumReferencesPerObject}
    {
        static_cast<void>(AttachEntries(m_entries, Capacity));
    }

private:
    alignas(EntryAlignment) Foundation::UInt8 m_entries[Capacity * EntryBytes]{};
};

/// The registry of most hosted tests: the 512 entries the kernel's registry held before
using TestObjectRegistry = SizedObjectRegistry<512U>;
} // Namespace Asteria::TestSupport

#endif // ASTERIA_TESTS_SUPPORT_OBJECT_REGISTRY_FIXTURE_HPP_INCLUDED
