// SPDX-License-Identifier: Apache-2.0
#include <cstring>
#include <memory>

#include "User/Interpreter/StubInterpreter.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Foundation::Size;
using Foundation::UInt64;
using Foundation::UInt8;
using Kernel::Ipc::MessageCodec;
using Kernel::Ipc::MessageEnvelope;
using User::Loader::ExecutableAbi;
using User::Loader::ExecutableByteOrder;
using User::Loader::ExecutableFormat;
using User::Loader::ExecutableMachine;

constexpr UInt64 ReportSelector{0x0202U};
constexpr UInt64 Text{0x2000'0000'0100'0000ULL};

void Store(UInt8* const bytes, const UInt64 value, const Size width) noexcept
{
    for (Size index{0U}; index < width; ++index)
    {
        bytes[index] = static_cast<UInt8>(value >> (index * 8U));
    }
}

[[nodiscard]] UInt64 LoadWord(const UInt8* const bytes) noexcept
{
    UInt64 value{0U};
    for (Size index{0U}; index < 8U; ++index)
    {
        value |= static_cast<UInt64>(bytes[index]) << (index * 8U);
    }
    return value;
}

// The build's ia64-sample (Tools/Build/WriteForeignSamples.py): one R+X
// PT_LOAD mapping the whole file at Text, a bundle at 0x80 and the entry
// descriptor at 0x90.
struct Sample
{
    alignas(8) UInt8 Bytes[0xA0U]{};

    Sample() noexcept
    {
        Bytes[0U] = 0x7FU;
        Bytes[1U] = 'E';
        Bytes[2U] = 'L';
        Bytes[3U] = 'F';
        Bytes[4U] = 2U;
        Bytes[5U] = 1U;
        Bytes[6U] = 1U;
        Store(Bytes + 16U, 2U, 2U);
        Store(Bytes + 18U, 50U, 2U);
        Store(Bytes + 20U, 1U, 4U);
        Store(Bytes + 24U, Text + 0x90U, 8U);
        Store(Bytes + 32U, 64U, 8U);
        Store(Bytes + 48U, 0x10U, 4U);
        Store(Bytes + 52U, 64U, 2U);
        Store(Bytes + 54U, 56U, 2U);
        Store(Bytes + 56U, 1U, 2U);
        UInt8* const load = Bytes + 64U;
        Store(load, 1U, 4U);
        Store(load + 4U, 5U, 4U);
        Store(load + 16U, Text, 8U);
        Store(load + 24U, Text, 8U);
        Store(load + 32U, sizeof(Bytes), 8U);
        Store(load + 40U, sizeof(Bytes), 8U);
        Store(Bytes + 0x90U, Text + 0x80U, 8U);
        Store(Bytes + 0x98U, Text + 0x1000U, 8U);
    }
};

/// Records the report the stub sends; refuses anything else.
struct StubKernel
{
    static void Transport(void* const context, const Abi::SystemCallRequest* const request,
                          Abi::SystemCallResponse* const response) noexcept
    {
        auto& kernel = *static_cast<StubKernel*>(context);
        *response = Abi::SystemCallResponse{.Error = Abi::SystemCallError::None, .Value0 = 0U, .Value1 = 0U};
        if ((static_cast<Abi::SystemCallNumber>(request->Number) != Abi::SystemCallNumber::IpcSend) ||
            (request->Arguments[0U] != ReportSelector) || (request->Arguments[3U] > sizeof(kernel.Report)))
        {
            ++kernel.Other;
            response->Error = Abi::SystemCallError::InvalidArguments;
            return;
        }
        std::memcpy(kernel.Report, reinterpret_cast<const UInt8*>(request->Arguments[2U]), request->Arguments[3U]);
        kernel.ReportBytes = request->Arguments[3U];
    }

    UInt8 Report[MessageCodec::MaximumWireSizeBytes]{};
    Size ReportBytes{0U};
    Size Other{0U};
};

struct StubFixture
{
    Sample Guest{};
    UInt64 Block[Abi::ComponentInterpreterBlockWords]{};
    User::Interpreter::StubBuffers Buffers{};

    StubFixture() noexcept
    {
        Block[Abi::ComponentStartMagicWord] = Abi::ComponentStartMagic;
        Block[Abi::ComponentIncarnationWord] = 9U;
        Block[Abi::ComponentReportWord] = ReportSelector;
        Block[Abi::ComponentBehaviorWord] = Abi::ComponentBehaviorInterpret;
        Block[Abi::ComponentForeignMagicWord] = Abi::ComponentForeignMagic;
        Block[Abi::ComponentForeignVersionWord] = Abi::ComponentForeignVersion;
        Block[Abi::ComponentForeignAddressWord] = reinterpret_cast<Foundation::UIntPointer>(Guest.Bytes);
        Block[Abi::ComponentForeignBytesWord] = sizeof(Guest.Bytes);
        Block[Abi::ComponentForeignFormatWord] = static_cast<UInt64>(ExecutableFormat::Elf64);
        Block[Abi::ComponentForeignMachineWord] = static_cast<UInt64>(ExecutableMachine::Ia64);
        Block[Abi::ComponentForeignAbiWord] = static_cast<UInt64>(ExecutableAbi::Asteria);
        Block[Abi::ComponentForeignByteOrderWord] = static_cast<UInt64>(ExecutableByteOrder::Little);
    }

    [[nodiscard]] UInt64 Run(StubKernel& kernel,
                             const User::Loader::MachineRules& rules = User::Interpreter::Ia64GuestRules) noexcept
    {
        return User::Interpreter::RunStubInterpreter(User::System::SystemCallTransport{&StubKernel::Transport, &kernel},
                                                     Block, Buffers, rules);
    }
};

// The stub describes the read-only IA-64 guest the record names with the loader's handlers and
// reports its machine, entry ip and gp, segments and size.
void TestReport(TestContext& runner) noexcept
{
    const auto fixture = std::make_unique<StubFixture>();
    StubKernel kernel{};
    const UInt64 code = fixture->Run(kernel);
    MessageEnvelope report{};
    const bool decoded = MessageCodec::Decode(Kernel::Ipc::ReadOnlyByteView{kernel.Report, kernel.ReportBytes},
                                              report) == Kernel::Ipc::MessageCodecError::None;
    ASTERIA_EXPECT(runner, "the stub reports the IA-64 guest it was given", code == User::Interpreter::StubExitCode,
                   decoded, report.Interface.GetLow() == User::Interpreter::StubReportInterface.GetLow(),
                   report.PayloadSizeBytes == User::Interpreter::StubReportPayloadBytes,
                   report.TransactionIdentifier == 9U, LoadWord(report.Payload) == 50U,
                   LoadWord(report.Payload + 8U) == (Text + 0x80U), LoadWord(report.Payload + 16U) == (Text + 0x1000U),
                   LoadWord(report.Payload + 24U) == 1U, LoadWord(report.Payload + 32U) == 0xA0U, kernel.Other == 0U);
}

// The mirror launch uses AMD64's code entry, with no entry descriptor or gp.
// This catches a stub that silently keeps IA-64's rules on IA-64.
void TestAmd64Report(TestContext& runner) noexcept
{
    constexpr UInt64 amd64Text{0x0040'0000U};
    const auto fixture = std::make_unique<StubFixture>();
    Store(fixture->Guest.Bytes + 18U, 62U, 2U);
    Store(fixture->Guest.Bytes + 24U, amd64Text + 0x80U, 8U);
    Store(fixture->Guest.Bytes + 48U, 0U, 4U);
    Store(fixture->Guest.Bytes + 80U, amd64Text, 8U);
    Store(fixture->Guest.Bytes + 88U, amd64Text, 8U);
    fixture->Block[Abi::ComponentForeignMachineWord] = static_cast<UInt64>(ExecutableMachine::Amd64);
    StubKernel kernel{};
    const UInt64 code = fixture->Run(kernel, User::Interpreter::Amd64GuestRules);
    MessageEnvelope report{};
    const bool decoded = MessageCodec::Decode(Kernel::Ipc::ReadOnlyByteView{kernel.Report, kernel.ReportBytes},
                                              report) == Kernel::Ipc::MessageCodecError::None;
    ASTERIA_EXPECT(runner, "the stub reports the AMD64 guest under its code-entry rules",
                   code == User::Interpreter::StubExitCode, decoded, LoadWord(report.Payload) == 62U,
                   LoadWord(report.Payload + 8U) == amd64Text + 0x80U, LoadWord(report.Payload + 16U) == 0U,
                   LoadWord(report.Payload + 24U) == 1U, LoadWord(report.Payload + 32U) == 0xA0U);
}

// The changes after which the stub must refuse its launch.
void NotInterpreted(StubFixture& fixture) noexcept
{
    fixture.Block[Abi::ComponentBehaviorWord] = Abi::ComponentBehaviorReport;
}

void NoRecord(StubFixture& fixture) noexcept
{
    fixture.Block[Abi::ComponentForeignMagicWord] = 0U;
}

void OtherVersion(StubFixture& fixture) noexcept
{
    fixture.Block[Abi::ComponentForeignVersionWord] = 2U;
}

void Oversized(StubFixture& fixture) noexcept
{
    fixture.Block[Abi::ComponentForeignBytesWord] = (Abi::ComponentForeignImagePages * 4096U) + 1U;
}

void OtherIdentity(StubFixture& fixture) noexcept
{
    fixture.Block[Abi::ComponentForeignMachineWord] = static_cast<UInt64>(ExecutableMachine::Amd64);
}

void OtherGuest(StubFixture& fixture) noexcept
{
    Store(fixture.Guest.Bytes + 18U, 62U, 2U);
}

void Truncated(StubFixture& fixture) noexcept
{
    fixture.Block[Abi::ComponentForeignBytesWord] = 0x40U;
}

[[nodiscard]] bool Refuses(void (*const change)(StubFixture&) noexcept) noexcept
{
    const auto fixture = std::make_unique<StubFixture>();
    if (fixture == nullptr)
    {
        return false;
    }
    change(*fixture);
    StubKernel kernel{};
    return (fixture->Run(kernel) == User::Interpreter::StubFailedExitCode) && (kernel.ReportBytes == 0U);
}

// A launch that is not an interpreted one, a record of another identity, an
// oversized or broken guest: the stub exits without a report.
void TestRefusals(TestContext& runner) noexcept
{
    ASTERIA_EXPECT(runner, "the stub refuses what is not an interpreted launch of its guest", Refuses(NotInterpreted),
                   Refuses(NoRecord), Refuses(OtherVersion), Refuses(Oversized), Refuses(OtherIdentity),
                   Refuses(OtherGuest), Refuses(Truncated));
}
} // Namespace
} // Namespace Asteria::Test

int main()
{
    TestContext runner{"StubInterpreterTests"};
    Asteria::Test::TestReport(runner);
    Asteria::Test::TestAmd64Report(runner);
    Asteria::Test::TestRefusals(runner);
    return runner.Finish();
}
