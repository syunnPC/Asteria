// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_PAGER_COMPONENT_LOADER_HPP_INCLUDED
#define ASTERIA_USER_PAGER_COMPONENT_LOADER_HPP_INCLUDED

#include "Abi/SystemCallAbi.hpp"
#include "Foundation/Types.hpp"
#include "User/Loader/ExecutableImage.hpp"
#include "User/Pager/ComponentRecord.hpp"
#include "User/Pager/DeviceManagerRole.hpp"
#include "User/Pager/ObjectCalls.hpp"
#include "User/Pager/PagerProgram.hpp"
#include "User/System/SystemCallClient.hpp"

namespace Asteria::User::Pager
{
/// The root service's loader: builds a component in a domain of its own and starts its thread. It
/// creates the component's ResourceDomain within its image's limits where the platform offers them,
/// its AddressSpace, CapabilitySpace and ProtectionDomain tagged for its supervision, maps its
/// segments and an interpreter's guest, then its stack and start block, staged through the pager's
/// window, with the capabilities its behavior needs (a report Endpoint's copy, a service's Endpoint
/// and WaitSet, a client's, an account incarnation's, a driver's device through the
/// DeviceManagerRole). It retains immutable shared image records for the pager run; incarnation
/// records are the ComponentSupervisor's.
class ComponentLoader final
{
public:
    constexpr ComponentLoader() noexcept = default;

    ComponentLoader(const ComponentLoader&) = delete;
    ComponentLoader& operator=(const ComponentLoader&) = delete;

    void Configure(ObjectCalls& calls, const PagerBootstrap& bootstrap, const PagerThreadLaunch& launch,
                   DeviceManagerRole& devices) noexcept;

    /// Builds the component build describes into component, whose Mode is
    /// set, and starts its thread unless the build is Deferred: component is
    /// Running then. On failure what was built stays in component, still
    /// Building, for the supervisor to reclaim.
    [[nodiscard, gnu::noinline]] bool Load(ComponentRecord& component, const ComponentBuild& build,
                                           Abi::SystemCallError& systemCall) noexcept;
    /// Creates the thread of a built component, which then runs.
    [[nodiscard, gnu::noinline]] bool StartThread(ComponentRecord& component,
                                                  Abi::SystemCallError& systemCall) noexcept;

