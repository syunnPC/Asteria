// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_MEMORY_IA64_PAGE_FAULT_DECODER_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_MEMORY_IA64_PAGE_FAULT_DECODER_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Kernel/Memory/FaultToken.hpp"

namespace Asteria::Platform::Ia64::Memory
{
enum class Ia64PageFaultVector : Foundation::UInt16
{
    AlternateDataTlb = 0x1000U,
    PageNotPresent = 0x5000U,
    KeyPermission = 0x5100U,
    InstructionAccessRights = 0x5200U,
    DataAccessRights = 0x5300U,
};

enum class Ia64PageFaultDecodeError : Foundation::UInt8
{
    None,
    UnsupportedInterruptionVector,
    UnsupportedInstructionSet,
    InvalidInstructionPointer,
    InvalidInterruptionStatus,
    InvalidAccessEncoding,
};

/// Values captured while interruption collection remains disabled. IFA is the
/// fault address, IIP is the interrupted bundle pointer, ISR describes the
/// access, and IPSR identifies the interrupted privilege and instruction set.
struct Ia64PageFaultState
{
    Foundation::UInt64 FaultAddress;
    Foundation::UInt64 InstructionPointer;
    Foundation::UInt64 InterruptionStatus;
    Foundation::UInt64 InterruptionProcessorStatus;
    Ia64PageFaultVector Vector;
};

/// Converts pager-relevant native IA-64 translation-protection faults into the
/// architecture-neutral FaultRecord. An Alternate Data TLB miss is pager
/// relevant only after the refill handler found no ledger record for it, and
/// is then a data access to a missing mapping; the instruction-TLB, VHPT,
/// dirty-bit, and access-bit vectors remain kernel-internal and are
/// deliberately rejected. A nested interruption is also rejected because its
/// IFA, IIP, and IPSR do not describe the nested access. Failure leaves output
/// unchanged.
class Ia64PageFaultDecoder final
{
public:
    [[nodiscard]] static Ia64PageFaultDecodeError Decode(const Ia64PageFaultState& state,
                                                         Kernel::Memory::FaultRecord& output) noexcept;

private:
    Ia64PageFaultDecoder() = delete;
};
} // namespace Asteria::Platform::Ia64::Memory

#endif // ASTERIA_PLATFORM_IA64_MEMORY_IA64_PAGE_FAULT_DECODER_HPP_INCLUDED
