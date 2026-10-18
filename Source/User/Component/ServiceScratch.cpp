// SPDX-License-Identifier: Apache-2.0

#include "User/Component/ServiceScratch.hpp"

namespace Asteria::User::Component
{
namespace
{
struct ScratchPlacement
{
    void* Address;
};
constinit ServiceScratch g_scratch{};
} // namespace
} // namespace Asteria::User::Component

[[nodiscard]] inline void* operator new(Asteria::Foundation::Size,
                                        Asteria::User::Component::ScratchPlacement place) noexcept
{
    return place.Address;
}

namespace Asteria::User::Component
{
void ServiceScratch::InitializeInput() noexcept
{
    m_input = new (ScratchPlacement{m_bytes}) InputCore{};
}
void ServiceScratch::InitializeFilesystem() noexcept
{
    m_filesystem = new (ScratchPlacement{m_bytes}) FilesystemCore{};
}
void ServiceScratch::InitializePartitions() noexcept
{
    static_cast<void>(new (ScratchPlacement{&m_filesystem->Volume}) Filesystem::Partitions{});
}
void ServiceScratch::InitializeNative() noexcept
{
    static_cast<void>(new (ScratchPlacement{&m_filesystem->Volume}) Filesystem::FsVolume{});
}
void ServiceScratch::InitializeFat() noexcept
{
    static_cast<void>(new (ScratchPlacement{&m_filesystem->Volume}) Filesystem::FatVolume{});
}
void ServiceScratch::InitializeExfat() noexcept
{
    static_cast<void>(new (ScratchPlacement{&m_filesystem->Volume}) Filesystem::ExfatVolume{});
}
void ServiceScratch::InitializeXhci() noexcept
{
    m_xhci = new (ScratchPlacement{m_bytes}) XhciCore{};
}
FilesystemCore& ServiceScratch::FilesystemState() const noexcept
{
    return *m_filesystem;
}
XhciCore& ServiceScratch::Xhci() const noexcept
{
    return *m_xhci;
}
InputCore& ServiceScratch::Input() const noexcept
{
    return *m_input;
}
Filesystem::FsVolume& ServiceScratch::Filesystem() const noexcept
{
    return m_filesystem->Volume.Native;
}
ServiceScratch& Scratch() noexcept
{
    return g_scratch;
}
} // namespace Asteria::User::Component
