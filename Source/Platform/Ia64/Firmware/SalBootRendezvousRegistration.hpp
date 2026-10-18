// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_FIRMWARE_SAL_BOOT_RENDEZVOUS_REGISTRATION_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_FIRMWARE_SAL_BOOT_RENDEZVOUS_REGISTRATION_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Platform/Ia64/Firmware/SalEntrypointBinder.hpp"
#include "Platform/Ia64/Firmware/SalSystemTableParser.hpp"

namespace Asteria::Platform::Ia64::Firmware
{
inline constexpr Foundation::UInt64 SalProcedureFreeStorageBytes{16'384U};
inline constexpr Foundation::UInt64 SalRegistrationAdapterReserveBytes{1'024U};
inline constexpr Foundation::UInt64 SalRegistrationFreeStorageBytes{SalProcedureFreeStorageBytes +
                                                                    SalRegistrationAdapterReserveBytes};

/// Describes the physical OS_BOOT_RENDEZ entrypoint and the complete code
/// extent that authorizes its first instruction bundle. The extent is a
/// structural range only; registration never reads or authenticates its bytes.
struct SalBootRendezvousHandler
{
    Foundation::UInt64 EntryPhysicalAddress;
    Foundation::UInt64 GlobalPointerPhysicalAddress;
    Foundation::UInt64 CodePhysicalStart;
    Foundation::UInt64 CodeSizeBytes;
};

/// Models the four integer result registers returned by the SAL procedure ABI.
/// The three reserved values are checked by the registration boundary and are
/// not retained after the call.
struct SalFirmwareCallResult
{
    Foundation::Int64 Status;
    Foundation::UInt64 Result1;
    Foundation::UInt64 Result2;
    Foundation::UInt64 Result3;
};

/// Fixed-layout request consumed by the native SAL bridge. Passing one request
/// pointer keeps the C boundary within the eight register parameter slots; the
/// bridge loads the raw entry, GP, and all eight SAL arguments from this block.
struct SalFirmwareCallRequest
{
    Foundation::UInt64 EntryPhysicalAddress;
    Foundation::UInt64 GlobalDataPhysicalAddress;
    Foundation::UInt64 Arguments[8U];
};

enum class SalBootRendezvousRegistrationStatus : Foundation::UInt32
{
    Registered,
    InvalidSalInformation,
    InvalidBinding,
    BindingInformationMismatch,
    InvalidHandlerCodeExtent,
    InvalidHandlerEntrypoint,
    InvalidHandlerGlobalPointer,
    UnsupportedProcessorState,
    FirmwareNotImplemented,
    FirmwareInvalidArgument,
    FirmwareCallFailed,
    FirmwareVirtualAddressNotRegistered,
    InvalidFirmwareReturn,
    UnexpectedFirmwareStatus,
};

/// States whether this call's handler must be treated as installed. Status
/// explains the diagnostic; this disposition controls handler lifetime.
/// NotRegistered is returned before firmware invocation, for native rejection,
/// and for well-formed SAL -1, -2, or -4. Registered requires a well-formed SAL
/// status zero. A malformed reserved result, SAL -3, or any unexpected status
/// is Indeterminate because the side effect may already have committed.
enum class SalBootRendezvousRegistrationDisposition : Foundation::UInt8
{
    NotRegistered,
    Registered,
    Indeterminate,
};

struct SalBootRendezvousRegistrationResult
{
    SalBootRendezvousRegistrationStatus Status;
    SalBootRendezvousRegistrationDisposition Disposition;
    Foundation::UInt8 Reserved[3U];
    Foundation::Int64 FirmwareStatus;
};

/// Registers the single OS_BOOT_RENDEZ vector through SAL_SET_VECTORS. The retained inputs and
/// handler fields must remain stable throughout this call. The handler length argument is always
/// zero: for SAL 2.8/2.9 this is the no-length form, and for SAL 3.x it is the
/// no-length/no-checksum length_cs form. The secondary address, GP, and length arguments are zero.
/// When Disposition is Registered or Indeterminate, the owner must keep every reachable first-level
/// handler bundle resident, immutable, and physically executable, and keep all data reachable
/// through the submitted GP resident, immutable where required by the handler, and physically
/// accessible. This ownership lasts until the submitted vector is replaced or invalidated and every
/// AP rendezvous use is quiesced. It is a lifetime contract, not code hashing or artifact
/// provenance. NotRegistered applies only to this attempted handler; it does not assert that no
/// earlier vector remains installed. Preconditions not representable by the value arguments: -
/// execution is at CPL0 with PSR.vm zero in the inherited UEFI/SAL physical environment; - PSR.bn
/// is one and the SAL-required PSR.is, PSR.da, PSR.dd, PSR.ss, PSR.ri, PSR.ed, and PSR.ia fields
/// are zero; - after all outer caller frames, at least 17,408 bytes remain free in both the memory
/// stack and RSE backing store at Register entry; the 1,024-byte adapter reserve covers the
/// complete verified Register/bridge live chain and RNAT overhead while leaving SAL's 16 KiB
/// minimum at SAL_PROC entry; - the call is serialized, cannot be preempted, and stays on this
/// processor. The native bridge verifies the architecturally readable PSR.be, PSR.dt, PSR.rt, and
/// PSR.it fields and reports UnsupportedProcessorState when any is nonzero. The other
/// processor-state preconditions cannot be read by `mov = psr`; violating CPL0 or PSR.vm can fault
/// instead of returning. Allocation: - Does not allocate.
class SalBootRendezvousRegistration final
{
public:
    [[nodiscard]] static SalBootRendezvousRegistrationResult Register(const SalEntrypointBinding& binding,
                                                                      const SalSystemTableInformation& salInformation,
                                                                      const SalBootRendezvousHandler& handler) noexcept;

private:
    SalBootRendezvousRegistration() = delete;
};

static_assert(sizeof(SalBootRendezvousHandler) == 32U);
static_assert(alignof(SalBootRendezvousHandler) == 8U);
static_assert(sizeof(SalFirmwareCallResult) == 32U);
static_assert(alignof(SalFirmwareCallResult) == 8U);
static_assert(__builtin_offsetof(SalFirmwareCallResult, Status) == 0U);
static_assert(__builtin_offsetof(SalFirmwareCallResult, Result1) == 8U);
static_assert(__builtin_offsetof(SalFirmwareCallResult, Result2) == 16U);
static_assert(__builtin_offsetof(SalFirmwareCallResult, Result3) == 24U);
static_assert(sizeof(SalFirmwareCallRequest) == 80U);
static_assert(alignof(SalFirmwareCallRequest) == 8U);
static_assert(__builtin_offsetof(SalFirmwareCallRequest, EntryPhysicalAddress) == 0U);
static_assert(__builtin_offsetof(SalFirmwareCallRequest, GlobalDataPhysicalAddress) == 8U);
static_assert(__builtin_offsetof(SalFirmwareCallRequest, Arguments) == 16U);
static_assert(SalRegistrationFreeStorageBytes == 17'408U);
static_assert(sizeof(SalBootRendezvousRegistrationStatus) == 4U);
static_assert(sizeof(SalBootRendezvousRegistrationDisposition) == 1U);
static_assert(sizeof(SalBootRendezvousRegistrationResult) == 16U);
static_assert(alignof(SalBootRendezvousRegistrationResult) == 8U);
static_assert(__builtin_offsetof(SalBootRendezvousRegistrationResult, Status) == 0U);
static_assert(__builtin_offsetof(SalBootRendezvousRegistrationResult, Disposition) == 4U);
static_assert(__builtin_offsetof(SalBootRendezvousRegistrationResult, Reserved) == 5U);
static_assert(__builtin_offsetof(SalBootRendezvousRegistrationResult, FirmwareStatus) == 8U);
} // Namespace Asteria::Platform::Ia64::Firmware

#endif // ASTERIA_PLATFORM_IA64_FIRMWARE_SAL_BOOT_RENDEZVOUS_REGISTRATION_HPP_INCLUDED
