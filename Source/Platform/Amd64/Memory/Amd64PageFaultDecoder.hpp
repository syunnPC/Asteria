// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_AMD64_MEMORY_AMD64_PAGE_FAULT_DECODER_HPP_INCLUDED
#define ASTERIA_PLATFORM_AMD64_MEMORY_AMD64_PAGE_FAULT_DECODER_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Kernel/Memory/FaultToken.hpp"

namespace Asteria::Platform::Amd64::Memory
{
enum class Amd64PageFaultDecodeError : Foundation::UInt8
{
    None,
    UnsupportedSyndrome,
    InvalidAccessEncoding,
};

/// Values captured by the AMD64 page-fault entry path before another fault can
/// replace CR2. ErrorCode is the complete value pushed for exception vector 14.
struct Amd64PageFaultState
{
    Foundation::UInt64 FaultAddress;
    Foundation::UInt64 InstructionPointer;
    Foundation::UInt64 ErrorCode;
};

/// Converts one native AMD64 page fault into the architecture-neutral record
/// consumed by FaultToken. Failure leaves output unchanged.
class Amd64PageFaultDecoder final
{
public:
    [[nodiscard]] static Amd64PageFaultDecodeError Decode(const Amd64PageFaultState& state,
                                                          Kernel::Memory::FaultRecord& output) noexcept;

private:
    Amd64PageFaultDecoder() = delete;
};
} // namespace Asteria::Platform::Amd64::Memory

#endif // ASTERIA_PLATFORM_AMD64_MEMORY_AMD64_PAGE_FAULT_DECODER_HPP_INCLUDED
