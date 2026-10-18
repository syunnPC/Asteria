// SPDX-License-Identifier: Apache-2.0
#if !defined(__ia64__)
#include <cstddef>
#include <cstdio>

#include "Boot/BootInfo.hpp"
#endif

#include "Foundation/Types.hpp"
#include "Platform/Ia64/Firmware/SalBootRendezvousRegistration.hpp"

#if defined(__ia64__)
extern "C" [[gnu::noinline]]
Asteria::Platform::Ia64::Firmware::SalFirmwareCallResult
AsteriaIa64InvokePhysicalSalSetVectors(const Asteria::Platform::Ia64::Firmware::SalFirmwareCallRequest* request,
                                       Asteria::Foundation::UInt64* invocationState) noexcept;

extern "C" [[gnu::used, gnu::noinline]]
Asteria::Platform::Ia64::Firmware::SalFirmwareCallResult AsteriaIa64SalSetVectorsBridgeAbiFixture() noexcept
{
    const Asteria::Platform::Ia64::Firmware::SalFirmwareCallRequest request{
        .EntryPhysicalAddress = 0x1111'1111'1111'1111U,
        .GlobalDataPhysicalAddress = 0x2222'2222'2222'2222U,
        .Arguments =
            {
                0x3333'3333'3333'3333U,
                0x4444'4444'4444'4444U,
                0x5555'5555'5555'5555U,
                0x6666'6666'6666'6666U,
                0x7777'7777'7777'7777U,
                0x1234'5678'9ABC'DEF0U,
                0x0FED'CBA9'8765'4321U,
                0x7FFF'FFFF'FFFF'FFFFU,
            },
    };
    Asteria::Foundation::UInt64 invocationState{~Asteria::Foundation::UInt64{0U}};
    return AsteriaIa64InvokePhysicalSalSetVectors(&request, &invocationState);
}
#else
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

extern "C" Asteria::Platform::Ia64::Firmware::SalFirmwareCallResult
AsteriaIa64InvokePhysicalSalSetVectors(const Asteria::Platform::Ia64::Firmware::SalFirmwareCallRequest* request,
                                       Asteria::Foundation::UInt64* invocationState) noexcept;

