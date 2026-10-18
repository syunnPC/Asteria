// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_PAGER_COMPONENT_RECORD_HPP_INCLUDED
#define ASTERIA_USER_PAGER_COMPONENT_RECORD_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "User/Loader/ExecutableImage.hpp"
#include "User/Pager/ComponentImage.hpp"
#include "User/System/SystemCallClient.hpp"

namespace Asteria::User::Pager
{
/// The components the root service supervises at once: one per slot of the kernel's domain pools.
inline constexpr Foundation::Size MaximumComponents{Abi::ComponentImageMaximumCount};

/// One mapping of a component's space and the objects behind it.
struct ComponentMapping
{
    System::CapabilityHandle Frame;
    System::CapabilityHandle Memory;
    Foundation::UInt64 VirtualStart;
    Foundation::UInt64 Pages;
    /// Borrowed from an immutable image record charged to the pager grant.
    /// Component teardown must neither destroy nor close these handles.
    bool Shared{false};
};

/// The segments, then the stack and start block, and an interpreter's
/// foreign executable.
inline constexpr Foundation::Size MaximumComponentMappings{MaximumComponentSegments + 2U};

/// The executable an interpreter is given: its bytes in the image table and its identity; empty
/// otherwise.
struct ForeignImage
{
    const Foundation::UInt8* Bytes;
    Foundation::Size Size;
    Loader::ExecutableIdentity Identity;
};

/// An image of the launch's table and its layout.
struct LoadedImage
{
    const Foundation::UInt8* Bytes;
    Foundation::Size Size;
    Loader::ExecutableImage Layout;
};

/// One immutable version of an embedded image, retained for this pager run. Segments are indexed
/// like LoadedImage::Layout; writable entries stay empty. Published handles carry Map only. Records
/// are never retired during a run. A failed staging leaves the record unavailable, with its
/// grant-owned objects retained: abandonment must not publish partially staged code.
struct SharedImageRecord
{
    const Foundation::UInt8* Bytes;
    Foundation::Size Size;
    Foundation::UInt64 Generation;
    ComponentMapping Segments[MaximumComponentSegments];
    bool Ready;
};

enum class ComponentState : Foundation::UInt8
{
    Free,
    /// Being built: what the loader created so far is in the record, and
    /// stays there should the build fail, for the supervisor to reclaim.
    Building,
    /// Built and started; its thread has not been reclaimed.
    Running,
    /// Its thread was reclaimed with ExitCode; its domain still stands.
    Ended,
};

/// How the supervisor treats a component: a task under its watchdog until it exits; a service
/// without an exit deadline, which ends only by a fault or a kill.
enum class ComponentMode : Foundation::UInt8
{
    Task,
    Service,
};

/// A component without an assignable device.
inline constexpr Foundation::UInt8 ComponentNoDevice{0xFFU};
/// The USB host group driver's record: every function of the USB host
/// table it was granted (UsbHostDriver.md), each confirmed quiet on its own
/// after the group's drain.
inline constexpr Foundation::UInt8 ComponentUsbHostGroup{0xFEU};

/// One component the root service built: its domain, memory and thread.
/// The ComponentLoader fills it, the ComponentSupervisor owns it.
struct ComponentRecord
{
    /// The image its thread runs.
    const LoadedImage* Image;
    SharedImageRecord* SharedImage;
    /// Its supervision tag, the incarnation number of its domain.
    Foundation::UInt64 Tag;
    Foundation::UInt64 Behavior;
    /// Its own ResourceDomain, a child of the grant charged with its writable Frames and objects,
    /// except shared image objects charged to the Code-bearing grant; zero where the platform
    /// offers none, and the grant is charged.
    System::CapabilityHandle Account;
    System::CapabilityHandle AccountParent{};
    System::CapabilityHandle FaultChannel{};
    System::CapabilityHandle Space;
    System::CapabilityHandle Capabilities;
    System::CapabilityHandle Domain;
    System::CapabilityHandle Thread;
    /// A service's Endpoint, receiving in its CapabilitySpace, and the
    /// WaitSet it is bound to; zero for a task.
    System::CapabilityHandle Service;
    System::CapabilityHandle WaitSet;
    System::CapabilityHandle FsManager{};
    /// An interpreter's guest.
    ForeignImage Foreign;
    ComponentMapping Mappings[MaximumComponentMappings];
    Foundation::Size MappingCount;
    /// The entry its thread starts at: the image's {ip, gp} where the
    /// ISA names entries by descriptor.
    alignas(16) Foundation::UInt64 EntryDescriptor[2U];
    /// The pager's count of crashed components when it started.
    Foundation::UInt64 CrashesAtStart;
    /// How long a wait may pass without a fault or an exit before the
    /// watchdog kills it.
    Foundation::UInt64 WatchdogMicroseconds;
    Foundation::UInt64 ExitCode;
    /// The assignable device granted to it (an Abi::PagerDevice* index), so its drain leaves the
    /// device quarantined for the Device Manager to confirm quiet; ComponentUsbHostGroup for the
    /// USB host table's functions; ComponentNoDevice for none.
    Foundation::UInt8 Device;
    ComponentMode Mode;
    ComponentState State;
    /// Product init reports an optional input failure once, even if quarantine
    /// prevents complete teardown.
    bool FailureReported{false};
};

/// What a component is built from, and the root service's Endpoints it is
/// linked to: a task's report Endpoint, a client's Directory.
struct ComponentBuild
{
    const LoadedImage* Image;
    Foundation::UInt64 Tag;
    Foundation::UInt64 Behavior;
    /// An interpreter's guest; empty otherwise.
    ForeignImage Foreign;
    System::CapabilityHandle Report;
    System::CapabilityHandle Directory;
    /// A client service's driver (a filesystem service's block driver, a
    /// network stack's NIC driver): the root service's send capability on
    /// the driver's Endpoint and the driver instance's tag; zero otherwise.
    System::CapabilityHandle DriverService;
    Foundation::UInt64 DriverGeneration;
    /// Its mount flags (Abi::ComponentMount*) or its network configuration (Abi:
    /// ComponentNetworkConfigurationWord).
    Foundation::UInt64 ServiceFlags;
    /// Built without its thread, which ComponentSupervisor::Launch starts: requests sent to its
    /// Endpoint meanwhile are queued before it runs.
    bool Deferred;
    // Optional supervisor account and channel.
    System::CapabilityHandle AccountParent{};
    System::CapabilityHandle FaultChannel{};
    System::CapabilityHandle SessionInput{};
    Foundation::UInt64 SessionInputEpoch{0U};
    System::CapabilityHandle IdentityLimiter{};
};
} // Namespace Asteria::User::Pager

#endif // ASTERIA_USER_PAGER_COMPONENT_RECORD_HPP_INCLUDED