    /// Completes any outstanding staging unmap before the window is reused.
    /// On failure the outstanding range remains recorded for a later retry.
    [[nodiscard]] bool CleanWindow(Abi::SystemCallError& systemCall) noexcept;

private:
    [[nodiscard, gnu::noinline]] bool PrepareUiFonts(ComponentRecord& component, const ComponentMapping& mapping,
                                                     volatile Foundation::UInt64* block, Foundation::UInt64 dataPages,
                                                     Abi::SystemCallError& systemCall) noexcept;
    [[nodiscard, gnu::noinline]] bool PrepareSession(ComponentRecord& component, const ComponentBuild& build,
                                                     const ComponentMapping& mapping,
                                                     volatile Foundation::UInt64* block, Foundation::UInt64 dataPages,
                                                     Abi::SystemCallError& systemCall) noexcept;
    [[nodiscard]] bool MapWindow(const ComponentMapping& mapping, Abi::SystemCallError& systemCall) noexcept;
    [[nodiscard]] bool AttenuateMap(System::CapabilityHandle& handle, Abi::SystemCallError& systemCall) noexcept;
    [[nodiscard, gnu::noinline]] bool AcquireImage(ComponentRecord& component, bool& staged,
                                                   Abi::SystemCallError& systemCall) noexcept;
    [[nodiscard, gnu::noinline]] bool StageSharedSegment(ComponentMapping& mapping,
                                                         const Loader::ExecutableSegment& segment,
                                                         const Foundation::UInt8* source,
                                                         Abi::SystemCallError& systemCall) noexcept;
    [[nodiscard, gnu::noinline]] bool ReportSharedMapping(const ComponentRecord& component, Foundation::Size segment,
                                                          bool staged, Abi::SystemCallError& systemCall) noexcept;
    /// The component's own ResourceDomain, within its image's limits, where
    /// the platform offers them; nothing otherwise.
    [[nodiscard, gnu::noinline]] bool CreateAccount(ComponentRecord& component, const LoadedImage& image,
                                                    Foundation::UInt64 serviceFlags,
                                                    Abi::SystemCallError& systemCall) noexcept;
    /// What component's objects are charged to: its own domain, or the grant.
    [[nodiscard]] System::CapabilityHandle AuthorityOf(const ComponentRecord& component) const noexcept;
    [[nodiscard, gnu::noinline]] bool CreateDomain(ComponentRecord& component, Foundation::UInt64 serviceFlags,
                                                   Abi::SystemCallError& systemCall) noexcept;
    /// Maps private zero-filled writable segments, or stages and seals
    /// non-executable foreign guest bytes, into the component's space.
    [[nodiscard, gnu::noinline]] bool LoadSegment(ComponentRecord& component, const Loader::ExecutableSegment& segment,
                                                  const Foundation::UInt8* source,
                                                  Abi::SystemCallError& systemCall) noexcept;
    /// Maps an interpreter's guest read-only at the platform's
    /// ForeignImageAddress.
    [[nodiscard, gnu::noinline]] bool LoadForeign(ComponentRecord& component,
                                                  Abi::SystemCallError& systemCall) noexcept;
    [[nodiscard, gnu::noinline]] bool LoadData(ComponentRecord& component, const ComponentBuild& build,
                                               Abi::SystemCallError& systemCall) noexcept;
    /// A service's Endpoint, sealed with selector, and its WaitSet, the
    /// Endpoint bound under tag, and their copies in its CapabilitySpace
    ///: Read on the Endpoint into service
    /// (start-block words 8 and 9), Wait on the WaitSet into waitSet (words 6
    /// and 7), and Manage besides for the block driver, which binds its
    /// Interrupt to it.
    [[nodiscard, gnu::noinline]] bool PrepareService(ComponentRecord& component, Foundation::UInt64 selector,
                                                     Foundation::UInt64 tag, System::CapabilityHandle& service,
                                                     System::CapabilityHandle& waitSet,
                                                     Abi::SystemCallError& systemCall) noexcept;
    /// A client service's driver and its driver-reply Endpoint of capacity replies per sender,
    /// bound to its WaitSet under tag, written into block.
    [[nodiscard, gnu::noinline]] bool PrepareDriverClient(ComponentRecord& component, const ComponentBuild& build,
                                                          volatile Foundation::UInt64* block, Foundation::Size capacity,
                                                          Foundation::UInt64 tag,
                                                          Abi::SystemCallError& systemCall) noexcept;
    /// The display broker's scanout: a copy of the start block's DeviceRegion and its mode, written
    /// into block.
    [[nodiscard, gnu::noinline]] bool PrepareScanout(ComponentRecord& component, volatile Foundation::UInt64* block,
                                                     Abi::SystemCallError& systemCall) noexcept;
    /// A client's Endpoint, unsealed, and its WaitSet as PrepareService
    /// makes them (words 6 to 9), then its copies of a send capability on
    /// that Endpoint (Invoke and Duplicate, word 12 and 13) and on the
    /// Directory's (Invoke, words 10 and 11), written into block.
    [[nodiscard, gnu::noinline]] bool PrepareClient(ComponentRecord& component, System::CapabilityHandle directory,
                                                    volatile Foundation::UInt64* block,
                                                    Abi::SystemCallError& systemCall) noexcept;
    /// An account incarnation's copies of its own ResourceDomain (Manage and Inspect) and
    /// AddressSpace (Map) and the platform's window, written into block.
    [[nodiscard, gnu::noinline]] bool PrepareAccount(ComponentRecord& component, volatile Foundation::UInt64* block,
                                                     Abi::SystemCallError& systemCall) noexcept;

    ObjectCalls* m_calls{nullptr};
    const PagerBootstrap* m_bootstrap{nullptr};
    const PagerThreadLaunch* m_launch{nullptr};
    DeviceManagerRole* m_devices{nullptr};
    SharedImageRecord m_images[MaximumComponentImages]{};
    Foundation::UInt64 m_imageGeneration{0U};
    Foundation::UInt64 m_stagingPages{0U};
};
} // Namespace Asteria::User::Pager

#endif // ASTERIA_USER_PAGER_COMPONENT_LOADER_HPP_INCLUDED