namespace Asteria::Test
{
namespace
{
using Boot::PhysicalMemoryKind;
using Boot::PhysicalMemoryRegion;
using Platform::Ia64::Firmware::SalBootRendezvousHandler;
using Platform::Ia64::Firmware::SalBootRendezvousRegistration;
using Platform::Ia64::Firmware::SalBootRendezvousRegistrationDisposition;
using Platform::Ia64::Firmware::SalBootRendezvousRegistrationResult;
using Platform::Ia64::Firmware::SalBootRendezvousRegistrationStatus;
using Platform::Ia64::Firmware::SalEntrypointBinding;
using Platform::Ia64::Firmware::SalFirmwareCallRequest;
using Platform::Ia64::Firmware::SalFirmwareCallResult;
using Platform::Ia64::Firmware::SalSystemTableInformation;

struct InvocationRecord
{
    Foundation::UInt64 EntryPhysicalAddress;
    Foundation::UInt64 GlobalDataPhysicalAddress;
    Foundation::UInt64 Arguments[8U];
    Foundation::UInt64 InvocationCount;
};

constinit InvocationRecord g_invocation{};
constinit Foundation::UInt64 g_stubInvocationState;
constinit SalFirmwareCallResult g_stubResult{};
Foundation::UInt8 g_bindingSnapshot[sizeof(SalEntrypointBinding)];
Foundation::UInt8 g_informationSnapshot[sizeof(SalSystemTableInformation)];
Foundation::UInt8 g_handlerSnapshot[sizeof(SalBootRendezvousHandler)];

[[nodiscard]] constexpr PhysicalMemoryRegion Region(const Foundation::UInt64 physicalStart,
                                                    const Foundation::UInt64 pageCount,
                                                    const Foundation::UInt32 firmwareType,
                                                    const PhysicalMemoryKind kind,
                                                    const Foundation::UInt64 attributes = 0U) noexcept
{
    return PhysicalMemoryRegion{
        .PhysicalStart = physicalStart,
        .VirtualStart = 0U,
        .PageCount = pageCount,
        .Attributes = attributes,
        .FirmwareType = firmwareType,
        .Kind = kind,
    };
}

[[nodiscard]] constexpr SalEntrypointBinding ValidBinding() noexcept
{
    constexpr Foundation::UInt64 runtimeAttribute = Foundation::UInt64{1U} << 63U;
    return SalEntrypointBinding{
        .PalProcedurePhysicalAddress = 0x2000U,
        .SalProcedurePhysicalAddress = 0x4000U,
        .SalGlobalDataPhysicalAddress = 0x6000U,
        .PalCodeRegion = Region(0x1000U, 2U, 13U, PhysicalMemoryKind::PalCode),
        .SalCodeRegion = Region(0x4000U, 2U, 5U, PhysicalMemoryKind::RuntimeServices, runtimeAttribute),
        .SalGlobalDataRegion = Region(0x6000U, 2U, 6U, PhysicalMemoryKind::RuntimeServices, runtimeAttribute),
    };
}

[[nodiscard]] constexpr SalSystemTableInformation ValidInformation(const Foundation::UInt16 revision = 0x0340U) noexcept
{
    return SalSystemTableInformation{
        .PalProcedurePhysicalAddress = 0x2000U,
        .SalProcedurePhysicalAddress = 0x4000U,
        .SalGlobalDataPhysicalAddress = 0x6000U,
        .TranslationRegisterVirtualAddress = 0U,
        .TranslationRegisterEncodedPageSize = 0U,
        .CoherenceDomainInformationPhysicalAddress = 0U,
        .ApWakeVector = 0U,
        .TotalTableLength = 144U,
        .CoherenceDomainCount = 0U,
        .SalRevision = revision,
        .EntryCount = 1U,
        .SalAVersion = 0x0100U,
        .SalBVersion = 0x0100U,
        .MemoryDescriptorCount = 0U,
        .PlatformFeatures = 0U,
        .HasPlatformFeatures = false,
        .HasTranslationRegister = false,
        .HasPurgeCoherenceInformation = false,
        .HasApWakeVector = false,
        .Reserved8 = 0U,
    };
}

[[nodiscard]] constexpr SalBootRendezvousHandler ValidHandler() noexcept
{
    return SalBootRendezvousHandler{
        .EntryPhysicalAddress = 0x0010'0100U,
        .GlobalPointerPhysicalAddress = 0x0011'0000U,
        .CodePhysicalStart = 0x0010'0000U,
        .CodeSizeBytes = 0x2000U,
    };
}

void ResetStub() noexcept
{
    g_invocation = InvocationRecord{};
    g_stubInvocationState = 0U;
    g_stubResult = SalFirmwareCallResult{
        .Status = 0,
        .Result1 = 0U,
        .Result2 = 0U,
        .Result3 = 0U,
    };
}

template <typename TValue> void Snapshot(const TValue& value, Foundation::UInt8* const snapshot) noexcept
{
    const auto* const bytes = reinterpret_cast<const Foundation::UInt8*>(&value);
    for (Foundation::Size index{0U}; index < sizeof(value); ++index)
    {
        snapshot[index] = bytes[index];
    }
}

template <typename TValue>
[[nodiscard]] bool MatchesSnapshot(const TValue& value, const Foundation::UInt8* const snapshot) noexcept
{
    const auto* const bytes = reinterpret_cast<const Foundation::UInt8*>(&value);
    for (Foundation::Size index{0U}; index < sizeof(value); ++index)
    {
        if (snapshot[index] != bytes[index])
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool HasResult(const SalBootRendezvousRegistrationResult& result,
                             const SalBootRendezvousRegistrationStatus status,
                             const SalBootRendezvousRegistrationDisposition disposition,
                             const Foundation::Int64 firmwareStatus = 0) noexcept
{
    return (result.Status == status) && (result.Disposition == disposition) && (result.Reserved[0U] == 0U) &&
           (result.Reserved[1U] == 0U) && (result.Reserved[2U] == 0U) && (result.FirmwareStatus == firmwareStatus);
}

[[nodiscard]] bool HasNoInvocation() noexcept
{
    return g_invocation.InvocationCount == 0U;
}

[[nodiscard]] bool HasExactSetVectorsCall(const SalEntrypointBinding& binding,
                                          const SalBootRendezvousHandler& handler) noexcept
{
    return (g_invocation.InvocationCount == 1U) &&
           (g_invocation.EntryPhysicalAddress == binding.SalProcedurePhysicalAddress) &&
           (g_invocation.GlobalDataPhysicalAddress == binding.SalGlobalDataPhysicalAddress) &&
           (g_invocation.Arguments[0U] == 0x0100'0000U) && (g_invocation.Arguments[1U] == 2U) &&
           (g_invocation.Arguments[2U] == handler.EntryPhysicalAddress) &&
           (g_invocation.Arguments[3U] == handler.GlobalPointerPhysicalAddress) && (g_invocation.Arguments[4U] == 0U) &&
           (g_invocation.Arguments[5U] == 0U) && (g_invocation.Arguments[6U] == 0U) &&
           (g_invocation.Arguments[7U] == 0U);
}

[[nodiscard]] bool HasExactRequest(const SalFirmwareCallRequest& request) noexcept
{
    if ((g_invocation.InvocationCount != 1U) || (g_invocation.EntryPhysicalAddress != request.EntryPhysicalAddress) ||
        (g_invocation.GlobalDataPhysicalAddress != request.GlobalDataPhysicalAddress))
    {
        return false;
    }
    for (Foundation::Size index{0U}; index < 8U; ++index)
    {
        if (g_invocation.Arguments[index] != request.Arguments[index])
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] SalBootRendezvousRegistrationResult RegisterValid(const SalSystemTableInformation& information) noexcept
{
    const SalEntrypointBinding binding = ValidBinding();
    const SalBootRendezvousHandler handler = ValidHandler();
    return SalBootRendezvousRegistration::Register(binding, information, handler);
}

void TestFixedRequestBoundary(TestContext& runner) noexcept
{
    const SalFirmwareCallRequest request{
        .EntryPhysicalAddress = 0x1111'1111'1111'1111U,
        .GlobalDataPhysicalAddress = 0x2222'2222'2222'2222U,
        .Arguments =
            {
                0x3333'3333'3333'3333U,
                0x4444'4444'4444'4444U,
                0x5555'5555'5555'5555U,
                0x6666'6666'6666'6666U,
                0x7777'7777'7777'7777U,
                0x1234'5678'9ABC'DEF0U,
                0x0FED'CBA9'8765'4321U,
                0x7FFF'FFFF'FFFF'FFFFU,
            },
    };
    constexpr SalFirmwareCallResult expectedResult{
        .Status = -3,
        .Result1 = 0xAAAA'AAAA'AAAA'AAAAU,
        .Result2 = 0xBBBB'BBBB'BBBB'BBBBU,
        .Result3 = 0xCCCC'CCCC'CCCC'CCCCU,
    };
    ResetStub();
    g_stubResult = expectedResult;
    Foundation::UInt64 invocationState{~Foundation::UInt64{0U}};
    const SalFirmwareCallResult result = AsteriaIa64InvokePhysicalSalSetVectors(&request, &invocationState);
    ASTERIA_EXPECT(runner, "the fixed request carries all ten words through a two-argument bridge",
                   HasExactRequest(request), invocationState == 0U, result.Status == expectedResult.Status,
                   result.Result1 == expectedResult.Result1, result.Result2 == expectedResult.Result2,
                   result.Result3 == expectedResult.Result3);
}

void TestSupportedRevisionEncoding(TestContext& runner) noexcept
{
    constexpr Foundation::UInt16 revisions[]{
        0x0280U, 0x0290U, 0x0300U, 0x0340U, 0x0399U,
    };
    for (const Foundation::UInt16 revision : revisions)
    {
        ResetStub();
        const SalEntrypointBinding binding = ValidBinding();
        const SalSystemTableInformation information = ValidInformation(revision);
        const SalBootRendezvousHandler handler = ValidHandler();
        const SalBootRendezvousRegistrationResult result =
            SalBootRendezvousRegistration::Register(binding, information, handler);
        ASTERIA_EXPECT(runner, "supported 2.8-through-3.x revisions use the zero-length form",
                       HasResult(result, SalBootRendezvousRegistrationStatus::Registered,
                                 SalBootRendezvousRegistrationDisposition::Registered),
                       HasExactSetVectorsCall(binding, handler));
    }
}

void TestRetainedInputValidation(TestContext& runner) noexcept
{
    SalSystemTableInformation information = ValidInformation();
    information.SalRevision = 0x0270U;
    ResetStub();
    ASTERIA_EXPECT(runner, "a pre-2.8 SAL revision is rejected before invocation",
                   HasResult(RegisterValid(information), SalBootRendezvousRegistrationStatus::InvalidSalInformation,
                             SalBootRendezvousRegistrationDisposition::NotRegistered),
                   HasNoInvocation());

    information = ValidInformation();
    information.SalRevision = 0x02A0U;
    ResetStub();
    ASTERIA_EXPECT(runner, "a non-BCD SAL revision is rejected before invocation",
                   HasResult(RegisterValid(information), SalBootRendezvousRegistrationStatus::InvalidSalInformation,
                             SalBootRendezvousRegistrationDisposition::NotRegistered),
                   HasNoInvocation());

    information = ValidInformation();
    information.SalRevision = 0x0400U;
    ResetStub();
    ASTERIA_EXPECT(runner, "a post-3.x SAL revision is rejected before invocation",
                   HasResult(RegisterValid(information), SalBootRendezvousRegistrationStatus::InvalidSalInformation,
                             SalBootRendezvousRegistrationDisposition::NotRegistered),
                   HasNoInvocation());

    information = ValidInformation();
    information.Reserved8 = 1U;
    ResetStub();
    ASTERIA_EXPECT(runner, "a changed retained SAL reserved byte is rejected",
                   HasResult(RegisterValid(information), SalBootRendezvousRegistrationStatus::InvalidSalInformation,
                             SalBootRendezvousRegistrationDisposition::NotRegistered),
                   HasNoInvocation());

    SalEntrypointBinding binding = ValidBinding();
    binding.SalCodeRegion.Attributes = 0U;
    ResetStub();
    const SalBootRendezvousHandler handler = ValidHandler();
    ASTERIA_EXPECT(runner, "SAL code without the retained runtime attribute is rejected",
                   HasResult(SalBootRendezvousRegistration::Register(binding, ValidInformation(), handler),
                             SalBootRendezvousRegistrationStatus::InvalidBinding,
                             SalBootRendezvousRegistrationDisposition::NotRegistered),
                   HasNoInvocation());

    binding = ValidBinding();
    binding.SalGlobalDataRegion.FirmwareType = 7U;
    ResetStub();
    ASTERIA_EXPECT(runner, "SAL GP storage outside runtime code or data is rejected",
                   HasResult(SalBootRendezvousRegistration::Register(binding, ValidInformation(), handler),
                             SalBootRendezvousRegistrationStatus::InvalidBinding,
                             SalBootRendezvousRegistrationDisposition::NotRegistered),
                   HasNoInvocation());

    information = ValidInformation();
    information.SalProcedurePhysicalAddress = 0x4010U;
    ResetStub();
    ASTERIA_EXPECT(runner, "a retained SAL entry mismatch is rejected before invocation",
                   HasResult(SalBootRendezvousRegistration::Register(ValidBinding(), information, handler),
                             SalBootRendezvousRegistrationStatus::BindingInformationMismatch,
                             SalBootRendezvousRegistrationDisposition::NotRegistered),
                   HasNoInvocation());
}

[[nodiscard]] bool HasHandlerFailure(const SalBootRendezvousHandler& handler,
                                     const SalBootRendezvousRegistrationStatus expectedStatus) noexcept
{
    ResetStub();
    const SalBootRendezvousRegistrationResult result =
        SalBootRendezvousRegistration::Register(ValidBinding(), ValidInformation(), handler);
    return HasResult(result, expectedStatus, SalBootRendezvousRegistrationDisposition::NotRegistered) &&
           HasNoInvocation();
}

void TestHandlerValidation(TestContext& runner) noexcept
{
    SalBootRendezvousHandler handler = ValidHandler();
    handler.CodePhysicalStart = 0U;
    ASTERIA_EXPECT(runner, "a zero handler code start is rejected",
                   HasHandlerFailure(handler, SalBootRendezvousRegistrationStatus::InvalidHandlerCodeExtent));

    handler = ValidHandler();
    handler.CodePhysicalStart += 8U;
    ASTERIA_EXPECT(runner, "a non-bundle-aligned handler code start is rejected",
                   HasHandlerFailure(handler, SalBootRendezvousRegistrationStatus::InvalidHandlerCodeExtent));

    handler = ValidHandler();
    handler.CodeSizeBytes = 0U;
    ASTERIA_EXPECT(runner, "an empty handler code extent is rejected",
                   HasHandlerFailure(handler, SalBootRendezvousRegistrationStatus::InvalidHandlerCodeExtent));

    handler = ValidHandler();
    handler.CodeSizeBytes = 17U;
    ASTERIA_EXPECT(runner, "a partial-bundle handler code extent is rejected",
                   HasHandlerFailure(handler, SalBootRendezvousRegistrationStatus::InvalidHandlerCodeExtent));

    handler = ValidHandler();
    handler.CodePhysicalStart = 0x7FFF'FFFF'FFFF'F000U;
    handler.CodeSizeBytes = 0x2000U;
    handler.EntryPhysicalAddress = handler.CodePhysicalStart;
    ASTERIA_EXPECT(runner, "a handler code extent crossing the physical limit is rejected",
                   HasHandlerFailure(handler, SalBootRendezvousRegistrationStatus::InvalidHandlerCodeExtent));

    handler = ValidHandler();
    handler.EntryPhysicalAddress += 8U;
    ASTERIA_EXPECT(runner, "a non-bundle-aligned handler entry is rejected",
                   HasHandlerFailure(handler, SalBootRendezvousRegistrationStatus::InvalidHandlerEntrypoint));

    handler = ValidHandler();
    handler.EntryPhysicalAddress = handler.CodePhysicalStart - 16U;
    ASTERIA_EXPECT(runner, "a handler entry before its code extent is rejected",
                   HasHandlerFailure(handler, SalBootRendezvousRegistrationStatus::InvalidHandlerEntrypoint));

    handler = ValidHandler();
    handler.EntryPhysicalAddress = handler.CodePhysicalStart + handler.CodeSizeBytes;
    ASTERIA_EXPECT(runner, "a handler entry after its code extent is rejected",
                   HasHandlerFailure(handler, SalBootRendezvousRegistrationStatus::InvalidHandlerEntrypoint));

    handler = ValidHandler();
    handler.GlobalPointerPhysicalAddress = 0U;
    ASTERIA_EXPECT(runner, "a zero handler GP is rejected",
                   HasHandlerFailure(handler, SalBootRendezvousRegistrationStatus::InvalidHandlerGlobalPointer));

    handler = ValidHandler();
    handler.GlobalPointerPhysicalAddress += 1U;
    ASTERIA_EXPECT(runner, "an unaligned handler GP is rejected",
                   HasHandlerFailure(handler, SalBootRendezvousRegistrationStatus::InvalidHandlerGlobalPointer));

    handler = ValidHandler();
    handler.GlobalPointerPhysicalAddress = Foundation::UInt64{1U} << 63U;
    ASTERIA_EXPECT(runner, "a non-physical handler GP is rejected",
                   HasHandlerFailure(handler, SalBootRendezvousRegistrationStatus::InvalidHandlerGlobalPointer));

    handler = ValidHandler();
    handler.CodePhysicalStart = 0x7FFF'FFFF'FFFF'F000U;
    handler.CodeSizeBytes = 0x1000U;
    handler.EntryPhysicalAddress = 0x7FFF'FFFF'FFFF'FFF0U;
    ResetStub();
    const SalBootRendezvousRegistrationResult boundaryResult =
        SalBootRendezvousRegistration::Register(ValidBinding(), ValidInformation(), handler);
    ASTERIA_EXPECT(runner, "the last complete physical handler bundle is accepted",
                   HasResult(boundaryResult, SalBootRendezvousRegistrationStatus::Registered,
                             SalBootRendezvousRegistrationDisposition::Registered),
                   g_invocation.Arguments[2U] == 0x7FFF'FFFF'FFFF'FFF0U);
}

void TestNativeAndFirmwareFailures(TestContext& runner) noexcept
{
    struct FirmwareCase
    {
        Foundation::Int64 FirmwareStatus;
        SalBootRendezvousRegistrationStatus ExpectedStatus;
        SalBootRendezvousRegistrationDisposition ExpectedDisposition;
    };
    constexpr FirmwareCase cases[]{
        {
            -1,
            SalBootRendezvousRegistrationStatus::FirmwareNotImplemented,
            SalBootRendezvousRegistrationDisposition::NotRegistered,
        },
        {
            -2,
            SalBootRendezvousRegistrationStatus::FirmwareInvalidArgument,
            SalBootRendezvousRegistrationDisposition::NotRegistered,
        },
        {
            -3,
            SalBootRendezvousRegistrationStatus::FirmwareCallFailed,
            SalBootRendezvousRegistrationDisposition::Indeterminate,
        },
        {
            -4,
            SalBootRendezvousRegistrationStatus::FirmwareVirtualAddressNotRegistered,
            SalBootRendezvousRegistrationDisposition::NotRegistered,
        },
        {
            1,
            SalBootRendezvousRegistrationStatus::UnexpectedFirmwareStatus,
            SalBootRendezvousRegistrationDisposition::Indeterminate,
        },
        {
            -15,
            SalBootRendezvousRegistrationStatus::UnexpectedFirmwareStatus,
            SalBootRendezvousRegistrationDisposition::Indeterminate,
        },
    };
    for (const FirmwareCase& firmwareCase : cases)
    {
        ResetStub();
        g_stubResult.Status = firmwareCase.FirmwareStatus;
        const SalBootRendezvousRegistrationResult result = RegisterValid(ValidInformation());
        ASTERIA_EXPECT(runner, "the exact SAL firmware status is retained and classified",
                       HasResult(result, firmwareCase.ExpectedStatus, firmwareCase.ExpectedDisposition,
                                 firmwareCase.FirmwareStatus),
                       g_invocation.InvocationCount == 1U);
    }

    ResetStub();
    g_stubInvocationState = 1U;
    g_stubResult.Status = -2;
    ASTERIA_EXPECT(runner, "native processor-state rejection is distinct from firmware status",
                   HasResult(RegisterValid(ValidInformation()),
                             SalBootRendezvousRegistrationStatus::UnsupportedProcessorState,
                             SalBootRendezvousRegistrationDisposition::NotRegistered),
                   g_invocation.InvocationCount == 1U);

    constexpr SalFirmwareCallResult reservedResultCases[]{
        {.Status = -3, .Result1 = 1U, .Result2 = 0U, .Result3 = 0U},
        {.Status = -3, .Result1 = 0U, .Result2 = 1U, .Result3 = 0U},
        {.Status = -3, .Result1 = 0U, .Result2 = 0U, .Result3 = 1U},
        {.Status = 0, .Result1 = 1U, .Result2 = 0U, .Result3 = 0U},
        {.Status = 0, .Result1 = 0U, .Result2 = 1U, .Result3 = 0U},
        {.Status = 0, .Result1 = 0U, .Result2 = 0U, .Result3 = 1U},
    };
    for (const SalFirmwareCallResult& reservedResult : reservedResultCases)
    {
        ResetStub();
        g_stubResult = reservedResult;
        ASTERIA_EXPECT(runner, "each nonzero reserved SAL result register makes commitment indeterminate",
                       HasResult(RegisterValid(ValidInformation()),
                                 SalBootRendezvousRegistrationStatus::InvalidFirmwareReturn,
                                 SalBootRendezvousRegistrationDisposition::Indeterminate, reservedResult.Status),
                       g_invocation.InvocationCount == 1U);
    }
}

void TestInputImmutability(TestContext& runner) noexcept
{
    const SalEntrypointBinding binding = ValidBinding();
    const SalSystemTableInformation information = ValidInformation(0x0290U);
    const SalBootRendezvousHandler handler = ValidHandler();
    Snapshot(binding, g_bindingSnapshot);
    Snapshot(information, g_informationSnapshot);
    Snapshot(handler, g_handlerSnapshot);
    ResetStub();
    const SalBootRendezvousRegistrationResult result =
        SalBootRendezvousRegistration::Register(binding, information, handler);
    ASTERIA_EXPECT(runner, "registration does not mutate any retained input",
                   HasResult(result, SalBootRendezvousRegistrationStatus::Registered,
                             SalBootRendezvousRegistrationDisposition::Registered),
                   MatchesSnapshot(binding, g_bindingSnapshot), MatchesSnapshot(information, g_informationSnapshot),
                   MatchesSnapshot(handler, g_handlerSnapshot));
}
} // namespace
} // namespace Asteria::Test

extern "C" Asteria::Platform::Ia64::Firmware::SalFirmwareCallResult
AsteriaIa64InvokePhysicalSalSetVectors(const Asteria::Platform::Ia64::Firmware::SalFirmwareCallRequest* const request,
                                       Asteria::Foundation::UInt64* const invocationState) noexcept
{
    ++Asteria::Test::g_invocation.InvocationCount;
    Asteria::Test::g_invocation.EntryPhysicalAddress = request->EntryPhysicalAddress;
    Asteria::Test::g_invocation.GlobalDataPhysicalAddress = request->GlobalDataPhysicalAddress;
    for (Asteria::Foundation::Size index{0U}; index < 8U; ++index)
    {
        Asteria::Test::g_invocation.Arguments[index] = request->Arguments[index];
    }
    *invocationState = Asteria::Test::g_stubInvocationState;
    return Asteria::Test::g_stubResult;
}

int main()
{
    TestContext runner{"SalBootRendezvousRegistrationTests"};
    Asteria::Test::TestFixedRequestBoundary(runner);
    Asteria::Test::TestSupportedRevisionEncoding(runner);
    Asteria::Test::TestRetainedInputValidation(runner);
    Asteria::Test::TestHandlerValidation(runner);
    Asteria::Test::TestNativeAndFirmwareFailures(runner);
    Asteria::Test::TestInputImmutability(runner);
    return runner.Finish();
}
#endif
