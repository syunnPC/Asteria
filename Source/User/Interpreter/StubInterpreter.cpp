// SPDX-License-Identifier: Apache-2.0

#include "User/Interpreter/StubInterpreter.hpp"

#include "User/Loader/ExecutableFormats.hpp"

namespace Asteria::User::Interpreter
{
namespace
{
constexpr Foundation::UInt64 PageBytes{4096U};
constexpr Foundation::UInt32 ReportMethod{1U};

void StoreWord(Foundation::UInt8* const bytes, const Foundation::UInt64 value) noexcept
{
    for (Foundation::Size index{0U}; index < 8U; ++index)
    {
        bytes[index] = static_cast<Foundation::UInt8>(value >> (index << 3U));
    }
}

// The record of an interpreted launch whose guest the stub can read: its
// size within what the root service stages.
[[nodiscard]] bool IsInterpretedLaunch(const Foundation::UInt64 (&block)[Abi::ComponentInterpreterBlockWords]) noexcept
{
    const Foundation::UInt64 bytes = block[Abi::ComponentForeignBytesWord];
    return (block[Abi::ComponentStartMagicWord] == Abi::ComponentStartMagic) &&
           (block[Abi::ComponentBehaviorWord] == Abi::ComponentBehaviorInterpret) &&
           (block[Abi::ComponentForeignMagicWord] == Abi::ComponentForeignMagic) &&
           (block[Abi::ComponentForeignVersionWord] == Abi::ComponentForeignVersion) &&
           (block[Abi::ComponentForeignAddressWord] != 0U) && (bytes != 0U) &&
           (bytes <= (Abi::ComponentForeignImagePages * PageBytes));
}

// The guest, described with the loader's handlers, is the executable
// the record says it is.
[[gnu::noinline]] bool DescribeGuest(const Foundation::UInt64 (&block)[Abi::ComponentInterpreterBlockWords],
                                     StubBuffers& buffers, const Loader::MachineRules& guestRules) noexcept
{
    const auto* const image = reinterpret_cast<const Foundation::UInt8*>(
        static_cast<Foundation::UIntPointer>(block[Abi::ComponentForeignAddressWord]));
    const auto bytes = static_cast<Foundation::Size>(block[Abi::ComponentForeignBytesWord]);
    const Loader::ExecutableIdentity& identity = buffers.Guest.Identity;
    return (Loader::DescribeExecutable(image, bytes, guestRules, buffers.Guest) == Loader::LoaderError::None) &&
           (block[Abi::ComponentForeignFormatWord] == static_cast<Foundation::UInt64>(identity.Format)) &&
           (block[Abi::ComponentForeignMachineWord] == static_cast<Foundation::UInt64>(identity.Machine)) &&
           (block[Abi::ComponentForeignAbiWord] == static_cast<Foundation::UInt64>(identity.Abi)) &&
           (block[Abi::ComponentForeignByteOrderWord] == static_cast<Foundation::UInt64>(identity.ByteOrder));
}

[[gnu::noinline]] bool Report(const System::SystemCallClient& client,
                              const Foundation::UInt64 (&block)[Abi::ComponentInterpreterBlockWords],
                              StubBuffers& buffers) noexcept
{
    buffers.Message = Kernel::Ipc::MessageEnvelope{};
    buffers.Message.Interface = StubReportInterface;
    buffers.Message.TransactionIdentifier = block[Abi::ComponentIncarnationWord];
    buffers.Message.Deadline = Kernel::Object::MonotonicTime::Infinite();
    buffers.Message.MethodIdentifier = ReportMethod;
    buffers.Message.InterfaceVersionMajor = 1U;
    buffers.Message.Kind = Kernel::Ipc::MessageKind::Asynchronous;
    buffers.Message.Cancellation = Kernel::Ipc::CancellationState::None;
    buffers.Message.FixedPayloadSizeBytes = StubReportPayloadBytes;
    buffers.Message.PayloadSizeBytes = StubReportPayloadBytes;
    StoreWord(buffers.Message.Payload, buffers.Guest.Identity.MachineCode);
    StoreWord(buffers.Message.Payload + 8U, buffers.Guest.Entry);
    StoreWord(buffers.Message.Payload + 16U, buffers.Guest.GlobalPointer);
    StoreWord(buffers.Message.Payload + 24U, buffers.Guest.SegmentCount);
    StoreWord(buffers.Message.Payload + 32U, block[Abi::ComponentForeignBytesWord]);
    const auto encoded = Kernel::Ipc::MessageCodec::Encode(
        buffers.Message, Kernel::Ipc::WritableByteView{buffers.Frame, sizeof(buffers.Frame)});
    return encoded.HasValue() &&
           (client
                .Send(System::CapabilityHandle{block[Abi::ComponentReportWord], block[Abi::ComponentReportWord + 1U]},
                      static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(buffers.Frame)),
                      encoded.GetValue())
                .Error == Abi::SystemCallError::None);
}
} // namespace

Foundation::UInt64 RunStubInterpreter(const System::SystemCallTransport transport,
                                      const Foundation::UInt64 (&block)[Abi::ComponentInterpreterBlockWords],
                                      StubBuffers& buffers, const Loader::MachineRules& guestRules) noexcept
{
    const System::SystemCallClient client{transport};
    return (IsInterpretedLaunch(block) && DescribeGuest(block, buffers, guestRules) && Report(client, block, buffers))
               ? StubExitCode
               : StubFailedExitCode;
}
} // namespace Asteria::User::Interpreter
