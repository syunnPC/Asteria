// SPDX-License-Identifier: Apache-2.0

#include "Runtime/Placement.hpp"

/// The bounded owners have already reserved aligned storage. This language
/// adapter constructs there without allocating or retaining a pointer.
void* operator new(Asteria::Foundation::Size, void* const storage) noexcept
{
    return storage;
}
