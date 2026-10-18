// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_ABI_PAGER_PROTOCOL_HPP_INCLUDED
#define ASTERIA_ABI_PAGER_PROTOCOL_HPP_INCLUDED
#include "Foundation/Types.hpp"
namespace Asteria::Abi
{
// Fault record, creator tag, kernel domain identity.
inline constexpr Foundation::UInt16 PagerProtocolMajor{1U};
inline constexpr Foundation::UInt16 PagerProtocolMinor{2U};
inline constexpr Foundation::UInt16 PagerFaultRecordBytes{32U};
inline constexpr Foundation::Size PagerFaultTagOffset{PagerFaultRecordBytes};
inline constexpr Foundation::Size PagerFaultIdentityOffset{PagerFaultTagOffset + 8U};
inline constexpr Foundation::UInt16 PagerFaultPayloadBytes{48U};
static_assert(PagerFaultPayloadBytes == PagerFaultIdentityOffset + 8U);
}
#endif
