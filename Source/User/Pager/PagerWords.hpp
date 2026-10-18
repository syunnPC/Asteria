// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_PAGER_PAGER_WORDS_HPP_INCLUDED
#define ASTERIA_USER_PAGER_PAGER_WORDS_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "User/System/ObjectRequests.hpp"

namespace Asteria::User::Pager
{
/// Little-endian words and handles in the request and delivery buffers the
/// pager and its root service exchange with the kernel.
using System::AddressOf;
using System::LoadWord;
using System::StoreHandle;
using System::StoreWord;

/// Borrow the kernel's immutable module slice, or a hosted caller's fake.
/// The start-block decoder has checked extent and alignment; lifetime is the run.
[[nodiscard]] inline const Foundation::UInt8* ModuleTableBytes(const Foundation::UInt64 base) noexcept
{
    return reinterpret_cast<const Foundation::UInt8*>(static_cast<Foundation::UIntPointer>(base));
}
} // namespace Asteria::User::Pager

#endif // ASTERIA_USER_PAGER_PAGER_WORDS_HPP_INCLUDED
