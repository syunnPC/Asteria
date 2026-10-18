// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_DIAGNOSTICS_APPLICATION_LAUNCH_EVIDENCE_HPP_INCLUDED
#define ASTERIA_KERNEL_DIAGNOSTICS_APPLICATION_LAUNCH_EVIDENCE_HPP_INCLUDED
#include "Foundation/Types.hpp"
#include "Abi/ObjectCreationLimits.hpp"
namespace Asteria::Kernel::Diagnostics
{
using Abi::LimitedObjectCount;
using Abi::FaultChannelMaximumCount;

// Counters persist after a channel incarnation is collected.
struct ApplicationLaunchEvidence
{
    Foundation::UInt64 Published;
    Foundation::UInt64 Created;
    Foundation::UInt64 Destroyed;
    Foundation::UInt64 Reclaimed;
    Foundation::UInt64 Live;
    Foundation::UInt64 TagRefusals;
    Foundation::UInt64 CodeFlagRefusals;
    Foundation::UInt64 TypeLimitRefusals[LimitedObjectCount];
    Foundation::UInt64 ChannelIdentity[FaultChannelMaximumCount];
    Foundation::UInt64 Deliveries[FaultChannelMaximumCount];
    Foundation::UInt64 Resolutions[FaultChannelMaximumCount];
    Foundation::UInt64 Terminations[FaultChannelMaximumCount];
    Foundation::UInt64 Busy[FaultChannelMaximumCount];
};
static_assert(__builtin_offsetof(ApplicationLaunchEvidence, Published) == 0U);
static_assert(__builtin_offsetof(ApplicationLaunchEvidence, Created) == 8U);
static_assert(__builtin_offsetof(ApplicationLaunchEvidence, Destroyed) == 16U);
static_assert(__builtin_offsetof(ApplicationLaunchEvidence, Reclaimed) == 24U);
static_assert(__builtin_offsetof(ApplicationLaunchEvidence, Live) == 32U);
static_assert(__builtin_offsetof(ApplicationLaunchEvidence, TagRefusals) == 40U);
static_assert(__builtin_offsetof(ApplicationLaunchEvidence, CodeFlagRefusals) == 48U);
static_assert(__builtin_offsetof(ApplicationLaunchEvidence, TypeLimitRefusals) == 56U);
static_assert(__builtin_offsetof(ApplicationLaunchEvidence, ChannelIdentity) == 120U);
static_assert(__builtin_offsetof(ApplicationLaunchEvidence, Deliveries) == 152U);
static_assert(__builtin_offsetof(ApplicationLaunchEvidence, Resolutions) == 184U);
static_assert(__builtin_offsetof(ApplicationLaunchEvidence, Terminations) == 216U);
static_assert(__builtin_offsetof(ApplicationLaunchEvidence, Busy) == 248U);
static_assert(sizeof(ApplicationLaunchEvidence) == 280U);
static_assert(sizeof(ApplicationLaunchEvidence) ==
              sizeof(Foundation::UInt64) * (7U + Abi::LimitedObjectCount + 5U * Abi::FaultChannelMaximumCount));
}
#endif
