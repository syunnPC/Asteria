// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_COMPONENT_FILESYSTEM_SERVICE_HPP_INCLUDED
#define ASTERIA_USER_COMPONENT_FILESYSTEM_SERVICE_HPP_INCLUDED

#include "Abi/ComponentBootstrap.hpp"
#include "Foundation/Types.hpp"
#include "User/Component/ComponentProgram.hpp"
#include "User/System/SystemCallClient.hpp"

namespace Asteria::User::Component
{
/// What the filesystem service takes from its image besides the start block: its crash (as
/// RunComponent's), and the address of its two-page window, the start block's (Abi:
/// ComponentWindowStartWord; a hosted test passes its own buffer's, its kernel model mapping
/// nothing).
struct FilesystemPlatform
{
    ComponentCrash Crash;
    Foundation::UInt64 Window;
};

/// The filesystem service, a client of the block service its start block names (words 34-39) and a
/// service of the file protocol (FsFrame.hpp) on its Endpoint, until it is killed. At start it
/// creates, in its own domain (words 24-27), a one-page bounce Frame mapped for good at the
/// window's second page, and the MemoryObject through which the window's first page maps one
/// client's data Frame for one request at a time. Then it mounts its volume through a synchronous
/// sector device over the block protocol, first Describe for 512-byte blocks, real sector capacity
/// and the medium generation: one block request outstanding, sent with a copy of its block-reply
/// Endpoint (Invoke) and a Map-only copy of the bounce Frame, awaited on its WaitSet (a wake for
/// its service Endpoint meanwhile is left to the loop, which drains that Endpoint after every
/// wake); BlockDone with the sectors asked is success, any other reply a device failure. Sector 0
/// decides the backend: a FAT parameter block mounts FAT (its serial the incarnation, word 1);
/// otherwise AsteriaFS mounts, and a blank volume is formatted first when the mount flags (word 37)
/// carry Abi::ComponentMountFormatBlank. A volume that mounts neither way is answered FsCorrupt
/// (FsFailed for a device failure) to every request. Requests are served one at a time, in order,
/// each answered on the reply capability it carried, which is then closed with its data Frame. A
/// request whose tag is not the incarnation is answered FsRestarted; a frame that is not a request
/// of the file protocol through the sealed Endpoint is dropped with every capability it carried
/// closed. AsteriaFS data never enters the service's space: a Write allocates its block and
/// forwards a Map-only copy of the client's Frame to the block service, a Read forwards it for the
/// block the object table names, and only a block the file does not hold is zeroed through the
/// window. FAT data passes through the window's first page. A transaction is bound to the sender's
/// identity, the ResourceDomainId the kernel writes at byte 24 of each delivery's header
/// (IpcDeliveryCodec). After a device failure in an AsteriaFS write or commit, or in any FAT
/// operation, the service mounts the volume again, which discards what was staged; a mount that
/// fails leaves every later request answered FsFailed or FsCorrupt. MediumChanged unmounts without
/// retry; all clients receive FsFailed/BlockMediumChanged. A non-512 block size leaves the service
/// unmounted with FsUnsupported. A Commit with FsCrashBeforeSuperblock or FsCrashAfterSuperblock
/// crashes the service through platform.Crash with the start block's crash kind: on AsteriaFS just
/// before the superblock sector's write is sent, or just after its BlockDone; on FAT at FatVolume:
/// Commit's crash point. The Commit is not answered. Returns only a failure's exit code (Abi:
/// ComponentFilesystemFailedExitCode with its step and error), or Abi:
/// ComponentCrashReturnedExitCode when the crash returned. Blocks in WaitSetWait.
[[nodiscard]] Foundation::UInt64 ServeFilesystem(const System::SystemCallClient& client,
                                                 const Foundation::UInt64 (&startBlock)[Abi::ComponentBlockWords],
                                                 const FilesystemPlatform& platform) noexcept;
} // Namespace Asteria::User::Component

#endif // ASTERIA_USER_COMPONENT_FILESYSTEM_SERVICE_HPP_INCLUDED
