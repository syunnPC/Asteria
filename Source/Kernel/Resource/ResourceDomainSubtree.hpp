// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_RESOURCE_RESOURCE_DOMAIN_SUBTREE_HPP_INCLUDED
#define ASTERIA_KERNEL_RESOURCE_RESOURCE_DOMAIN_SUBTREE_HPP_INCLUDED

namespace Asteria::Kernel::Resource
{
class ResourceDomain;

/// A ResourceDomain and its descendants, which a drain destroys everything charged to. A pool asks
/// it under its own lock, so Contains takes only locks that order after every pool's.
class IResourceDomainSubtree
{
public:
    [[nodiscard]] virtual bool Contains(const ResourceDomain* domain) const noexcept = 0;

protected:
    constexpr IResourceDomainSubtree() noexcept = default;
    ~IResourceDomainSubtree() noexcept = default;
};
} // Namespace Asteria::Kernel::Resource

#endif // ASTERIA_KERNEL_RESOURCE_RESOURCE_DOMAIN_SUBTREE_HPP_INCLUDED
