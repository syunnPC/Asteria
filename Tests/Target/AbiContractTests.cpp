// SPDX-License-Identifier: Apache-2.0
#include "Abi/SystemCallAbi.hpp"
#include "Foundation/Types.hpp"
#include "Hal/Interrupt/IInterruptSource.hpp"
#include "Hal/Time/ILocalTimer.hpp"
#include "Kernel/Device/DeviceManager.hpp"
#include "Kernel/Device/DeviceRegion.hpp"
#include "Kernel/Device/IommuDomain.hpp"
#include "Kernel/Device/PciFunction.hpp"
#include "Kernel/Device/PciFunctionCatalog.hpp"
#include "Kernel/Interrupt/Interrupt.hpp"
#include "Kernel/Ipc/IpcDeliveryCodec.hpp"
#include "Kernel/Memory/BootstrapPageAllocator.hpp"
#include "Kernel/Memory/IUserMemoryAccess.hpp"
#include "Kernel/Memory/InitialMappingPlan.hpp"
#include "Kernel/Memory/MemoryMapper.hpp"
#include "Kernel/Memory/PagerFaultDispatcher.hpp"
#include "Kernel/Memory/FaultTokenFactory.hpp"
#include "Kernel/Memory/PagerProtocol.hpp"
#include "Kernel/Ipc/Endpoint.hpp"
#include "Kernel/Ipc/MessageCodec.hpp"
#include "Kernel/Ipc/Notification.hpp"
#include "Kernel/Ipc/ReplyToken.hpp"
#include "Kernel/Ipc/SynchronousReplyDispatcher.hpp"
#include "Kernel/Ipc/WaitSet.hpp"
#include "Kernel/Object/CapabilityTransfer.hpp"
#include "Kernel/Object/CapabilityTypes.hpp"
#include "Kernel/Resource/ResourceDomain.hpp"
#include "Kernel/System/SystemCallDispatcher.hpp"
#include "Kernel/System/IpcSystemCallDispatcher.hpp"
#include "Kernel/System/SystemCallContext.hpp"
#include "Kernel/System/SystemCallIngress.hpp"
#include "Kernel/System/SystemCallScratch.hpp"
#include "Kernel/Time/Timer.hpp"
#include "Kernel/Time/TimerQueue.hpp"
#include "Kernel/Time/ProcessorDeadlineController.hpp"
#include "Platform/Amd64/Firmware/Amd64LoadedImageLayout.hpp"
#include "Platform/Amd64/Memory/BootstrapMappingPlan.hpp"
#include "Platform/Amd64/Memory/InitialAddressSpacePreparation.hpp"
#include "Platform/Amd64/Memory/InitialPagingCapabilityCapture.hpp"
#include "Platform/Amd64/Memory/InitialPageTableBuilder.hpp"
#include "Platform/Amd64/Memory/InitialPageTablePreparation.hpp"
#include "Platform/Amd64/Memory/InitialPageTableVerifier.hpp"
#include "Platform/Amd64/System/Amd64SystemCallRegisterCodec.hpp"
#include "Platform/Amd64/Time/Amd64LocalTimer.hpp"
#include "Platform/Ia64/Firmware/PalBootstrapCapabilityCapture.hpp"
#include "Platform/Ia64/Firmware/PalStaticCallAdapter.hpp"
#include "Platform/Ia64/Firmware/SalSystemTableCapture.hpp"
#include "Platform/Ia64/Firmware/SalSystemTableLocator.hpp"
#include "Platform/Ia64/Firmware/SalSystemTableParser.hpp"
#include "Platform/Ia64/Memory/InitialAddressSpacePreparation.hpp"
#include "Platform/Ia64/Memory/InitialExecutionContextSwitcher.hpp"
#include "Platform/Ia64/Memory/InitialInterruptVectorTableInstaller.hpp"
#include "Platform/Ia64/Memory/InitialTranslationEncoder.hpp"
#include "Platform/Ia64/Memory/InitialTranslationInstaller.hpp"
#include "Platform/Ia64/Memory/InitialTranslationResourcePlan.hpp"
#include "Platform/Ia64/Memory/Ia64RuntimeTranslationLedger.hpp"
#include "Platform/Ia64/Memory/PalVirtualMemoryDecoder.hpp"
#include "Platform/Ia64/System/Ia64SystemCallRegisterCodec.hpp"
#include "Platform/Ia64/Time/Ia64LocalTimer.hpp"
#include "Platform/Synchronization/AtomicReferenceCount.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

#if !defined(__BYTE_ORDER__) || !defined(__ORDER_LITTLE_ENDIAN__)
#error "The compiler must expose byte-order macros"
#endif

#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "Asteria targets must be little-endian"
#endif

#if !defined(__CHAR_BIT__) || (__CHAR_BIT__ != 8)
#error "Asteria targets require eight-bit bytes"
#endif

#if defined(__CHAR_UNSIGNED__)
#error "Asteria target profiles require signed plain char"
#endif

#if !defined(__LP64__)
#error "Asteria targets require the LP64 data model"
#endif

#if (defined(ASTERIA_TARGET_IA64) && defined(ASTERIA_TARGET_AMD64)) ||                                                 \
    (!defined(ASTERIA_TARGET_IA64) && !defined(ASTERIA_TARGET_AMD64))
#error "Exactly one supported target profile must be selected"
#endif

#if defined(ASTERIA_TARGET_IA64)
#if ASTERIA_TARGET_IA64 != 1
#error "The IA-64 target profile macro must have value one"
#endif
#if !defined(__ia64__)
#error "The IA-64 profile requires an IA-64 compiler"
#endif
#endif

#if defined(ASTERIA_TARGET_AMD64)
#if ASTERIA_TARGET_AMD64 != 1
#error "The AMD64 target profile macro must have value one"
#endif
#if !defined(__x86_64__)
#error "The AMD64 profile requires an x86-64 compiler"
#endif
#endif

