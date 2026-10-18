// SPDX-License-Identifier: Apache-2.0
#include "Guest/Pager/RootScenarios.hpp"

#include "Abi/ComponentBootstrap.hpp"
#include "Kernel/Ipc/MessageCodec.hpp"
#include "User/Interpreter/StubInterpreter.hpp"
#include "User/Loader/ExecutableFormats.hpp"
#include "User/Loader/InterpreterRegistry.hpp"
#include "User/Pager/PagerWords.hpp"

namespace Asteria::User::Pager
{
namespace
{
constexpr Foundation::Size DeliveryHeaderBytes{48U};
// The registrations the build gives the pager: the opposite machine runs under its native stub.
constexpr Loader::InterpreterEntry Interpreters[]{
    {.Match = Loader::InterpreterMatch::Format | Loader::InterpreterMatch::Machine,
     .Format = Loader::ExecutableFormat::Elf64,
     .Machine = Loader::ExecutableMachine::Ia64,
     .Abi = Loader::ExecutableAbi::Asteria,
     .Interpreter = "ia64-stub",
     .Deny = false},
    {.Match = Loader::InterpreterMatch::Format | Loader::InterpreterMatch::Machine,
     .Format = Loader::ExecutableFormat::Elf64,
     .Machine = Loader::ExecutableMachine::Amd64,
     .Abi = Loader::ExecutableAbi::Asteria,
     .Interpreter = "amd64-stub",
     .Deny = false},
};
constexpr Foundation::Size InterpreterCount{sizeof(Interpreters) / sizeof(Interpreters[0U])};
constexpr char Ia64Sample[] = "ia64-sample";
constexpr char Amd64Sample[] = "amd64-sample";
constexpr char AArch64Sample[] = "aarch64-sample";
constexpr char PeSample[] = "pe-sample";

// The interpreter's image and its description, and a guest's.
constinit LoadedImage g_interpreter{};
constinit Loader::ExecutableImage g_guest{};

// An executable the native loader refuses as another machine's or system's goes to the registry,
// and the interpreter it names must itself be a native image of the table, the interpreter the root
// service builds; its guest at most the pages the loader stages.
[[nodiscard, gnu::noinline]] Loader::LoaderError Resolve(const ScenarioContext& context, const char* const name,
                                                         ForeignImage& foreign) noexcept
{
    const PagerThreadLaunch& launch = *context.Launch;
    const Loader::MachineRules& rules = launch.Component.Image;
    foreign = ForeignImage{};
    if (!FindComponentImage(launch.ComponentImages, launch.ComponentImagesBytes, name, foreign.Bytes, foreign.Size))
    {
        return Loader::LoaderError::Malformed;
    }
    Loader::LoaderError error = Loader::DescribeExecutable(foreign.Bytes, foreign.Size, rules, g_guest);
    foreign.Identity = g_guest.Identity;
    if (((error != Loader::LoaderError::ForeignMachineUnsupported) &&
         (error != Loader::LoaderError::UnsupportedFormat)) ||
        (foreign.Identity.Format == Loader::ExecutableFormat::Unknown))
    {
        return (error == Loader::LoaderError::None) ? Loader::LoaderError::UnsupportedFormat : error;
    }
    const char* interpreter{nullptr};
    error = Loader::ResolveInterpreter(Interpreters, InterpreterCount, foreign.Identity, rules.Machine, interpreter);
    if (error != Loader::LoaderError::None)
    {
        return error;
    }
    if (!FindComponentImage(launch.ComponentImages, launch.ComponentImagesBytes, interpreter, g_interpreter.Bytes,
                            g_interpreter.Size) ||
        (Loader::DescribeExecutable(g_interpreter.Bytes, g_interpreter.Size, rules, g_interpreter.Layout) !=
         Loader::LoaderError::None))
    {
        return Loader::LoaderError::InterpreterNotNative;
    }
    return (foreign.Size > (Abi::ComponentForeignImagePages * 4096U)) ? Loader::LoaderError::ImageTooLarge
                                                                      : Loader::LoaderError::None;
}

// The stub's report: the guest's machine, entry and gp, segments and size,
// as the root service describes the guest under its own machine rules.
[[nodiscard, gnu::noinline]] bool ReadStubReport(ScenarioContext& context, const Foundation::UInt64 received,
                                                 const ForeignImage& foreign) noexcept
{
    const Loader::MachineRules& guestRules =
        (context.Launch->Component.Image.Machine == Loader::ExecutableMachine::Ia64) ? Interpreter::Amd64GuestRules
                                                                                     : Interpreter::Ia64GuestRules;
    const Foundation::UInt8* const payload = context.Message.Payload;
    return (received > DeliveryHeaderBytes) &&
           (Kernel::Ipc::MessageCodec::Decode(
                Kernel::Ipc::ReadOnlyByteView{context.Delivery + DeliveryHeaderBytes,
                                              static_cast<Foundation::Size>(received - DeliveryHeaderBytes)},
                context.Message) == Kernel::Ipc::MessageCodecError::None) &&
           (context.Message.Interface.GetLow() == Interpreter::StubReportInterface.GetLow()) &&
           (context.Message.Interface.GetHigh() == Interpreter::StubReportInterface.GetHigh()) &&
           (context.Message.PayloadSizeBytes == Interpreter::StubReportPayloadBytes) &&
           (Loader::DescribeExecutable(foreign.Bytes, foreign.Size, guestRules, g_guest) ==
            Loader::LoaderError::None) &&
           (LoadWord(payload) == g_guest.Identity.MachineCode) && (LoadWord(payload + 8U) == g_guest.Entry) &&
           (LoadWord(payload + 16U) == g_guest.GlobalPointer) && (LoadWord(payload + 24U) == g_guest.SegmentCount) &&
           (LoadWord(payload + 32U) == foreign.Size);
}

// The interpreter runs as a task under its watchdog, given its guest; it
// reports and exits, and the root service requires both before it tears it
// down.
[[nodiscard, gnu::noinline]] PagerServeError Interpret(ScenarioContext& context, const ForeignImage& foreign,
                                                       Abi::SystemCallError& systemCall) noexcept
{
    ComponentSupervisor& supervisor = *context.Supervisor;
    ComponentRecord* const interpreter =
        StartComponent(context, g_interpreter, context.NextTag++, Abi::ComponentBehaviorInterpret, foreign, systemCall);
    if (interpreter == nullptr)
    {
        return PagerServeError::ManagementFailed;
    }
    const PagerServeError served = supervisor.Supervise(*interpreter, systemCall);
    if (served != PagerServeError::None)
    {
        return served;
    }
    const Abi::SystemCallResponse received =
        context.Client->Receive(context.Report, AddressOf(context.Delivery), sizeof(context.Delivery));
    const bool crashed = supervisor.HasCrashedSince(*interpreter);
    if ((interpreter->ExitCode != Abi::ComponentInterpretedExitCode) || crashed || !Check(received, systemCall) ||
        !ReadStubReport(context, received.Value1, foreign))
    {
        ReportUnexpectedEnd(context, interpreter->Tag, interpreter->ExitCode, crashed);
        return PagerServeError::ManagementFailed;
    }
    return supervisor.Teardown(*interpreter, systemCall) ? PagerServeError::None : PagerServeError::ManagementFailed;
}
} // Namespace

// The loader's demonstration: a foreign executable runs under the interpreter registered for it; an
// AArch64 one finds none and a PE32+ one for the native ISA is a format the loader does not support
// ( precedence).
PagerServeError RunLoaderDemo(ScenarioContext& context, Abi::SystemCallError& systemCall) noexcept
{
    ForeignImage foreign{};
    const bool ia64 = context.Launch->Component.Image.Machine == Loader::ExecutableMachine::Ia64;
    if (Resolve(context, ia64 ? Amd64Sample : Ia64Sample, foreign) != Loader::LoaderError::None)
    {
        return PagerServeError::ManagementFailed;
    }
    const PagerServeError interpreted = Interpret(context, foreign, systemCall);
    if (interpreted != PagerServeError::None)
    {
        return interpreted;
    }
    if (ia64)
    {
        WriteLine(context, LoaderAmd64InterpretedLine, sizeof(LoaderAmd64InterpretedLine) - 1U);
    }
    else
    {
        WriteLine(context, LoaderInterpretedLine, sizeof(LoaderInterpretedLine) - 1U);
    }
    if (Resolve(context, AArch64Sample, foreign) != Loader::LoaderError::ForeignMachineUnsupported)
    {
        return PagerServeError::ManagementFailed;
    }
    WriteLine(context, LoaderForeignLine, sizeof(LoaderForeignLine) - 1U);
    if ((Resolve(context, PeSample, foreign) != Loader::LoaderError::UnsupportedFormat) ||
        (foreign.Identity.Format != Loader::ExecutableFormat::Pe32Plus))
    {
        return PagerServeError::ManagementFailed;
    }
    WriteLine(context, LoaderFormatLine, sizeof(LoaderFormatLine) - 1U);
    return PagerServeError::None;
}
} // Namespace Asteria::User::Pager
