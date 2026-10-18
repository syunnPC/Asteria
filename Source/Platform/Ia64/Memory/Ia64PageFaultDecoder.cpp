// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Memory/Ia64PageFaultDecoder.hpp"

namespace Asteria::Platform::Ia64::Memory
{
namespace
{
constexpr Foundation::UInt64 ExecuteBit{Foundation::UInt64{1U} << 32U};
constexpr Foundation::UInt64 WriteBit{Foundation::UInt64{1U} << 33U};
constexpr Foundation::UInt64 ReadBit{Foundation::UInt64{1U} << 34U};
constexpr Foundation::UInt64 NativeInterruptionStatusBits{0xFFFFU | (Foundation::UInt64{0x7FU} << 32U) |
                                                          (Foundation::UInt64{0x7U} << 41U)};
constexpr Foundation::UInt64 InstructionSetBit{Foundation::UInt64{1U} << 34U};
constexpr Foundation::UInt64 PrivilegeMask{0x3U};
constexpr Foundation::UInt64 PrivilegeShift{32U};
constexpr Foundation::UInt64 BundleAlignmentMask{0xFU};
constexpr Foundation::UInt64 SyndromeVectorShift{48U};

[[nodiscard]] constexpr bool IsSupportedVector(const Ia64PageFaultVector vector) noexcept
{
    switch (vector)
    {
    case Ia64PageFaultVector::AlternateDataTlb:
    case Ia64PageFaultVector::PageNotPresent:
    case Ia64PageFaultVector::KeyPermission:
    case Ia64PageFaultVector::InstructionAccessRights:
    case Ia64PageFaultVector::DataAccessRights:
        return true;
    }
    return false;
}
} // namespace

Ia64PageFaultDecodeError Ia64PageFaultDecoder::Decode(const Ia64PageFaultState& state,
                                                      Kernel::Memory::FaultRecord& output) noexcept
{
    if (!IsSupportedVector(state.Vector))
    {
        return Ia64PageFaultDecodeError::UnsupportedInterruptionVector;
    }
    if ((state.InterruptionProcessorStatus & InstructionSetBit) != 0U)
    {
        return Ia64PageFaultDecodeError::UnsupportedInstructionSet;
    }
    if ((state.InstructionPointer & BundleAlignmentMask) != 0U)
    {
        return Ia64PageFaultDecodeError::InvalidInstructionPointer;
    }
    if ((state.InterruptionStatus & ~NativeInterruptionStatusBits) != 0U)
    {
        return Ia64PageFaultDecodeError::InvalidInterruptionStatus;
    }

    const bool execute = (state.InterruptionStatus & ExecuteBit) != 0U;
    const bool write = (state.InterruptionStatus & WriteBit) != 0U;
    const bool read = (state.InterruptionStatus & ReadBit) != 0U;
    const bool instructionOnly = state.Vector == Ia64PageFaultVector::InstructionAccessRights;
    const bool dataOnly = (state.Vector == Ia64PageFaultVector::DataAccessRights) ||
                          (state.Vector == Ia64PageFaultVector::AlternateDataTlb);
    if ((execute && (read || write || dataOnly)) || (!execute && (!read && !write)) || (!execute && instructionOnly))
    {
        return Ia64PageFaultDecodeError::InvalidAccessEncoding;
    }

    Kernel::Memory::FaultAccess access{Kernel::Memory::FaultAccess::Read};
    if (execute)
    {
        access = Kernel::Memory::FaultAccess::Execute;
    }
    else if (write)
    {
        access = Kernel::Memory::FaultAccess::Write;
    }

    const auto vector = static_cast<Foundation::UInt64>(state.Vector);
    output = Kernel::Memory::FaultRecord{
        .VirtualAddress = state.FaultAddress,
        .InstructionPointer = state.InstructionPointer,
        .ArchitectureSyndrome = state.InterruptionStatus | (vector << SyndromeVectorShift),
        .Access = access,
        .Cause = ((state.Vector == Ia64PageFaultVector::PageNotPresent) ||
                  (state.Vector == Ia64PageFaultVector::AlternateDataTlb))
                     ? Kernel::Memory::FaultCause::MissingMapping
                     : Kernel::Memory::FaultCause::ProtectionViolation,
        .Privilege = ((state.InterruptionProcessorStatus >> PrivilegeShift) & PrivilegeMask) == 3U
                         ? Kernel::Memory::FaultPrivilege::User
                         : Kernel::Memory::FaultPrivilege::Supervisor,
        .Reserved = {},
    };
    return Ia64PageFaultDecodeError::None;
}
} // namespace Asteria::Platform::Ia64::Memory
