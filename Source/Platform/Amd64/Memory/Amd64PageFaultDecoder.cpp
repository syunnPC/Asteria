// SPDX-License-Identifier: Apache-2.0

#include "Platform/Amd64/Memory/Amd64PageFaultDecoder.hpp"

namespace Asteria::Platform::Amd64::Memory
{
namespace
{
constexpr Foundation::UInt64 PresentBit{Foundation::UInt64{1U} << 0U};
constexpr Foundation::UInt64 WriteBit{Foundation::UInt64{1U} << 1U};
constexpr Foundation::UInt64 UserBit{Foundation::UInt64{1U} << 2U};
constexpr Foundation::UInt64 ReservedEncodingBit{Foundation::UInt64{1U} << 3U};
constexpr Foundation::UInt64 InstructionFetchBit{Foundation::UInt64{1U} << 4U};
constexpr Foundation::UInt64 ProtectionKeyBit{Foundation::UInt64{1U} << 5U};
constexpr Foundation::UInt64 ShadowStackBit{Foundation::UInt64{1U} << 6U};
constexpr Foundation::UInt64 ReverseMapViolationBit{Foundation::UInt64{1U} << 31U};
constexpr Foundation::UInt64 SupportedSyndromeBits{PresentBit | WriteBit | UserBit | ReservedEncodingBit |
                                                   InstructionFetchBit | ProtectionKeyBit | ShadowStackBit |
                                                   ReverseMapViolationBit};
} // namespace

Amd64PageFaultDecodeError Amd64PageFaultDecoder::Decode(const Amd64PageFaultState& state,
                                                        Kernel::Memory::FaultRecord& output) noexcept
{
    if ((state.ErrorCode & ~SupportedSyndromeBits) != 0U)
    {
        return Amd64PageFaultDecodeError::UnsupportedSyndrome;
    }

    const bool instructionFetch = (state.ErrorCode & InstructionFetchBit) != 0U;
    if (instructionFetch && ((state.ErrorCode & (WriteBit | ProtectionKeyBit | ShadowStackBit)) != 0U))
    {
        return Amd64PageFaultDecodeError::InvalidAccessEncoding;
    }
    if (((state.ErrorCode & PresentBit) == 0U) &&
        ((state.ErrorCode & (ReservedEncodingBit | ProtectionKeyBit | ReverseMapViolationBit)) != 0U))
    {
        return Amd64PageFaultDecodeError::InvalidAccessEncoding;
    }

    Kernel::Memory::FaultAccess access{Kernel::Memory::FaultAccess::Read};
    if (instructionFetch)
    {
        access = Kernel::Memory::FaultAccess::Execute;
    }
    else if ((state.ErrorCode & WriteBit) != 0U)
    {
        access = Kernel::Memory::FaultAccess::Write;
    }

    Kernel::Memory::FaultCause cause{Kernel::Memory::FaultCause::MissingMapping};
    if ((state.ErrorCode & ReservedEncodingBit) != 0U)
    {
        cause = Kernel::Memory::FaultCause::InvalidMappingEncoding;
    }
    else if ((state.ErrorCode & PresentBit) != 0U)
    {
        cause = Kernel::Memory::FaultCause::ProtectionViolation;
    }

    output = Kernel::Memory::FaultRecord{
        .VirtualAddress = state.FaultAddress,
        .InstructionPointer = state.InstructionPointer,
        .ArchitectureSyndrome = state.ErrorCode,
        .Access = access,
        .Cause = cause,
        .Privilege = (state.ErrorCode & UserBit) != 0U ? Kernel::Memory::FaultPrivilege::User
                                                       : Kernel::Memory::FaultPrivilege::Supervisor,
        .Reserved = {},
    };
    return Amd64PageFaultDecodeError::None;
}
} // namespace Asteria::Platform::Amd64::Memory