namespace Asteria::Test
{
constinit Kernel::Memory::BootstrapPageAllocator g_bootstrapPageAllocatorProbe;
constinit Kernel::Memory::InitialMappingPlan g_initialMappingPlanProbe;
constinit Platform::Amd64::Memory::InitialPageTablePreparation g_initialPageTablePreparationProbe;
constinit Platform::Amd64::Memory::InitialAddressSpacePreparation g_amd64InitialAddressSpacePreparationProbe;
constinit Platform::Ia64::Memory::InitialTranslationResourcePlan g_initialTranslationResourcePlanProbe;
constinit Platform::Ia64::Memory::InitialAddressSpacePreparation g_initialAddressSpacePreparationProbe;
constinit Platform::Ia64::Memory::Ia64RuntimeTranslationLedger g_ia64RuntimeTranslationLedgerProbe;

static_assert(static_cast<Foundation::UInt16>(Kernel::Object::CapabilityRight::Inspect) == Abi::CapabilityRightInspect);
static_assert(static_cast<Foundation::UInt16>(Kernel::Object::CapabilityRight::Invoke) == Abi::CapabilityRightInvoke);
static_assert(static_cast<Foundation::UInt16>(Kernel::Object::CapabilityRight::Read) == Abi::CapabilityRightRead);
static_assert(static_cast<Foundation::UInt16>(Kernel::Object::CapabilityRight::Write) == Abi::CapabilityRightWrite);
static_assert(static_cast<Foundation::UInt16>(Kernel::Object::CapabilityRight::Map) == Abi::CapabilityRightMap);
static_assert(static_cast<Foundation::UInt16>(Kernel::Object::CapabilityRight::Wait) == Abi::CapabilityRightWait);
static_assert(static_cast<Foundation::UInt16>(Kernel::Object::CapabilityRight::Signal) == Abi::CapabilityRightSignal);
static_assert(static_cast<Foundation::UInt16>(Kernel::Object::CapabilityRight::Transfer) ==
              Abi::CapabilityRightTransfer);
static_assert(static_cast<Foundation::UInt16>(Kernel::Object::CapabilityRight::Duplicate) ==
              Abi::CapabilityRightDuplicate);
static_assert(static_cast<Foundation::UInt16>(Kernel::Object::CapabilityRight::Derive) == Abi::CapabilityRightDerive);
static_assert(static_cast<Foundation::UInt16>(Kernel::Object::CapabilityRight::Revoke) == Abi::CapabilityRightRevoke);
static_assert(static_cast<Foundation::UInt16>(Kernel::Object::CapabilityRight::Manage) == Abi::CapabilityRightManage);
static_assert(static_cast<Foundation::UInt16>(Kernel::Object::CapabilityRight::Debug) == Abi::CapabilityRightDebug);
static_assert(static_cast<Foundation::UInt16>(Kernel::Object::CapabilityRight::Code) == Abi::CapabilityRightCode);
static_assert(Abi::ObjectCreateThreadPointerOffset == 56U);
static_assert(sizeof(Foundation::Size) == 8U);
static_assert(sizeof(Foundation::UIntPointer) == 8U);
static_assert(sizeof(void*) == 8U);
static_assert(sizeof(Kernel::Object::CapabilityHandle) == 16U);
static_assert(alignof(Kernel::Object::CapabilityHandle) == 8U);
static_assert(sizeof(Kernel::Object::RevocationDomainId) == 8U);
static_assert(sizeof(Kernel::Device::DeviceRegionId) == 16U);
static_assert(alignof(Kernel::Device::DeviceRegionId) == 8U);
static_assert(sizeof(Kernel::Device::DeviceRegionKind) == 1U);
static_assert(sizeof(Kernel::Device::DeviceRegionPermissions) == 1U);
static_assert(sizeof(Kernel::Device::DeviceAccessWidth) == 1U);
static_assert(sizeof(Kernel::Device::DeviceAccessWidths) == 1U);
static_assert(sizeof(Kernel::Device::DeviceAccessOperation) == 1U);
static_assert(sizeof(Kernel::Device::DeviceRegionState) == 1U);
static_assert(sizeof(Kernel::Device::DeviceRegionError) == 1U);
static_assert(sizeof(Kernel::Device::DeviceRegionDescriptor) == 24U);
static_assert(alignof(Kernel::Device::DeviceRegionDescriptor) == 8U);
static_assert(sizeof(Kernel::Device::DeviceRegionFailure) == 8U);
static_assert(alignof(Kernel::Device::DeviceRegionFailure) == 1U);
static_assert(sizeof(Kernel::Device::DeviceRegionSnapshot) == 72U);
static_assert(alignof(Kernel::Device::DeviceRegionSnapshot) == 8U);
static_assert(sizeof(Kernel::Device::DeviceManagerState) == 1U);
static_assert(sizeof(Kernel::Device::DeviceManagerError) == 1U);
static_assert(sizeof(Kernel::Device::DeviceRegionCreateRequest) == 72U);
static_assert(alignof(Kernel::Device::DeviceRegionCreateRequest) == 8U);
static_assert(sizeof(Kernel::Device::DeviceRegionCreation) == 40U);
static_assert(alignof(Kernel::Device::DeviceRegionCreation) == 8U);
static_assert(sizeof(Kernel::Device::DeviceManagerFailure) == 24U);
static_assert(alignof(Kernel::Device::DeviceManagerFailure) == 8U);
static_assert(sizeof(Kernel::Device::DeviceManagerSnapshot) == 40U);
static_assert(alignof(Kernel::Device::DeviceManagerSnapshot) == 8U);
static_assert(sizeof(Kernel::Device::PciFunctionManagerError) == 1U);
static_assert(sizeof(Kernel::Device::PciFunctionCreateRequest) == 56U);
static_assert(alignof(Kernel::Device::PciFunctionCreateRequest) == 8U);
static_assert(sizeof(Kernel::Device::PciFunctionCreation) == 32U);
static_assert(alignof(Kernel::Device::PciFunctionCreation) == 8U);
static_assert(sizeof(Kernel::Device::PciFunctionManagerFailure) == 40U);
static_assert(alignof(Kernel::Device::PciFunctionManagerFailure) == 8U);
static_assert(sizeof(Kernel::Device::PciFunctionManagerSnapshot) == 48U);
static_assert(alignof(Kernel::Device::PciFunctionManagerSnapshot) == 8U);
static_assert(
    sizeof(Foundation::Result<Kernel::Device::PciFunctionCreation, Kernel::Device::PciFunctionManagerFailure>) == 80U);
static_assert(
    alignof(Foundation::Result<Kernel::Device::PciFunctionCreation, Kernel::Device::PciFunctionManagerFailure>) == 8U);
static_assert(sizeof(Foundation::Result<Kernel::Object::ObjectId, Kernel::Device::PciFunctionManagerFailure>) == 56U);
static_assert(alignof(Foundation::Result<Kernel::Object::ObjectId, Kernel::Device::PciFunctionManagerFailure>) == 8U);
static_assert(sizeof(Kernel::Device::DmaAddress) == 8U);
static_assert(alignof(Kernel::Device::DmaAddress) == 8U);
static_assert(sizeof(Kernel::Device::DmaPermissions) == 1U);
static_assert(sizeof(Kernel::Device::IommuDomainId) == 16U);
static_assert(alignof(Kernel::Device::IommuDomainId) == 8U);
static_assert(sizeof(Kernel::Device::IommuMappingToken) == 16U);
static_assert(alignof(Kernel::Device::IommuMappingToken) == 8U);
static_assert(sizeof(Kernel::Device::IommuAttachmentToken) == 16U);
static_assert(alignof(Kernel::Device::IommuAttachmentToken) == 8U);
static_assert(sizeof(Kernel::Device::IommuDomainConfiguration) == 8U);
static_assert(sizeof(Kernel::Device::IommuMappingRequest) == 56U);
static_assert(alignof(Kernel::Device::IommuMappingRequest) == 8U);
static_assert(sizeof(Kernel::Device::IommuMappingReceipt) == 64U);
static_assert(alignof(Kernel::Device::IommuMappingReceipt) == 8U);
static_assert(sizeof(Kernel::Device::IommuAttachmentReceipt) == 16U);
static_assert(alignof(Kernel::Device::IommuAttachmentReceipt) == 8U);
static_assert(sizeof(Kernel::Device::IommuDomainFailure) == 8U);
static_assert(alignof(Kernel::Device::IommuDomainFailure) == 1U);
static_assert(sizeof(Kernel::Device::IommuDomainSnapshot) == 72U);
static_assert(alignof(Kernel::Device::IommuDomainSnapshot) == 8U);
static_assert(
    sizeof(Asteria::Foundation::Result<Kernel::Device::IommuMappingReceipt, Kernel::Device::IommuDomainFailure>) ==
    80U);
static_assert(
    alignof(Asteria::Foundation::Result<Kernel::Device::IommuMappingReceipt, Kernel::Device::IommuDomainFailure>) ==
    8U);
static_assert(
    sizeof(Asteria::Foundation::Result<Kernel::Device::IommuAttachmentReceipt, Kernel::Device::IommuDomainFailure>) ==
    32U);
static_assert(
    alignof(Asteria::Foundation::Result<Kernel::Device::IommuAttachmentReceipt, Kernel::Device::IommuDomainFailure>) ==
    8U);
static_assert(sizeof(Asteria::Hal::Device::IommuBackendMapRequest) == 32U);
static_assert(alignof(Asteria::Hal::Device::IommuBackendMapRequest) == 8U);
static_assert(sizeof(Hal::Device::PciConfigurationReadError) == 1U);
static_assert(sizeof(Kernel::Device::PciFunctionAddress) == 6U);
static_assert(alignof(Kernel::Device::PciFunctionAddress) == 2U);
static_assert(sizeof(Kernel::Device::PciBusRange) == 8U);
static_assert(alignof(Kernel::Device::PciBusRange) == 2U);
static_assert(sizeof(Kernel::Device::PciBusRangeView) == 16U);
static_assert(alignof(Kernel::Device::PciBusRangeView) == 8U);
static_assert(sizeof(Kernel::Device::PciFunctionCatalogState) == 1U);
static_assert(sizeof(Kernel::Device::PciFunctionCatalogError) == 1U);
static_assert(sizeof(Kernel::Device::PciFunctionDescriptor) == 16U);
static_assert(alignof(Kernel::Device::PciFunctionDescriptor) == 2U);
static_assert(sizeof(Kernel::Device::PciFunctionCatalogFailure) == 16U);
static_assert(alignof(Kernel::Device::PciFunctionCatalogFailure) == 2U);
static_assert(sizeof(Kernel::Device::PciFunctionCatalogSnapshot) == 40U);
static_assert(alignof(Kernel::Device::PciFunctionCatalogSnapshot) == 8U);
static_assert(
    sizeof(Foundation::Result<Kernel::Device::PciFunctionDescriptor, Kernel::Device::PciFunctionCatalogFailure>) ==
    34U);
static_assert(
    alignof(Foundation::Result<Kernel::Device::PciFunctionDescriptor, Kernel::Device::PciFunctionCatalogFailure>) ==
    2U);
static_assert(sizeof(Kernel::Device::PciFunctionId) == 16U);
static_assert(alignof(Kernel::Device::PciFunctionId) == 8U);
static_assert(sizeof(Kernel::Device::PciFunctionAssignmentToken) == 16U);
static_assert(alignof(Kernel::Device::PciFunctionAssignmentToken) == 8U);
static_assert(sizeof(Kernel::Device::PciFunctionState) == 1U);
static_assert(sizeof(Kernel::Device::PciFunctionError) == 1U);
static_assert(sizeof(Kernel::Device::PciFunctionFailure) == 8U);
static_assert(alignof(Kernel::Device::PciFunctionFailure) == 1U);
static_assert(sizeof(Kernel::Device::PciFunctionAssignmentReceipt) == 32U);
static_assert(alignof(Kernel::Device::PciFunctionAssignmentReceipt) == 8U);
static_assert(sizeof(Kernel::Device::PciFunctionSnapshot) == 48U);
static_assert(alignof(Kernel::Device::PciFunctionSnapshot) == 8U);
static_assert(
    sizeof(Foundation::Result<Kernel::Device::PciFunctionAssignmentReceipt, Kernel::Device::PciFunctionFailure>) ==
    48U);
static_assert(
    alignof(Foundation::Result<Kernel::Device::PciFunctionAssignmentReceipt, Kernel::Device::PciFunctionFailure>) ==
    8U);
static_assert(sizeof(Kernel::Memory::PagerProtocolError) == 1U);
static_assert(sizeof(Kernel::Memory::PagerFaultNotification) == 104U);
static_assert(alignof(Kernel::Memory::PagerFaultNotification) == 8U);
static_assert(sizeof(Kernel::Memory::PagerFaultDispatcherError) == 1U);
static_assert(sizeof(Kernel::Memory::PagerFaultDispatcherFailure) == 24U);
static_assert(alignof(Kernel::Memory::PagerFaultDispatcherFailure) == 8U);
static_assert(sizeof(Kernel::Memory::PagerFaultDispatcherReceipt) == 56U);
static_assert(alignof(Kernel::Memory::PagerFaultDispatcherReceipt) == 8U);
static_assert(sizeof(Kernel::Memory::PagerFaultDispatcherSnapshot) == 64U);
static_assert(alignof(Kernel::Memory::PagerFaultDispatcherSnapshot) == 8U);
static_assert(sizeof(Kernel::Memory::FaultTokenFactoryError) == 1U);
static_assert(sizeof(Kernel::Memory::FaultTokenCreateRequest) == 112U);
static_assert(alignof(Kernel::Memory::FaultTokenCreateRequest) == 8U);
static_assert(sizeof(Kernel::Memory::FaultTokenCreation) == 32U);
static_assert(alignof(Kernel::Memory::FaultTokenCreation) == 8U);
static_assert(sizeof(Kernel::Memory::FaultTokenFactoryFailure) == 16U);
static_assert(alignof(Kernel::Memory::FaultTokenFactoryFailure) == 8U);
static_assert(sizeof(Kernel::Memory::FaultTokenFactorySnapshot) == 48U);
static_assert(alignof(Kernel::Memory::FaultTokenFactorySnapshot) == 8U);
static_assert(sizeof(Hal::Interrupt::InterruptSourceError) == 1U);
static_assert(sizeof(Kernel::Interrupt::InterruptState) == 1U);
static_assert(sizeof(Kernel::Interrupt::InterruptCompletion) == 1U);
static_assert(sizeof(Kernel::Interrupt::InterruptError) == 1U);
static_assert(sizeof(Kernel::Interrupt::InterruptEventToken) == 16U);
static_assert(alignof(Kernel::Interrupt::InterruptEventToken) == 8U);
static_assert(sizeof(Kernel::Interrupt::InterruptFailure) == 16U);
static_assert(alignof(Kernel::Interrupt::InterruptFailure) == 1U);
static_assert(sizeof(Kernel::Interrupt::InterruptDelivery) == 24U);
static_assert(alignof(Kernel::Interrupt::InterruptDelivery) == 8U);
static_assert(sizeof(Kernel::Interrupt::InterruptSnapshot) == 48U);
static_assert(alignof(Kernel::Interrupt::InterruptSnapshot) == 8U);
static_assert(sizeof(Foundation::Result<Kernel::Interrupt::InterruptDelivery, Kernel::Interrupt::InterruptFailure>) ==
              48U);
static_assert(alignof(Foundation::Result<Kernel::Interrupt::InterruptDelivery, Kernel::Interrupt::InterruptFailure>) ==
              8U);
static_assert(sizeof(Foundation::Result<Kernel::Interrupt::InterruptSnapshot, Kernel::Interrupt::InterruptFailure>) ==
              72U);
static_assert(alignof(Foundation::Result<Kernel::Interrupt::InterruptSnapshot, Kernel::Interrupt::InterruptFailure>) ==
              8U);
static_assert(sizeof(Kernel::Ipc::NotificationState) == 1U);
static_assert(sizeof(Kernel::Ipc::NotificationError) == 1U);
static_assert(sizeof(Kernel::Ipc::NotificationId) == 16U);
static_assert(alignof(Kernel::Ipc::NotificationId) == 8U);
static_assert(sizeof(Kernel::Ipc::NotificationClaimToken) == 16U);
static_assert(alignof(Kernel::Ipc::NotificationClaimToken) == 8U);
static_assert(sizeof(Kernel::Ipc::NotificationFailure) == 8U);
static_assert(alignof(Kernel::Ipc::NotificationFailure) == 1U);
static_assert(sizeof(Kernel::Ipc::NotificationSnapshot) == 16U);
static_assert(alignof(Kernel::Ipc::NotificationSnapshot) == 8U);
static_assert(sizeof(Foundation::Result<Kernel::Ipc::NotificationClaimToken, Kernel::Ipc::NotificationFailure>) == 32U);
static_assert(alignof(Foundation::Result<Kernel::Ipc::NotificationClaimToken, Kernel::Ipc::NotificationFailure>) == 8U);
static_assert(sizeof(Foundation::Result<Kernel::Ipc::NotificationSnapshot, Kernel::Ipc::NotificationFailure>) == 32U);
static_assert(alignof(Foundation::Result<Kernel::Ipc::NotificationSnapshot, Kernel::Ipc::NotificationFailure>) == 8U);
static_assert(sizeof(Kernel::Ipc::WaitSetState) == 1U);
static_assert(sizeof(Kernel::Ipc::WaitSetError) == 1U);
static_assert(sizeof(Kernel::Ipc::WaitSetRegistrationToken) == 16U);
static_assert(alignof(Kernel::Ipc::WaitSetRegistrationToken) == 8U);
static_assert(sizeof(Kernel::Ipc::WaitSetClaimToken) == 16U);
static_assert(alignof(Kernel::Ipc::WaitSetClaimToken) == 8U);
static_assert(sizeof(Kernel::Ipc::WaitSetFailure) == 16U);
static_assert(alignof(Kernel::Ipc::WaitSetFailure) == 1U);
static_assert(sizeof(Kernel::Ipc::WaitSetRegistrationReceipt) == 40U);
static_assert(alignof(Kernel::Ipc::WaitSetRegistrationReceipt) == 8U);
static_assert(sizeof(Kernel::Ipc::WaitSetNotificationClaim) == 56U);
static_assert(alignof(Kernel::Ipc::WaitSetNotificationClaim) == 8U);
static_assert(sizeof(Kernel::Ipc::WaitSetSnapshot) == 24U);
static_assert(alignof(Kernel::Ipc::WaitSetSnapshot) == 8U);
static_assert(sizeof(Foundation::Result<Kernel::Ipc::WaitSetRegistrationReceipt, Kernel::Ipc::WaitSetFailure>) == 64U);
static_assert(alignof(Foundation::Result<Kernel::Ipc::WaitSetRegistrationReceipt, Kernel::Ipc::WaitSetFailure>) == 8U);
static_assert(sizeof(Foundation::Result<Kernel::Ipc::WaitSetNotificationClaim, Kernel::Ipc::WaitSetFailure>) == 80U);
static_assert(alignof(Foundation::Result<Kernel::Ipc::WaitSetNotificationClaim, Kernel::Ipc::WaitSetFailure>) == 8U);
static_assert(sizeof(Foundation::Result<Kernel::Ipc::WaitSetSnapshot, Kernel::Ipc::WaitSetFailure>) == 48U);
static_assert(alignof(Foundation::Result<Kernel::Ipc::WaitSetSnapshot, Kernel::Ipc::WaitSetFailure>) == 8U);
static_assert(sizeof(Kernel::Time::TimerState) == 1U);
static_assert(sizeof(Kernel::Time::TimerError) == 1U);
static_assert(sizeof(Kernel::Time::TimerToken) == 16U);
static_assert(alignof(Kernel::Time::TimerToken) == 8U);
static_assert(sizeof(Kernel::Time::TimerFailure) == 16U);
static_assert(alignof(Kernel::Time::TimerFailure) == 8U);
static_assert(sizeof(Kernel::Time::TimerSnapshot) == 32U);
static_assert(alignof(Kernel::Time::TimerSnapshot) == 8U);
static_assert(sizeof(Kernel::Time::TimerArmReceipt) == 40U);
static_assert(alignof(Kernel::Time::TimerArmReceipt) == 8U);
static_assert(sizeof(Kernel::Time::TimerExpiration) == 32U);
static_assert(alignof(Kernel::Time::TimerExpiration) == 8U);
static_assert(sizeof(Foundation::Result<Kernel::Time::TimerArmReceipt, Kernel::Time::TimerFailure>) == 64U);
static_assert(alignof(Foundation::Result<Kernel::Time::TimerArmReceipt, Kernel::Time::TimerFailure>) == 8U);
static_assert(sizeof(Kernel::Time::TimerId) == 16U);
static_assert(alignof(Kernel::Time::TimerId) == 8U);
static_assert(sizeof(Kernel::Time::TimerQueueError) == 1U);
static_assert(sizeof(Kernel::Time::TimerQueueFailure) == 32U);
static_assert(alignof(Kernel::Time::TimerQueueFailure) == 8U);
static_assert(sizeof(Kernel::Time::TimerQueueArmReceipt) == 48U);
static_assert(alignof(Kernel::Time::TimerQueueArmReceipt) == 8U);
static_assert(sizeof(Kernel::Time::TimerQueueCancelReceipt) == 40U);
static_assert(alignof(Kernel::Time::TimerQueueCancelReceipt) == 8U);
static_assert(sizeof(Kernel::Time::TimerQueueState) == 24U);
static_assert(alignof(Kernel::Time::TimerQueueState) == 8U);
static_assert(sizeof(Kernel::Time::TimerQueueClaimToken) == 16U);
static_assert(alignof(Kernel::Time::TimerQueueClaimToken) == 8U);
static_assert(sizeof(Kernel::Time::TimerQueueExpirationClaim) == 80U);
static_assert(alignof(Kernel::Time::TimerQueueExpirationClaim) == 8U);
static_assert(sizeof(Hal::Time::LocalTimerError) == 1U);
static_assert(sizeof(Platform::Amd64::Time::Amd64LocalTimerState) == 1U);
static_assert(sizeof(Platform::Amd64::Time::Amd64LocalTimerInitializationError) == 1U);
static_assert(sizeof(Platform::Amd64::Time::Amd64LocalTimerSnapshot) == 16U);
static_assert(alignof(Platform::Amd64::Time::Amd64LocalTimerSnapshot) == 8U);
static_assert(sizeof(Platform::Ia64::Time::Ia64LocalTimerState) == 1U);
static_assert(sizeof(Platform::Ia64::Time::Ia64LocalTimerInitializationError) == 1U);
static_assert(sizeof(Platform::Ia64::Time::Ia64LocalTimerSnapshot) == 16U);
static_assert(alignof(Platform::Ia64::Time::Ia64LocalTimerSnapshot) == 8U);
static_assert(sizeof(Kernel::Time::ProcessorDeadlineState) == 1U);
static_assert(sizeof(Kernel::Time::ProcessorDeadlineError) == 1U);
static_assert(sizeof(Kernel::Time::ProcessorDeadlineSource) == 1U);
static_assert(sizeof(Kernel::Time::ProcessorDeadlineSources) == 24U);
static_assert(alignof(Kernel::Time::ProcessorDeadlineSources) == 8U);
static_assert(sizeof(Kernel::Time::ProcessorDeadlineFailure) == 24U);
static_assert(alignof(Kernel::Time::ProcessorDeadlineFailure) == 8U);
static_assert(sizeof(Kernel::Time::ProcessorDeadlineReceipt) == 24U);
static_assert(alignof(Kernel::Time::ProcessorDeadlineReceipt) == 8U);
static_assert(sizeof(Kernel::Time::ProcessorDeadlineSnapshot) == 16U);
static_assert(alignof(Kernel::Time::ProcessorDeadlineSnapshot) == 8U);
static_assert(
    sizeof(Foundation::Result<Kernel::Time::ProcessorDeadlineReceipt, Kernel::Time::ProcessorDeadlineFailure>) == 56U);
static_assert(
    alignof(Foundation::Result<Kernel::Time::ProcessorDeadlineReceipt, Kernel::Time::ProcessorDeadlineFailure>) == 8U);
static_assert(sizeof(Kernel::Object::CapabilityDescriptor) == 48U);
static_assert(alignof(Kernel::Object::CapabilityDescriptor) == 8U);
static_assert(sizeof(Kernel::Object::CapabilityTransferRequest) == 32U);
static_assert(alignof(Kernel::Object::CapabilityTransferRequest) == 8U);
static_assert(sizeof(Kernel::Object::CapabilityTransferRequestView) == 16U);
static_assert(alignof(Kernel::Object::CapabilityTransferRequestView) == 8U);
static_assert(sizeof(Kernel::Object::CapabilityTransferBatch) == 72U);
static_assert(alignof(Kernel::Object::CapabilityTransferBatch) == 8U);
static_assert(sizeof(Foundation::Result<Kernel::Object::CapabilityTransferBatch, Kernel::Object::CapabilityError>) ==
              80U);
static_assert(alignof(Foundation::Result<Kernel::Object::CapabilityTransferBatch, Kernel::Object::CapabilityError>) ==
              8U);
static_assert(sizeof(Kernel::Ipc::InterfaceIdentifier) == 16U);
static_assert(alignof(Kernel::Ipc::InterfaceIdentifier) == 8U);
static_assert(sizeof(Kernel::Ipc::VariablePayloadDescriptor) == 8U);
static_assert(sizeof(Kernel::Ipc::CapabilityAttachment) == 32U);
static_assert(alignof(Kernel::Ipc::CapabilityAttachment) == 8U);
static_assert(sizeof(Kernel::Ipc::MessageEnvelope) == 496U);
static_assert(alignof(Kernel::Ipc::MessageEnvelope) == 8U);
static_assert(sizeof(Kernel::Ipc::ReadOnlyByteView) == 16U);
static_assert(alignof(Kernel::Ipc::ReadOnlyByteView) == 8U);
static_assert(sizeof(Kernel::Ipc::WritableByteView) == 16U);
static_assert(alignof(Kernel::Ipc::WritableByteView) == 8U);
static_assert(sizeof(Foundation::Result<Foundation::Size, Kernel::Ipc::MessageCodecError>) == 16U);
static_assert(alignof(Foundation::Result<Foundation::Size, Kernel::Ipc::MessageCodecError>) == 8U);
static_assert(Kernel::Ipc::MessageCodec::MaximumWireSizeBytes == 504U);
static_assert(sizeof(Kernel::Ipc::IpcDeliveryCodecError) == 1U);
static_assert(sizeof(Kernel::Ipc::IpcDeliveryCodecFailure) == 8U);
static_assert(alignof(Kernel::Ipc::IpcDeliveryCodecFailure) == 1U);
static_assert(Kernel::Ipc::IpcDeliveryCodec::WireHeaderSizeBytes == 48U);
static_assert(Kernel::Ipc::IpcDeliveryCodec::MaximumWireSizeBytes == 552U);
static_assert(sizeof(Kernel::Memory::UserMemoryRangeError) == 1U);
static_assert(sizeof(Kernel::Memory::UserMemoryRange) == 16U);
static_assert(alignof(Kernel::Memory::UserMemoryRange) == 8U);
static_assert(sizeof(Kernel::Memory::ReadOnlyKernelBuffer) == 16U);
static_assert(sizeof(Kernel::Memory::WritableKernelBuffer) == 16U);
static_assert(sizeof(Kernel::Memory::UserMemoryAccessError) == 1U);
static_assert(sizeof(Kernel::Memory::UserMemoryWriteReservation) == 32U);
static_assert(alignof(Kernel::Memory::UserMemoryWriteReservation) == 8U);
static_assert(sizeof(Kernel::System::SystemCallNumber) == 8U);
static_assert(sizeof(Kernel::System::SystemCallError) == 8U);
static_assert(Kernel::System::SystemCallArgumentCount == 6U);
static_assert(Kernel::System::SystemCallQueryObjectTypeMask == 0xFFU);
static_assert(Kernel::System::SystemCallQueryRightsShift == 16U);
static_assert(static_cast<Foundation::UInt64>(Kernel::System::SystemCallNumber::CapabilityClose) == 1U);
static_assert(static_cast<Foundation::UInt64>(Kernel::System::SystemCallNumber::CapabilityQuery) == 2U);
static_assert(static_cast<Foundation::UInt64>(Kernel::System::SystemCallNumber::CapabilityDerive) == 3U);
static_assert(static_cast<Foundation::UInt64>(Kernel::System::SystemCallNumber::CapabilityDuplicate) == 4U);
static_assert(static_cast<Foundation::UInt64>(Kernel::System::SystemCallNumber::CapabilityRevoke) == 5U);
static_assert(static_cast<Foundation::UInt64>(Kernel::System::SystemCallNumber::IpcSend) == 6U);
static_assert(static_cast<Foundation::UInt64>(Kernel::System::SystemCallNumber::IpcReceive) == 7U);
static_assert(static_cast<Foundation::UInt64>(Kernel::System::SystemCallNumber::ObjectCreate) == 11U);
static_assert(static_cast<Foundation::UInt64>(Kernel::System::SystemCallNumber::ObjectDestroy) == 12U);
static_assert(static_cast<Foundation::UInt64>(Kernel::System::SystemCallNumber::MemoryMap) == 13U);
static_assert(static_cast<Foundation::UInt64>(Kernel::System::SystemCallNumber::MemoryUnmap) == 14U);
static_assert(static_cast<Foundation::UInt64>(Kernel::System::SystemCallNumber::ThreadExit) == 15U);
static_assert(static_cast<Foundation::UInt64>(Kernel::System::SystemCallNumber::CapabilityGrant) == 16U);
static_assert(static_cast<Foundation::UInt64>(Kernel::System::SystemCallNumber::ThreadTerminate) == 17U);
static_assert(static_cast<Foundation::UInt64>(Kernel::System::SystemCallNumber::WaitSetBind) == 18U);
static_assert(static_cast<Foundation::UInt64>(Kernel::System::SystemCallNumber::DeviceControl) == 19U);
static_assert(static_cast<Foundation::UInt64>(Kernel::System::SystemCallNumber::IommuControl) == 20U);
static_assert(static_cast<Foundation::UInt64>(Kernel::System::SystemCallNumber::InterruptControl) == 21U);
static_assert(Abi::ObjectTypeInterrupt == static_cast<Foundation::UInt64>(Kernel::Object::ObjectType::Interrupt));
static_assert(Abi::ObjectTypeIommuDomain == static_cast<Foundation::UInt64>(Kernel::Object::ObjectType::IommuDomain));
static_assert(Abi::KilledExitCode == 0x8000'0000'0000'0003ULL);
static_assert((Abi::KilledExitCode >> 63U) == 1U);
static_assert(Abi::KilledExitCode != Abi::FaultTerminatedExitCode);
static_assert(Abi::CapabilityGrantMove ==
              static_cast<Foundation::UInt64>(Kernel::Object::CapabilityTransferDisposition::Move));
static_assert(Abi::CapabilityGrantCopy ==
              static_cast<Foundation::UInt64>(Kernel::Object::CapabilityTransferDisposition::Copy));
static_assert(Abi::ObjectTypeThread == static_cast<Foundation::UInt64>(Kernel::Object::ObjectType::Thread));
static_assert(Abi::ObjectTypeCapabilitySpace ==
              static_cast<Foundation::UInt64>(Kernel::Object::ObjectType::CapabilitySpace));
static_assert(Abi::ObjectTypeAddressSpace == static_cast<Foundation::UInt64>(Kernel::Object::ObjectType::AddressSpace));
static_assert(Abi::ObjectTypeProtectionDomain ==
              static_cast<Foundation::UInt64>(Kernel::Object::ObjectType::ProtectionDomain));
static_assert(Abi::ObjectCreateDomainBillingOffset + sizeof(Kernel::Object::CapabilityHandle) + 8U ==
              Abi::ObjectCreateProtectionDomainRequestSizeBytes);
static_assert(sizeof(Kernel::Memory::MemoryMapRequest) == Abi::FaultResolveRequestSizeBytes);
static_assert(Abi::ObjectTypeEndpoint == static_cast<Foundation::UInt64>(Kernel::Object::ObjectType::Endpoint));
static_assert(Abi::ObjectTypeWaitSet == static_cast<Foundation::UInt64>(Kernel::Object::ObjectType::WaitSet));
static_assert(Abi::ObjectTypeResourceDomain ==
              static_cast<Foundation::UInt64>(Kernel::Object::ObjectType::ResourceDomain));
// The request is ResourceDomainLimits, field for field.
static_assert(Abi::ObjectCreateResourceDomainRequestSizeBytes ==
              __builtin_offsetof(Kernel::Resource::ResourceDomainLimits, Objects));
static_assert(Abi::ObjectCreateDomainCommittedPagesOffset ==
              __builtin_offsetof(Kernel::Resource::ResourceDomainLimits, CommittedMemoryPages));
static_assert(Abi::ObjectCreateDomainResidentPagesOffset ==
              __builtin_offsetof(Kernel::Resource::ResourceDomainLimits, ResidentMemoryPages));
static_assert(Abi::ObjectCreateDomainMetadataOffset ==
              __builtin_offsetof(Kernel::Resource::ResourceDomainLimits, KernelMetadataBytes));
static_assert(Abi::ObjectCreateDomainHandlesOffset ==
              __builtin_offsetof(Kernel::Resource::ResourceDomainLimits, CapabilityHandles));
static_assert(Abi::ObjectCreateDomainQueueBytesOffset ==
              __builtin_offsetof(Kernel::Resource::ResourceDomainLimits, IpcQueueBytes));
static_assert(Abi::ObjectCreateDomainThreadsOffset ==
              __builtin_offsetof(Kernel::Resource::ResourceDomainLimits, Threads));
static_assert(Abi::ObjectCreateDomainChildrenOffset ==
              __builtin_offsetof(Kernel::Resource::ResourceDomainLimits, ChildDomains));
static_assert((Abi::ObjectCreateEndpointSealedSelectorOffset + sizeof(Foundation::UInt64)) ==
              Abi::ObjectCreateEndpointRequestSizeBytes);
static_assert(Abi::ObjectCreateEndpointReservedOffset + 4U == Abi::ObjectCreateEndpointSealedSelectorOffset);
static_assert(Abi::ObjectTypeFrame == static_cast<Foundation::UInt64>(Kernel::Object::ObjectType::Frame));
static_assert(Abi::ObjectTypeMemoryObject == static_cast<Foundation::UInt64>(Kernel::Object::ObjectType::MemoryObject));
static_assert(Abi::ObjectCreatePermissionsOffset == (Abi::ObjectCreatePagesOffset + sizeof(Foundation::UInt64)));
static_assert(Abi::ObjectCreateEndpointSealedSelectorOffset ==
              (Abi::ObjectCreateEndpointQueuedBytesOffset + sizeof(Kernel::Ipc::EndpointConfiguration)));
static_assert(Abi::ObjectCreateEndpointQueuedBytesOffset ==
              (Abi::ObjectCreateEndpointReceiverSpaceOffset + sizeof(Kernel::Object::CapabilityHandle)));
static_assert(Abi::ObjectCreateEndpointQueuedBytesOffset +
                  __builtin_offsetof(Kernel::Ipc::EndpointConfiguration, MaximumAttachmentsPerSender) ==
              Abi::ObjectCreateEndpointAttachmentsPerSenderOffset);
static_assert(Abi::ObjectCreateEndpointQueuedBytesOffset +
                  __builtin_offsetof(Kernel::Ipc::EndpointConfiguration, Reserved) ==
              Abi::ObjectCreateEndpointReservedOffset);
static_assert(static_cast<Foundation::UInt64>(Kernel::System::SystemCallError::None) == 0U);
static_assert(static_cast<Foundation::UInt64>(Kernel::System::SystemCallError::UnknownSystemCall) == 1U);
static_assert(static_cast<Foundation::UInt64>(Kernel::System::SystemCallError::InvalidArguments) == 2U);
static_assert(static_cast<Foundation::UInt64>(Kernel::System::SystemCallError::InvalidConfiguration) == 3U);
static_assert(static_cast<Foundation::UInt64>(Kernel::System::SystemCallError::InvalidHandle) == 4U);
static_assert(static_cast<Foundation::UInt64>(Kernel::System::SystemCallError::ObjectNotFound) == 5U);
static_assert(static_cast<Foundation::UInt64>(Kernel::System::SystemCallError::ResourceExhausted) == 6U);
static_assert(static_cast<Foundation::UInt64>(Kernel::System::SystemCallError::SpaceExhausted) == 7U);
static_assert(static_cast<Foundation::UInt64>(Kernel::System::SystemCallError::RightsDenied) == 8U);
static_assert(static_cast<Foundation::UInt64>(Kernel::System::SystemCallError::RightsAmplification) == 9U);
static_assert(static_cast<Foundation::UInt64>(Kernel::System::SystemCallError::NoAttenuation) == 10U);
static_assert(static_cast<Foundation::UInt64>(Kernel::System::SystemCallError::TypeMismatch) == 11U);
static_assert(static_cast<Foundation::UInt64>(Kernel::System::SystemCallError::Revoked) == 12U);
static_assert(static_cast<Foundation::UInt64>(Kernel::System::SystemCallError::Expired) == 13U);
static_assert(static_cast<Foundation::UInt64>(Kernel::System::SystemCallError::LeaseExtension) == 14U);
static_assert(static_cast<Foundation::UInt64>(Kernel::System::SystemCallError::RevocationDomainExhausted) == 15U);
static_assert(static_cast<Foundation::UInt64>(Kernel::System::SystemCallError::WouldBlock) == 16U);
static_assert(static_cast<Foundation::UInt64>(Kernel::System::SystemCallError::UserMemoryFault) == 17U);
static_assert(static_cast<Foundation::UInt64>(Kernel::System::SystemCallError::TimedOut) == 18U);
static_assert(sizeof(Kernel::System::SystemCallRequest) == 56U);
static_assert(alignof(Kernel::System::SystemCallRequest) == 8U);
static_assert(__builtin_offsetof(Kernel::System::SystemCallRequest, Number) == 0U);
static_assert(__builtin_offsetof(Kernel::System::SystemCallRequest, Arguments) == 8U);
static_assert(sizeof(Kernel::System::SystemCallResponse) == 24U);
static_assert(alignof(Kernel::System::SystemCallResponse) == 8U);
static_assert(__builtin_offsetof(Kernel::System::SystemCallResponse, Error) == 0U);
static_assert(__builtin_offsetof(Kernel::System::SystemCallResponse, Value0) == 8U);
static_assert(__builtin_offsetof(Kernel::System::SystemCallResponse, Value1) == 16U);
static_assert(sizeof(Kernel::System::SystemCallIngressError) == 1U);
static_assert(sizeof(Kernel::System::IpcSystemCallDispatchError) == 1U);
static_assert(sizeof(Kernel::System::IpcSystemCallDispatchFailure) == 4U);
static_assert(alignof(Kernel::System::IpcSystemCallDispatchFailure) == 1U);
static_assert(sizeof(Kernel::System::SystemCallContext) == 80U);
static_assert(alignof(Kernel::System::SystemCallContext) == 8U);
static_assert(sizeof(Kernel::System::SystemCallScratch) == 1'048U);
static_assert(alignof(Kernel::System::SystemCallScratch) == 8U);
static_assert(sizeof(Kernel::System::SystemCallIngressFailure) == 8U);
static_assert(alignof(Kernel::System::SystemCallIngressFailure) == 1U);
static_assert(sizeof(Kernel::Scheduler::ProcessorActiveProtectionDomainBinding) == 48U);
static_assert(alignof(Kernel::Scheduler::ProcessorActiveProtectionDomainBinding) == 8U);
static_assert(sizeof(Platform::Amd64::System::Amd64SystemCallRegisterImage) == 56U);
static_assert(alignof(Platform::Amd64::System::Amd64SystemCallRegisterImage) == 8U);
static_assert(__builtin_offsetof(Platform::Amd64::System::Amd64SystemCallRegisterImage, Rax) == 0U);
static_assert(__builtin_offsetof(Platform::Amd64::System::Amd64SystemCallRegisterImage, Rdi) == 8U);
static_assert(__builtin_offsetof(Platform::Amd64::System::Amd64SystemCallRegisterImage, Rsi) == 16U);
static_assert(__builtin_offsetof(Platform::Amd64::System::Amd64SystemCallRegisterImage, Rdx) == 24U);
static_assert(__builtin_offsetof(Platform::Amd64::System::Amd64SystemCallRegisterImage, R10) == 32U);
static_assert(__builtin_offsetof(Platform::Amd64::System::Amd64SystemCallRegisterImage, R8) == 40U);
static_assert(__builtin_offsetof(Platform::Amd64::System::Amd64SystemCallRegisterImage, R9) == 48U);
static_assert(sizeof(Platform::Ia64::System::Ia64SystemCallRegisterDecodeError) == 1U);
static_assert(static_cast<Foundation::UInt8>(Platform::Ia64::System::Ia64SystemCallRegisterDecodeError::None) == 0U);
static_assert(
    static_cast<Foundation::UInt8>(Platform::Ia64::System::Ia64SystemCallRegisterDecodeError::InputContainsNat) == 1U);
static_assert(static_cast<Foundation::UInt8>(
                  Platform::Ia64::System::Ia64SystemCallRegisterDecodeError::InvalidNatMetadata) == 2U);
static_assert(Platform::Ia64::System::Ia64SystemCallKnownInputNatBitsMask == 0x7FU);
static_assert(sizeof(Platform::Ia64::System::Ia64SystemCallRegisterImage) == 88U);
static_assert(alignof(Platform::Ia64::System::Ia64SystemCallRegisterImage) == 8U);
static_assert(__builtin_offsetof(Platform::Ia64::System::Ia64SystemCallRegisterImage, R15) == 0U);
static_assert(__builtin_offsetof(Platform::Ia64::System::Ia64SystemCallRegisterImage, R32) == 8U);
static_assert(__builtin_offsetof(Platform::Ia64::System::Ia64SystemCallRegisterImage, R33) == 16U);
static_assert(__builtin_offsetof(Platform::Ia64::System::Ia64SystemCallRegisterImage, R34) == 24U);
static_assert(__builtin_offsetof(Platform::Ia64::System::Ia64SystemCallRegisterImage, R35) == 32U);
static_assert(__builtin_offsetof(Platform::Ia64::System::Ia64SystemCallRegisterImage, R36) == 40U);
static_assert(__builtin_offsetof(Platform::Ia64::System::Ia64SystemCallRegisterImage, R37) == 48U);
static_assert(__builtin_offsetof(Platform::Ia64::System::Ia64SystemCallRegisterImage, InputNatBits) == 56U);
static_assert(__builtin_offsetof(Platform::Ia64::System::Ia64SystemCallRegisterImage, R8) == 64U);
static_assert(__builtin_offsetof(Platform::Ia64::System::Ia64SystemCallRegisterImage, R9) == 72U);
static_assert(__builtin_offsetof(Platform::Ia64::System::Ia64SystemCallRegisterImage, R10) == 80U);
static_assert(sizeof(Kernel::Ipc::EndpointError) == 4U);
static_assert(alignof(Kernel::Ipc::EndpointError) == 1U);
static_assert(sizeof(Kernel::Ipc::EndpointConfiguration) == 16U);
static_assert(alignof(Kernel::Ipc::EndpointConfiguration) == 4U);
static_assert(sizeof(Kernel::Ipc::EndpointSendReceipt) == 8U);
static_assert(alignof(Kernel::Ipc::EndpointSendReceipt) == 8U);
static_assert(sizeof(Kernel::Ipc::EndpointSendReservation) == 16U);
static_assert(alignof(Kernel::Ipc::EndpointSendReservation) == 8U);
static_assert(sizeof(Kernel::Ipc::EndpointReceiveReservation) == 16U);
static_assert(alignof(Kernel::Ipc::EndpointReceiveReservation) == 8U);
static_assert(sizeof(Kernel::Ipc::EndpointReceiveMetadata) == 32U);
static_assert(alignof(Kernel::Ipc::EndpointReceiveMetadata) == 8U);
static_assert(sizeof(Kernel::Ipc::EndpointReceivePreparation) == 48U);
static_assert(alignof(Kernel::Ipc::EndpointReceivePreparation) == 8U);
static_assert(sizeof(Kernel::Ipc::EndpointQueueState) == 24U);
static_assert(alignof(Kernel::Ipc::EndpointQueueState) == 8U);
static_assert(sizeof(Foundation::Result<Kernel::Ipc::EndpointSendReceipt, Kernel::Ipc::EndpointError>) == 16U);
static_assert(alignof(Foundation::Result<Kernel::Ipc::EndpointSendReceipt, Kernel::Ipc::EndpointError>) == 8U);
static_assert(sizeof(Foundation::Result<Kernel::Ipc::EndpointReceiveMetadata, Kernel::Ipc::EndpointError>) == 40U);
static_assert(alignof(Foundation::Result<Kernel::Ipc::EndpointReceiveMetadata, Kernel::Ipc::EndpointError>) == 8U);
static_assert(sizeof(Foundation::Result<Kernel::Ipc::EndpointQueueState, Kernel::Ipc::EndpointError>) == 32U);
static_assert(alignof(Foundation::Result<Kernel::Ipc::EndpointQueueState, Kernel::Ipc::EndpointError>) == 8U);
static_assert(sizeof(Kernel::Ipc::ReplyTokenState) == 1U);
static_assert(sizeof(Kernel::Ipc::ReplyTokenError) == 1U);
static_assert(sizeof(Kernel::Ipc::ReplyTokenFailure) == 8U);
static_assert(alignof(Kernel::Ipc::ReplyTokenFailure) == 1U);
static_assert(sizeof(Kernel::Ipc::ReplyTokenCompletion) == 48U);
static_assert(alignof(Kernel::Ipc::ReplyTokenCompletion) == 8U);
static_assert(sizeof(Kernel::Ipc::ReplyTokenSnapshot) == 80U);
static_assert(alignof(Kernel::Ipc::ReplyTokenSnapshot) == 8U);
static_assert(sizeof(Foundation::Result<Kernel::Ipc::ReplyTokenCompletion, Kernel::Ipc::ReplyTokenFailure>) == 64U);
static_assert(alignof(Foundation::Result<Kernel::Ipc::ReplyTokenCompletion, Kernel::Ipc::ReplyTokenFailure>) == 8U);
static_assert(sizeof(Kernel::Scheduler::ReadyQueueEnqueueReservation) == 16U);
static_assert(alignof(Kernel::Scheduler::ReadyQueueEnqueueReservation) == 8U);
static_assert(sizeof(Kernel::Scheduler::ReadyQueueState) == 16U);
static_assert(alignof(Kernel::Scheduler::ReadyQueueState) == 8U);
static_assert(sizeof(Kernel::Ipc::SynchronousReplyDispatcherState) == 1U);
static_assert(sizeof(Kernel::Ipc::SynchronousReplyDispatcherError) == 1U);
static_assert(sizeof(Kernel::Ipc::SynchronousReplyDispatcherFailure) == 32U);
static_assert(alignof(Kernel::Ipc::SynchronousReplyDispatcherFailure) == 8U);
static_assert(sizeof(Kernel::Ipc::SynchronousReplyDispatcherReceipt) == 48U);
static_assert(alignof(Kernel::Ipc::SynchronousReplyDispatcherReceipt) == 8U);
static_assert(sizeof(Kernel::Ipc::SynchronousReplyDispatcherSnapshot) == 64U);
static_assert(alignof(Kernel::Ipc::SynchronousReplyDispatcherSnapshot) == 8U);
static_assert(sizeof(Kernel::Memory::PhysicalPageRange) == 32U);
static_assert(alignof(Kernel::Memory::PhysicalPageRange) == 8U);
static_assert(sizeof(Kernel::Memory::BootstrapPageAllocator) == 65'568U);
static_assert(alignof(Kernel::Memory::BootstrapPageAllocator) == 8U);
static_assert(sizeof(Kernel::Memory::PhysicalAddress) == 8U);
static_assert(sizeof(Kernel::Memory::VirtualAddress) == 8U);
static_assert(sizeof(Kernel::Memory::PageCount) == 8U);
static_assert(sizeof(Kernel::Memory::InitialMapping) == 32U);
static_assert(alignof(Kernel::Memory::InitialMapping) == 8U);
static_assert(sizeof(Kernel::Memory::InitialMappingPlan) == 8'208U);
static_assert(alignof(Kernel::Memory::InitialMappingPlan) == 8U);
static_assert(sizeof(Platform::Amd64::Firmware::Amd64LoadedImageSectionPurpose) == 4U);
static_assert(sizeof(Platform::Amd64::Firmware::Amd64LoadedImageSection) == 24U);
static_assert(alignof(Platform::Amd64::Firmware::Amd64LoadedImageSection) == 8U);
static_assert(__builtin_offsetof(Platform::Amd64::Firmware::Amd64LoadedImageSection, PageCount) == 8U);
static_assert(__builtin_offsetof(Platform::Amd64::Firmware::Amd64LoadedImageSection, Purpose) == 16U);
static_assert(__builtin_offsetof(Platform::Amd64::Firmware::Amd64LoadedImageSection, Reserved) == 20U);
static_assert(sizeof(Platform::Amd64::Firmware::Amd64LoadedImageLayout) == 184U);
static_assert(alignof(Platform::Amd64::Firmware::Amd64LoadedImageLayout) == 8U);
static_assert(__builtin_offsetof(Platform::Amd64::Firmware::Amd64LoadedImageLayout, ImageSize) == 8U);
static_assert(__builtin_offsetof(Platform::Amd64::Firmware::Amd64LoadedImageLayout, Sections) == 16U);
static_assert(sizeof(Platform::Amd64::Firmware::Amd64LoadedImageLayoutError) == 4U);
static_assert(sizeof(Platform::Amd64::Memory::BootstrapMappingPlanError) == 4U);
static_assert(sizeof(Platform::Amd64::Memory::BootstrapMappingPlanSummary) == 40U);
static_assert(alignof(Platform::Amd64::Memory::BootstrapMappingPlanSummary) == 8U);
static_assert(__builtin_offsetof(Platform::Amd64::Memory::BootstrapMappingPlanSummary, ReservedPhysicalStart) == 0U);
static_assert(__builtin_offsetof(Platform::Amd64::Memory::BootstrapMappingPlanSummary, StackPhysicalStart) == 8U);
static_assert(__builtin_offsetof(Platform::Amd64::Memory::BootstrapMappingPlanSummary, PageTableStoragePhysicalStart) ==
              16U);
static_assert(__builtin_offsetof(Platform::Amd64::Memory::BootstrapMappingPlanSummary,
                                 DescriptorTableStoragePhysicalStart) == 24U);
static_assert(__builtin_offsetof(Platform::Amd64::Memory::BootstrapMappingPlanSummary, MappingCount) == 32U);
static_assert(__builtin_offsetof(Platform::Amd64::Memory::BootstrapMappingPlanSummary, Reserved16) == 34U);
static_assert(__builtin_offsetof(Platform::Amd64::Memory::BootstrapMappingPlanSummary, Reserved32) == 36U);
static_assert(__builtin_offsetof(Platform::Amd64::Memory::BootstrapMappingPlanSummary, Reserved32) +
                  sizeof(Platform::Amd64::Memory::BootstrapMappingPlanSummary::Reserved32) ==
              sizeof(Platform::Amd64::Memory::BootstrapMappingPlanSummary));
static_assert(Platform::Amd64::Memory::BootstrapReservedPageCount == 23U);
static_assert(Platform::Amd64::Memory::BootstrapInitialMappingCount == 10U);
static_assert(Platform::Amd64::Memory::BootstrapDescriptorTableStoragePageCount == 2U);
static_assert(Platform::Amd64::Memory::BootstrapMaximumRequiredPageTablePages == 13U);
static_assert(sizeof(Platform::Amd64::Memory::InitialPagingCapabilityCaptureError) == 4U);
static_assert(sizeof(Platform::Amd64::Memory::InitialPagingCapabilityReport) == 24U);
static_assert(alignof(Platform::Amd64::Memory::InitialPagingCapabilityReport) == 4U);
static_assert(__builtin_offsetof(Platform::Amd64::Memory::InitialPagingCapabilityReport, MaximumBasicLeaf) == 0U);
static_assert(__builtin_offsetof(Platform::Amd64::Memory::InitialPagingCapabilityReport, BasicFeaturesEdx) == 4U);
static_assert(__builtin_offsetof(Platform::Amd64::Memory::InitialPagingCapabilityReport, MaximumExtendedLeaf) == 8U);
static_assert(__builtin_offsetof(Platform::Amd64::Memory::InitialPagingCapabilityReport, ExtendedFeaturesEdx) == 12U);
static_assert(__builtin_offsetof(Platform::Amd64::Memory::InitialPagingCapabilityReport, AddressSizesEax) == 16U);
static_assert(__builtin_offsetof(Platform::Amd64::Memory::InitialPagingCapabilityReport, Reserved) == 20U);
static_assert(sizeof(Platform::Amd64::Memory::PagingCapabilities) == 12U);
static_assert(alignof(Platform::Amd64::Memory::PagingCapabilities) == 4U);
static_assert(__builtin_offsetof(Platform::Amd64::Memory::PagingCapabilities, Reserved16) == 6U);
static_assert(__builtin_offsetof(Platform::Amd64::Memory::PagingCapabilities, Reserved) == 8U);
static_assert(__builtin_offsetof(Platform::Amd64::Memory::PagingCapabilities, Reserved16) +
                  sizeof(Platform::Amd64::Memory::PagingCapabilities::Reserved16) ==
              __builtin_offsetof(Platform::Amd64::Memory::PagingCapabilities, Reserved));
static_assert(sizeof(Platform::Amd64::Memory::PageTableStorage) == 32U);
static_assert(alignof(Platform::Amd64::Memory::PageTableStorage) == 8U);
static_assert(__builtin_offsetof(Platform::Amd64::Memory::PageTableStorage, Entries) == 8U);
static_assert(__builtin_offsetof(Platform::Amd64::Memory::PageTableStorage, PageCapacity) == 16U);
static_assert(__builtin_offsetof(Platform::Amd64::Memory::PageTableStorage, Reserved) == 24U);
static_assert(sizeof(Platform::Amd64::Memory::InitialPageTableBuildResult) == 32U);
static_assert(alignof(Platform::Amd64::Memory::InitialPageTableBuildResult) == 8U);
static_assert(__builtin_offsetof(Platform::Amd64::Memory::InitialPageTableBuildResult, UsedTablePages) == 8U);
static_assert(__builtin_offsetof(Platform::Amd64::Memory::InitialPageTableBuildResult, MappedPages) == 16U);
static_assert(__builtin_offsetof(Platform::Amd64::Memory::InitialPageTableBuildResult, Reserved) == 24U);
static_assert(sizeof(Platform::Amd64::Memory::InitialPageTablePreparationStatus) == 4U);
static_assert(sizeof(Platform::Amd64::Memory::InitialPageTablePreparationError) == 4U);
static_assert(sizeof(Platform::Amd64::Memory::PreparedPageTableStorage) == 32U);
static_assert(alignof(Platform::Amd64::Memory::PreparedPageTableStorage) == 8U);
static_assert(__builtin_offsetof(Platform::Amd64::Memory::PreparedPageTableStorage, RootPhysicalAddress) == 0U);
static_assert(__builtin_offsetof(Platform::Amd64::Memory::PreparedPageTableStorage, Entries) == 8U);
static_assert(__builtin_offsetof(Platform::Amd64::Memory::PreparedPageTableStorage, PageCapacity) == 16U);
static_assert(__builtin_offsetof(Platform::Amd64::Memory::PreparedPageTableStorage, UsedTablePages) == 24U);
static_assert(sizeof(Platform::Amd64::Memory::InitialPageTablePreparationSummary) == 48U);
static_assert(alignof(Platform::Amd64::Memory::InitialPageTablePreparationSummary) == 8U);
static_assert(__builtin_offsetof(Platform::Amd64::Memory::InitialPageTablePreparationSummary, Storage) == 0U);
static_assert(__builtin_offsetof(Platform::Amd64::Memory::InitialPageTablePreparationSummary, MappedPages) == 32U);
static_assert(__builtin_offsetof(Platform::Amd64::Memory::InitialPageTablePreparationSummary, MappingCount) == 40U);
static_assert(__builtin_offsetof(Platform::Amd64::Memory::InitialPageTablePreparationSummary, Reserved16) == 42U);
static_assert(__builtin_offsetof(Platform::Amd64::Memory::InitialPageTablePreparationSummary, Reserved32) == 44U);
static_assert(sizeof(Platform::Amd64::Memory::InitialPageTablePreparationFailure) == 24U);
static_assert(alignof(Platform::Amd64::Memory::InitialPageTablePreparationFailure) == 8U);
static_assert(__builtin_offsetof(Platform::Amd64::Memory::InitialPageTablePreparationFailure, Error) == 0U);
static_assert(__builtin_offsetof(Platform::Amd64::Memory::InitialPageTablePreparationFailure, BuildError) == 4U);
static_assert(__builtin_offsetof(Platform::Amd64::Memory::InitialPageTablePreparationFailure, MappingError) == 8U);
static_assert(__builtin_offsetof(Platform::Amd64::Memory::InitialPageTablePreparationFailure, Reserved) == 12U);
static_assert(__builtin_offsetof(Platform::Amd64::Memory::InitialPageTablePreparationFailure, MappingIndex) == 16U);
static_assert(sizeof(Platform::Amd64::Memory::InitialPageTablePreparation) == 8'288U);
static_assert(alignof(Platform::Amd64::Memory::InitialPageTablePreparation) == 8U);
static_assert(sizeof(Foundation::Result<Platform::Amd64::Memory::InitialPageTablePreparationStatus,
                                        Platform::Amd64::Memory::InitialPageTablePreparationError>) == 12U);
static_assert(alignof(Foundation::Result<Platform::Amd64::Memory::InitialPageTablePreparationStatus,
                                         Platform::Amd64::Memory::InitialPageTablePreparationError>) == 4U);
static_assert(sizeof(Foundation::Result<const Platform::Amd64::Memory::InitialPageTablePreparationSummary*,
                                        Platform::Amd64::Memory::InitialPageTablePreparationError>) == 16U);
static_assert(alignof(Foundation::Result<const Platform::Amd64::Memory::InitialPageTablePreparationSummary*,
                                         Platform::Amd64::Memory::InitialPageTablePreparationError>) == 8U);
static_assert(sizeof(Platform::Amd64::Memory::InitialPageTableVerificationError) == 4U);
static_assert(sizeof(Platform::Amd64::Memory::InitialProcessorStateCaptureError) == 4U);
static_assert(sizeof(Platform::Amd64::Memory::InitialAddressSpacePreparationStatus) == 4U);
static_assert(sizeof(Platform::Amd64::Memory::InitialAddressSpacePreparationError) == 4U);
static_assert(sizeof(Platform::Amd64::Memory::WritableInitialMemoryStackView) == 24U);
static_assert(alignof(Platform::Amd64::Memory::WritableInitialMemoryStackView) == 8U);
static_assert(__builtin_offsetof(Platform::Amd64::Memory::WritableInitialMemoryStackView, Data) == 0U);
static_assert(__builtin_offsetof(Platform::Amd64::Memory::WritableInitialMemoryStackView, SizeBytes) == 8U);
static_assert(__builtin_offsetof(Platform::Amd64::Memory::WritableInitialMemoryStackView, PhysicalStart) == 16U);
static_assert(sizeof(Platform::Amd64::Memory::WritableInitialDescriptorTableView) == 24U);
static_assert(alignof(Platform::Amd64::Memory::WritableInitialDescriptorTableView) == 8U);
static_assert(__builtin_offsetof(Platform::Amd64::Memory::WritableInitialDescriptorTableView, Data) == 0U);
static_assert(__builtin_offsetof(Platform::Amd64::Memory::WritableInitialDescriptorTableView, SizeBytes) == 8U);
static_assert(__builtin_offsetof(Platform::Amd64::Memory::WritableInitialDescriptorTableView, PhysicalStart) == 16U);
static_assert(sizeof(Platform::Amd64::Memory::InitialProcessorStateReport) == 424U);
static_assert(alignof(Platform::Amd64::Memory::InitialProcessorStateReport) == 8U);
static_assert(__builtin_offsetof(Platform::Amd64::Memory::InitialProcessorStateReport, VariableMtrrBase) == 64U);
static_assert(__builtin_offsetof(Platform::Amd64::Memory::InitialProcessorStateReport, VariableMtrrMask) == 192U);
static_assert(__builtin_offsetof(Platform::Amd64::Memory::InitialProcessorStateReport, FixedMtrr) == 320U);
static_assert(__builtin_offsetof(Platform::Amd64::Memory::InitialProcessorStateReport, CodeSelector) == 408U);
static_assert(__builtin_offsetof(Platform::Amd64::Memory::InitialProcessorStateReport, ReservedTail) == 420U);
static_assert(sizeof(Platform::Amd64::Memory::InitialAddressSpacePreparationSummary) == 64U);
static_assert(alignof(Platform::Amd64::Memory::InitialAddressSpacePreparationSummary) == 8U);
static_assert(__builtin_offsetof(Platform::Amd64::Memory::InitialAddressSpacePreparationSummary, RootPhysicalAddress) ==
              0U);
static_assert(__builtin_offsetof(Platform::Amd64::Memory::InitialAddressSpacePreparationSummary, InitialStackPointer) ==
              16U);
static_assert(__builtin_offsetof(Platform::Amd64::Memory::InitialAddressSpacePreparationSummary,
                                 GlobalDescriptorTableAddress) == 24U);
static_assert(__builtin_offsetof(Platform::Amd64::Memory::InitialAddressSpacePreparationSummary, UsedTablePages) ==
              40U);
static_assert(__builtin_offsetof(Platform::Amd64::Memory::InitialAddressSpacePreparationSummary, CodeSelector) == 48U);
static_assert(__builtin_offsetof(Platform::Amd64::Memory::InitialAddressSpacePreparationSummary, ReservedTail) == 60U);
static_assert(sizeof(Platform::Amd64::Memory::InitialAddressSpacePreparationFailure) == 40U);
static_assert(alignof(Platform::Amd64::Memory::InitialAddressSpacePreparationFailure) == 8U);
static_assert(__builtin_offsetof(Platform::Amd64::Memory::InitialAddressSpacePreparationFailure, Error) == 0U);
static_assert(__builtin_offsetof(Platform::Amd64::Memory::InitialAddressSpacePreparationFailure, AdapterStatus) == 24U);
static_assert(__builtin_offsetof(Platform::Amd64::Memory::InitialAddressSpacePreparationFailure, MappingIndex) == 32U);
static_assert(sizeof(Platform::Amd64::Memory::InitialAddressSpacePreparation) == 8'952U);
static_assert(alignof(Platform::Amd64::Memory::InitialAddressSpacePreparation) == 8U);
static_assert(sizeof(Foundation::Result<Platform::Amd64::Memory::InitialAddressSpacePreparationStatus,
                                        Platform::Amd64::Memory::InitialAddressSpacePreparationError>) == 12U);
static_assert(alignof(Foundation::Result<Platform::Amd64::Memory::InitialAddressSpacePreparationStatus,
                                         Platform::Amd64::Memory::InitialAddressSpacePreparationError>) == 4U);
static_assert(sizeof(Foundation::Result<const Platform::Amd64::Memory::InitialAddressSpacePreparationSummary*,
                                        Platform::Amd64::Memory::InitialAddressSpacePreparationError>) == 16U);
static_assert(alignof(Foundation::Result<const Platform::Amd64::Memory::InitialAddressSpacePreparationSummary*,
                                         Platform::Amd64::Memory::InitialAddressSpacePreparationError>) == 8U);
static_assert(sizeof(Platform::Ia64::Firmware::SalSystemTableLocation) == 40U);
static_assert(alignof(Platform::Ia64::Firmware::SalSystemTableLocation) == 8U);
static_assert(sizeof(Platform::Ia64::Firmware::SalSystemTableInformation) == 80U);
static_assert(alignof(Platform::Ia64::Firmware::SalSystemTableInformation) == 8U);
static_assert(sizeof(Platform::Ia64::Firmware::SalSystemTableCaptureError) == 12U);
static_assert(alignof(Platform::Ia64::Firmware::SalSystemTableCaptureError) == 4U);
static_assert(sizeof(Foundation::Result<Platform::Ia64::Firmware::SalSystemTableInformation,
                                        Platform::Ia64::Firmware::SalSystemTableCaptureError>) == 96U);
static_assert(alignof(Foundation::Result<Platform::Ia64::Firmware::SalSystemTableInformation,
                                         Platform::Ia64::Firmware::SalSystemTableCaptureError>) == 8U);
static_assert(sizeof(Platform::Ia64::Firmware::PalStaticCallAdapterError) == 4U);
static_assert(sizeof(Platform::Ia64::Firmware::PalBootstrapCapabilityCapturePhase) == 4U);
static_assert(sizeof(Platform::Ia64::Firmware::PalBootstrapCapabilityCaptureError) == 16U);
static_assert(alignof(Platform::Ia64::Firmware::PalBootstrapCapabilityCaptureError) == 4U);
static_assert(sizeof(Platform::Ia64::Firmware::PalBootstrapCapabilities) == 88U);
static_assert(alignof(Platform::Ia64::Firmware::PalBootstrapCapabilities) == 8U);
static_assert(sizeof(Foundation::Result<Platform::Ia64::Firmware::PalBootstrapCapabilities,
                                        Platform::Ia64::Firmware::PalBootstrapCapabilityCaptureError>) == 112U);
static_assert(alignof(Foundation::Result<Platform::Ia64::Firmware::PalBootstrapCapabilities,
                                         Platform::Ia64::Firmware::PalBootstrapCapabilityCaptureError>) == 8U);
static_assert(sizeof(Foundation::Result<Platform::Ia64::Memory::PalStaticCallResult,
                                        Platform::Ia64::Firmware::PalStaticCallAdapterError>) == 40U);
static_assert(alignof(Foundation::Result<Platform::Ia64::Memory::PalStaticCallResult,
                                         Platform::Ia64::Firmware::PalStaticCallAdapterError>) == 8U);
static_assert(sizeof(Platform::Ia64::Memory::TranslationCapabilities) == 24U);
static_assert(alignof(Platform::Ia64::Memory::TranslationCapabilities) == 8U);
static_assert(sizeof(Platform::Ia64::Memory::RegionConfiguration) == 16U);
static_assert(alignof(Platform::Ia64::Memory::RegionConfiguration) == 4U);
static_assert(sizeof(Platform::Ia64::Memory::EncodedInitialTranslation) == 32U);
static_assert(alignof(Platform::Ia64::Memory::EncodedInitialTranslation) == 8U);
static_assert(sizeof(Platform::Ia64::Memory::InitialTranslationRegionSet) == 128U);
static_assert(alignof(Platform::Ia64::Memory::InitialTranslationRegionSet) == 4U);
static_assert(sizeof(Platform::Ia64::Memory::InitialTranslationResourceEntry) == 48U);
static_assert(alignof(Platform::Ia64::Memory::InitialTranslationResourceEntry) == 8U);
static_assert(sizeof(Platform::Ia64::Memory::InitialTranslationResourcePlanSummary) == 8U);
static_assert(sizeof(Platform::Ia64::Memory::InitialTranslationResourcePlanFailure) == 24U);
static_assert(alignof(Platform::Ia64::Memory::InitialTranslationResourcePlanFailure) == 8U);
static_assert(sizeof(Platform::Ia64::Memory::InitialTranslationResourcePlan) == 12'384U);
static_assert(alignof(Platform::Ia64::Memory::InitialTranslationResourcePlan) == 8U);
static_assert(sizeof(Platform::Ia64::Memory::WritableInterruptVectorTableView) == 24U);
static_assert(alignof(Platform::Ia64::Memory::WritableInterruptVectorTableView) == 8U);
static_assert(sizeof(Platform::Ia64::Memory::InitialInterruptVectorTablePreparationError) == 4U);
static_assert(sizeof(Platform::Ia64::Memory::InitialInterruptVectorTableInstallError) == 4U);
static_assert(sizeof(Platform::Ia64::Memory::InitialInterruptVectorTableInstallFailure) == 16U);
static_assert(alignof(Platform::Ia64::Memory::InitialInterruptVectorTableInstallFailure) == 8U);
static_assert(sizeof(Platform::Ia64::Memory::InitialInterruptVectorTableInstallSummary) == 16U);
static_assert(alignof(Platform::Ia64::Memory::InitialInterruptVectorTableInstallSummary) == 8U);
static_assert(sizeof(Foundation::Result<Platform::Ia64::Memory::InitialInterruptVectorTableInstallSummary,
                                        Platform::Ia64::Memory::InitialInterruptVectorTableInstallFailure>) == 40U);
static_assert(alignof(Foundation::Result<Platform::Ia64::Memory::InitialInterruptVectorTableInstallSummary,
                                         Platform::Ia64::Memory::InitialInterruptVectorTableInstallFailure>) == 8U);
static_assert(sizeof(Platform::Ia64::Memory::WritableRseBackingStoreView) == 24U);
static_assert(alignof(Platform::Ia64::Memory::WritableRseBackingStoreView) == 8U);
static_assert(sizeof(Platform::Ia64::Memory::InitialRseBackingStorePreparationError) == 4U);
static_assert(sizeof(Platform::Ia64::Memory::WritableInitialMemoryStackView) == 24U);
static_assert(alignof(Platform::Ia64::Memory::WritableInitialMemoryStackView) == 8U);
static_assert(sizeof(Platform::Ia64::Memory::InitialMemoryStackPreparationError) == 4U);
static_assert(sizeof(Platform::Ia64::Memory::InitialExecutionContextSwitchError) == 4U);
static_assert(sizeof(Platform::Ia64::Memory::InitialExecutionContextValidationSummary) == 32U);
static_assert(alignof(Platform::Ia64::Memory::InitialExecutionContextValidationSummary) == 8U);
static_assert(sizeof(Platform::Ia64::Memory::InitialExecutionContextSwitchFailure) == 16U);
static_assert(alignof(Platform::Ia64::Memory::InitialExecutionContextSwitchFailure) == 4U);
static_assert(sizeof(Foundation::Result<Platform::Ia64::Memory::InitialExecutionContextValidationSummary,
                                        Platform::Ia64::Memory::InitialExecutionContextSwitchFailure>) == 56U);
static_assert(alignof(Foundation::Result<Platform::Ia64::Memory::InitialExecutionContextValidationSummary,
                                         Platform::Ia64::Memory::InitialExecutionContextSwitchFailure>) == 8U);
static_assert(sizeof(Platform::Ia64::Memory::InitialAddressSpacePreparationStatus) == 4U);
static_assert(sizeof(Platform::Ia64::Memory::InitialAddressSpacePreparationError) == 4U);
static_assert(sizeof(Platform::Ia64::Memory::InitialAddressSpacePreparationSummary) == 40U);
static_assert(alignof(Platform::Ia64::Memory::InitialAddressSpacePreparationSummary) == 8U);
static_assert(sizeof(Platform::Ia64::Memory::InitialAddressSpacePreparationFailure) == 56U);
static_assert(alignof(Platform::Ia64::Memory::InitialAddressSpacePreparationFailure) == 8U);
static_assert(sizeof(Platform::Ia64::Memory::InitialAddressSpacePreparation) == 20'696U);
static_assert(alignof(Platform::Ia64::Memory::InitialAddressSpacePreparation) == 8U);
static_assert(sizeof(Foundation::Result<Platform::Ia64::Memory::InitialAddressSpacePreparationStatus,
                                        Platform::Ia64::Memory::InitialAddressSpacePreparationError>) == 12U);
static_assert(alignof(Foundation::Result<Platform::Ia64::Memory::InitialAddressSpacePreparationStatus,
                                         Platform::Ia64::Memory::InitialAddressSpacePreparationError>) == 4U);
static_assert(sizeof(Foundation::Result<const Platform::Ia64::Memory::InitialTranslationResourcePlan*,
                                        Platform::Ia64::Memory::InitialAddressSpacePreparationError>) == 16U);
static_assert(alignof(Foundation::Result<const Platform::Ia64::Memory::InitialTranslationResourcePlan*,
                                         Platform::Ia64::Memory::InitialAddressSpacePreparationError>) == 8U);
static_assert(sizeof(Platform::Ia64::Memory::InitialTranslationInstallError) == 4U);
static_assert(sizeof(Platform::Ia64::Memory::InitialTranslationInstallFailure) == 16U);
static_assert(alignof(Platform::Ia64::Memory::InitialTranslationInstallFailure) == 8U);
static_assert(sizeof(Platform::Ia64::Memory::InitialTranslationInstallSummary) == 16U);
static_assert(alignof(Platform::Ia64::Memory::InitialTranslationInstallSummary) == 8U);
static_assert(sizeof(Foundation::Result<Platform::Ia64::Memory::InitialTranslationInstallSummary,
                                        Platform::Ia64::Memory::InitialTranslationInstallFailure>) == 40U);
static_assert(alignof(Foundation::Result<Platform::Ia64::Memory::InitialTranslationInstallSummary,
                                         Platform::Ia64::Memory::InitialTranslationInstallFailure>) == 8U);
static_assert(sizeof(Platform::Ia64::Memory::PalStaticCallResult) == 32U);
static_assert(alignof(Platform::Ia64::Memory::PalStaticCallResult) == 8U);
static_assert(sizeof(Platform::Ia64::Memory::PalVirtualMemoryCapabilities) == 48U);
static_assert(alignof(Platform::Ia64::Memory::PalVirtualMemoryCapabilities) == 8U);
static_assert(sizeof(Platform::Synchronization::AtomicReferenceCount) == 4U);
static_assert(alignof(Platform::Synchronization::AtomicReferenceCount) == 4U);
static_assert(sizeof(Platform::Synchronization::SpinLock) == 8U);
static_assert(alignof(Platform::Synchronization::SpinLock) == 8U);
} // namespace Asteria::Test
