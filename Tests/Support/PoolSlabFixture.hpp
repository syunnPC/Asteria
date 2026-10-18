// SPDX-License-Identifier: Apache-2.0
#ifndef ASTERIA_TESTS_SUPPORT_POOL_SLAB_FIXTURE_HPP_INCLUDED
#define ASTERIA_TESTS_SUPPORT_POOL_SLAB_FIXTURE_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Kernel/Memory/FrameObjectPool.hpp"
#include "Kernel/Memory/MemoryObjectPool.hpp"

namespace Asteria::TestSupport
{
/// The Frame and MemoryObject slabs a boot runtime places in its metadata, for a hosted pool or
/// ObjectManager to attach. Too large for a test function's stack: fixtures holding it live in
/// static or heap storage.
struct PoolSlabs
{
    alignas(Kernel::Memory::FrameObjectPool::SlabAlignment)
        Foundation::UInt8 Frames[Kernel::Memory::FrameObjectPool::SlabBytes]{};
    alignas(Kernel::Memory::MemoryObjectPool::SlabAlignment)
        Foundation::UInt8 MemoryObjects[Kernel::Memory::MemoryObjectPool::SlabBytes]{};
};
} // Namespace Asteria::TestSupport

#endif // ASTERIA_TESTS_SUPPORT_POOL_SLAB_FIXTURE_HPP_INCLUDED
