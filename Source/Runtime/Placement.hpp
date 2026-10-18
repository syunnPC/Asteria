// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_RUNTIME_PLACEMENT_HPP_INCLUDED
#define ASTERIA_RUNTIME_PLACEMENT_HPP_INCLUDED

#include "Foundation/Types.hpp"

/// Constructs only in caller-owned, suitably aligned storage. No allocation.
void* operator new(Asteria::Foundation::Size, void* storage) noexcept;

#endif // ASTERIA_RUNTIME_PLACEMENT_HPP_INCLUDED
