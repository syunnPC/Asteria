// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Device/Ia64VtdHardware.hpp"

#include "Kernel/Diagnostics/StopTextWriter.hpp"
#include "Platform/Firmware/Diagnostic.hpp"

extern "C" void AsteriaIa64VtdFlushTables(const volatile void* start, Asteria::Foundation::UInt64 bytes) noexcept;
extern "C" void AsteriaIa64VtdFence() noexcept;

// The target leaves are in Ia64VtdCacheMaintenance.S. Hosted builds share the
// one cache-coherent memory of the host, so their stand-ins only order the
// compiler.
#if !defined(__ia64__)
extern "C" void AsteriaIa64VtdFlushTables(const volatile void*, const Asteria::Foundation::UInt64) noexcept
{
    __asm__ volatile("" ::: "memory");
}

extern "C" void AsteriaIa64VtdFence() noexcept
{
    __asm__ volatile("" ::: "memory");
}
#endif

namespace Asteria::Platform::Ia64::Device
{
namespace
{
using Foundation::Size;
using Foundation::UInt32;
using Foundation::UInt64;

/// Boot output on the diagnostic channel only: the IA-64 boot console is the
/// firmware serial port the same channel reaches, so no framebuffer is drawn.
class HandoffWriter final : public Kernel::Diagnostics::StopTextWriter<HandoffWriter>
{
public:
    void Put(const Foundation::UInt8 byte) noexcept
    {
        Platform::Firmware::WriteDiagnosticBytes(&byte, 1U);
    }
};
} // namespace

void Ia64VtdRegisters::Bind(const UInt64 base, const Foundation::UInt8 physicalBits) noexcept
{
    m_base = base;
    m_physicalBits = physicalBits;
}

void Ia64VtdRegisters::SetFirmwareMemoryPolicy(const Boot::BootInfo& info,
                                               const Platform::Device::VtdFirmwareMemoryOwnership& ownership) noexcept
{
    m_bootInfo = &info;
    m_ownership = ownership;
}

UInt32 Ia64VtdRegisters::Read32(const Size offset) const noexcept
{
    return static_cast<UInt32>(m_access.ReadDoubleWord(m_base + offset));
}

UInt64 Ia64VtdRegisters::Read64(const Size offset) const noexcept
{
    return m_access.ReadQuadWord(m_base + offset);
}

void Ia64VtdRegisters::Write32(const Size offset, const UInt32 value) noexcept
{
    m_access.WriteDoubleWord(m_base + offset, value);
}

void Ia64VtdRegisters::Write64(const Size offset, const UInt64 value) noexcept
{
    m_access.WriteQuadWord(m_base + offset, value);
}

bool Ia64VtdRegisters::CanWriteFirmwareMemory(const UInt64 address, const UInt64 bytes) const noexcept
{
    return (m_bootInfo != nullptr) && (bytes != 0U) &&
           m_access.IsPhysicalRangeImplemented(address, bytes, m_physicalBits) &&
           Platform::Device::VtdFirmwareQueueRangeIsWritable(*m_bootInfo, address, address + bytes - 1U, m_ownership);
}

bool Ia64VtdRegisters::ReadQueueWord(const UInt64 address, UInt32& value) const noexcept
{
    // BSP boot handoff only: firmware queue memory read through the uncached
    // corridor, so no cached copy can hide what the unit wrote.
    if (((address & 3U) != 0U) || !m_access.IsPhysicalRangeImplemented(address, 4U, m_physicalBits))
    {
        return false;
    }
    value = static_cast<UInt32>(m_access.ReadDoubleWord(address));
    return true;
}

bool Ia64VtdRegisters::WriteQueueWord(const UInt64 address, const UInt32 value) noexcept
{
    if (((address & 3U) != 0U) || !CanWriteFirmwareMemory(address, 4U))
    {
        return false;
    }
    m_access.WriteDoubleWord(address, value);
    return true;
}

void Ia64VtdRegisters::ReportHandoff(const Platform::Device::VtdFirmwareState& state) noexcept
{
    HandoffWriter writer;
    writer.Text("VT-D handoff BASE=");
    writer.Hex(m_base);
    writer.Text(" GSTS=");
    writer.Code(state.Status);
    writer.Text("\r\nIQA=");
    writer.Hex(state.QueueAddress);
    writer.Text(" IQH=");
    writer.Hex(state.QueueHead);
    writer.Text(" IQT=");
    writer.Hex(state.QueueTail);
    writer.Text("\r\nFSTS=");
    writer.Code(state.FaultStatus);
    writer.Text("\r\n");
}

void Ia64VtdRegisters::ReportRefusal(const Platform::Device::VtdHandoffFailure failure) noexcept
{
    HandoffWriter writer;
    writer.Text("VT-D refused BASE=");
    writer.Hex(m_base);
    writer.Text(" REASON=");
    writer.Text(Platform::Device::VtdHandoffFailureName(failure));
    writer.Text("\r\n");
}

void Ia64VtdCacheMaintenance::Flush(const volatile void* const line) noexcept
{
    FlushTables(line, 1U);
}

void Ia64VtdCacheMaintenance::FlushTables(const volatile void* const start, const Size bytes) noexcept
{
    AsteriaIa64VtdFlushTables(start, bytes);
    // The unit's lock serializes the writers; the evidence reader takes no lock.
    m_tableFlushes.StoreRelease(m_tableFlushes.LoadAcquire() + 1U);
}

void Ia64VtdCacheMaintenance::Fence() noexcept
{
    AsteriaIa64VtdFence();
}
} // namespace Asteria::Platform::Ia64::Device
