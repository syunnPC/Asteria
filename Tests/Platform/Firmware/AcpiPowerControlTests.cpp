// SPDX-License-Identifier: Apache-2.0
#include <csetjmp>
#include "Platform/Firmware/AcpiPowerControl.hpp"
#include "Platform/Ia64/Diagnostics/Ia64Stop.hpp"
#include "User/System/AhciPort.hpp"
#include "Support/TestContext.hpp"
namespace
{
namespace Boot = Asteria::Boot;
namespace Foundation = Asteria::Foundation;
namespace Hal = Asteria::Hal;
namespace Platform = Asteria::Platform;
namespace User = Asteria::User;
std::jmp_buf jump;
struct Sample
{
    Foundation::UInt16 Port;
    Foundation::UInt8 Width;
    Foundation::UInt32 Value;
};
class Ports final : public Hal::Device::IPortIo
{
public:
    Sample Writes[8U]{};
    Foundation::Size Count{0U};
    bool Parked{false};
    bool Early{false};
    bool Legacy{false};
    bool EnableWorks{true};
    Foundation::UInt32 Read(Foundation::UInt16 port, Foundation::UInt8) noexcept override
    {
        return (port == 0x400U ? 0xC205U : 0xC201U) & (Legacy ? ~1U : ~0U);
    }
    void Write(Foundation::UInt16 port, Foundation::UInt8 width, Foundation::UInt32 value) noexcept override
    {
        Early = Early || !Parked;
        Writes[Count++] = {port, width, value};
        if (port == 0xB2U && EnableWorks)
        {
            Legacy = false;
        }
    }
};
class Control final : public Platform::Firmware::AcpiPowerControl
{
public:
    Ports* Io{nullptr};
    bool Confirm{true};
    Foundation::UInt64 MemoryAddress[4U]{};
    Foundation::UInt32 MemoryValue[4U]{};
    Foundation::UInt8 MemoryWidth[4U]{};
    Foundation::Size MemoryCount{0U};

private:
    bool ParkOthers() noexcept override
    {
        Io->Parked = true;
        return Confirm;
    }
    Foundation::UInt32 ReadMemory(const Boot::Acpi::PowerRegister&) noexcept override
    {
        return 0x8005U;
    }
    void WriteMemory(const Boot::Acpi::PowerRegister& reg, Foundation::UInt32 value) noexcept override
    {
        MemoryAddress[MemoryCount] = reg.Address;
        MemoryValue[MemoryCount] = value;
        MemoryWidth[MemoryCount++] = reg.Width;
    }
    [[noreturn]] void Returned(Platform::Firmware::PowerResult) noexcept override
    {
        std::longjmp(jump, 1);
    }
};
Ports ports;
Control control;
alignas(4096) Foundation::UInt8 page[4096U];
} // namespace
int main() noexcept
{
    Asteria::TestSupport::TestContext test{"AcpiPowerControlTests"};
    ASTERIA_EXPECT(test, "SAL INIT targets the requesting CPU with architectural delivery mode",
                   Platform::Ia64::Diagnostics::Ia64PowerInitMessage == 0x500U,
                   Platform::Ia64::Diagnostics::Ia64PowerInitAddress(0xFEE00000U, 0x12AB0000U) == 0xFEE12AB0U);
    ASTERIA_EXPECT(test, "SAL version eligibility uses the firmware packed BCD revision",
                   Platform::Ia64::Diagnostics::Ia64PowerSalRevision(0x0290U),
                   Platform::Ia64::Diagnostics::Ia64PowerSalRevision(0x0300U),
                   !Platform::Ia64::Diagnostics::Ia64PowerSalRevision(0x0209U),
                   !Platform::Ia64::Diagnostics::Ia64PowerSalRevision(0x0289U));
    Boot::Acpi::PowerRegisters facts{};
    facts.ControlA = {.Address = 0x400U, .Space = 1U, .Width = 2U, .Reserved = {}};
    facts.ControlB = {.Address = 0x404U, .Space = 1U, .Width = 2U, .Reserved = {}};
    facts.Reset = {.Address = 0xCF9U, .Space = 1U, .Width = 1U, .Reserved = {}};
    facts.ResetValue = 6U;
    facts.TypeA = 5U;
    facts.TypeB = 7U;
    facts.SleepKnown = 1U;
    control.Io = &ports;
    control.Configure(facts, &ports);
    ASTERIA_EXPECT(test, "validated S5 and reset, EFI unavailable", control.Available() == 3U);
    if (setjmp(jump) == 0)
    {
        static_cast<void>(control.Transition(false));
    }
    ASTERIA_EXPECT(test, "CPU acknowledgement precedes every write", !ports.Early, ports.Count == 4U);
    ASTERIA_EXPECT(test, "stage both types then enable A and B preserving unrelated controls",
                   ports.Writes[0].Value == 0xD601U, ports.Writes[1].Value == 0xDE01U, ports.Writes[2].Value == 0xF601U,
                   ports.Writes[3].Value == 0xFE01U, ports.Writes[0].Width == 2U, ports.Writes[1].Port == 0x404U);
    ports = {};
    control.Confirm = false;
    if (setjmp(jump) == 0)
    {
        static_cast<void>(control.Transition(true));
    }
    ASTERIA_EXPECT(test, "unconfirmed CPU prevents reset write", ports.Count == 0U);
    ports = {};
    control.Confirm = true;
    if (setjmp(jump) == 0)
    {
        static_cast<void>(control.Transition(true));
    }
    ASTERIA_EXPECT(test, "FADT reset is exact width and value", ports.Count == 1U, ports.Writes[0].Width == 1U,
                   ports.Writes[0].Value == 6U, ports.Writes[0].Port == 0xCF9U);
    facts.SmiCommand = 0xB2U;
    facts.AcpiEnable = 0xA1U;
    ports = {};
    ports.Legacy = true;
    control.Configure(facts, &ports);
    if (setjmp(jump) == 0)
    {
        static_cast<void>(control.Transition(false));
    }
    ASTERIA_EXPECT(test, "FADT ACPI ownership is acquired before sleep writes", !ports.Early, ports.Count == 5U,
                   ports.Writes[0U].Port == 0xB2U, ports.Writes[0U].Width == 1U, ports.Writes[0U].Value == 0xA1U,
                   ports.Writes[1U].Value == 0xD601U);
    ports = {};
    ports.Legacy = true;
    ports.EnableWorks = false;
    if (setjmp(jump) == 0)
    {
        static_cast<void>(control.Transition(false));
    }
    ASTERIA_EXPECT(test, "ACPI ownership timeout prevents all PM1 writes", ports.Count == 1U,
                   ports.Writes[0U].Port == 0xB2U);
    facts.SmiCommand = 0U;
    control.Configure(facts, &ports);
    ports = {};
    ports.Legacy = true;
    if (setjmp(jump) == 0)
    {
        static_cast<void>(control.Transition(false));
    }
    ASTERIA_EXPECT(test, "missing legacy ownership command prevents all writes", ports.Count == 0U);
    facts.SleepKnown = 0U;
    control.Configure(facts, &ports);
    ASTERIA_EXPECT(test, "unknown AML leaves independent reset available", control.Available() == 2U,
                   !control.Transition(false));
    facts.Reset.Width = 2U;
    control.Configure(facts, &ports);
    ASTERIA_EXPECT(test, "malformed reset width refused", control.Available() == 0U);
    facts.ControlA = {.Address = 0xFED01000U, .Space = 0U, .Width = 2U, .Reserved = {}};
    facts.ControlB = {};
    facts.Reset = {};
    facts.SleepKnown = 1U;
    control.Configure(facts, nullptr);
    if (setjmp(jump) == 0)
    {
        static_cast<void>(control.Transition(false));
    }
    ASTERIA_EXPECT(test, "MMIO S5 uses exact-width controls without a port backend", control.Available() == 1U,
                   control.MemoryCount == 2U, control.MemoryAddress[0U] == 0xFED01000U, control.MemoryWidth[0U] == 2U,
                   control.MemoryValue[0U] == 0x9401U, control.MemoryValue[1U] == 0xB401U);
    ASTERIA_EXPECT(test, "flush is a non-data H2D FIS", User::System::BuildAhciFlush(page, 0U, 0x100500U),
                   page[0] == 5U, page[2] == 0U, page[0x502] == 0xEAU, page[0x500] == 0x27U, page[0x501] == 0x80U);
    return test.Finish();
}
