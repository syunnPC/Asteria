// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_COMPONENT_FS_VIEWS_HPP_INCLUDED
#define ASTERIA_USER_COMPONENT_FS_VIEWS_HPP_INCLUDED
#include "User/Component/FsFrame.hpp"
namespace Asteria::User::Component
{
// Authority comes from the delivery's ResourceDomain identity. Slots are private
// to a view. Closed ancestors remain pinned by their children for lexical '..'; reopening one
// issues a new generation. No identifier wraps.
struct FsNode
{
    Foundation::UInt64 Object{0U};
    Foundation::UInt64 Birth{0U};
    Foundation::UInt64 ParentBytes{0U};
    Filesystem::NodeKind Kind{static_cast<Filesystem::NodeKind>(0U)};
    bool ParentContiguous{false};
};
class FsViews final
{
public:
    struct Slot
    {
        FsNode Node{};
        Foundation::UInt64 Generation{0U};
        Foundation::UInt16 Children{0U};
        Foundation::UInt8 Parent{0U};
        bool Open{false};
        bool Used{false};
    };
    struct View
    {
        Foundation::UInt64 Id{0U};
        Foundation::UInt64 Owner{0U};
        Foundation::UInt64 Rights{0U};
        Slot Slots[FsOpenSlots]{};
    };
    void Initialize() noexcept
    {
        m_next = 0U;
        Reset();
    }
    void Reset() noexcept
    {
        for (auto& view : m_views)
        {
            view.Id = 0U;
        }
    }
    [[nodiscard]] View* Find(Foundation::UInt64 id, Foundation::UInt64 owner) noexcept
    {
        for (auto& view : m_views)
        {
            if (view.Id == id && id != 0U && view.Owner == owner)
            {
                return &view;
            }
        }
        return nullptr;
    }
    [[nodiscard]] View* ManagerFind(Foundation::UInt64 id) noexcept
    {
        for (auto& view : m_views)
        {
            if (view.Id == id && id != 0U)
            {
                return &view;
            }
        }
        return nullptr;
    }
    [[nodiscard]] Slot* Resolve(View& view, Foundation::UInt64 slot, Foundation::UInt64 generation) noexcept
    {
        if (slot >= FsOpenSlots)
        {
            return nullptr;
        }
        auto& entry = view.Slots[slot];
        return entry.Open && entry.Generation == generation ? &entry : nullptr;
    }
    [[nodiscard]] View* Install(Foundation::UInt64 owner, Foundation::UInt64 rights, const FsNode& root) noexcept
    {
        if (owner == 0U || root.Kind != Filesystem::NodeKind::Directory || rights == 0U ||
            (rights & ~FsAllRights) != 0U || m_next == ~Foundation::UInt64{0U})
        {
            return nullptr;
        }
        for (auto& view : m_views)
        {
            if (view.Id != 0U)
            {
                continue;
            }
            view.Owner = owner;
            view.Rights = rights;
            view.Id = ++m_next;
            for (auto& slot : view.Slots)
            {
                slot = {};
            }
            view.Slots[0U] = Slot{.Node = root, .Generation = view.Id, .Open = true, .Used = true};
            return &view;
        }
        return nullptr;
    }
    [[nodiscard]] Foundation::Size Open(View& view, const FsNode& node, Foundation::Size parent) noexcept
    {
        if (m_next == ~Foundation::UInt64{0U})
        {
            return FsOpenSlots;
        }
        for (Foundation::Size index{1U}; index < FsOpenSlots; ++index)
        {
            auto& entry = view.Slots[index];
            if (entry.Used)
            {
                continue;
            }
            entry = Slot{.Node = node,
                         .Generation = ++m_next,
                         .Parent = static_cast<Foundation::UInt8>(parent),
                         .Open = true,
                         .Used = true};
            ++view.Slots[parent].Children;
            return index;
        }
        return FsOpenSlots;
    }
    [[nodiscard]] Foundation::Size Parent(View& view, Foundation::Size index) noexcept
    {
        const auto parent = view.Slots[index].Parent;
        auto& entry = view.Slots[parent];
        if (!entry.Open)
        {
            if (m_next == ~Foundation::UInt64{0U})
            {
                return FsOpenSlots;
            }
            entry.Generation = ++m_next;
            entry.Open = true;
        }
        return parent;
    }
    void Close(View& view, Foundation::Size index) noexcept
    {
        view.Slots[index].Open = false;
        if (index == 0U)
        {
            return;
        }
        for (Foundation::Size step{0U}; step < FsOpenSlots && index != 0U; ++step)
        {
            auto& entry = view.Slots[index];
            if (entry.Open || entry.Children != 0U)
            {
                break;
            }
            const auto parent = entry.Parent;
            entry.Used = false;
            --view.Slots[parent].Children;
            index = parent;
        }
    }
    // Owner zero invalidates all object slots after a backend remount.
    [[gnu::noinline]] void InvalidateOwnerSlots(Foundation::UInt64 owner) noexcept
    {
        for (auto& view : m_views)
        {
            if (view.Id == 0U || (owner != 0U && view.Owner != owner))
            {
                continue;
            }
            view.Slots[0U].Children = 0U;
            for (Foundation::Size index{1U}; index < FsOpenSlots; ++index)
            {
                view.Slots[index] = {};
            }
        }
    }
    [[nodiscard]] bool HasFree(const View& view) const noexcept
    {
        if (m_next == ~Foundation::UInt64{0U})
        {
            return false;
        }
        for (const auto& slot : view.Slots)
        {
            if (!slot.Used)
            {
                return true;
            }
        }
        return false;
    }

private:
    View m_views[FsViewCapacity]{};
    Foundation::UInt64 m_next{0U};
};
static_assert(FsOpenSlots <= 256U);
static_assert(sizeof(FsViews::Slot) <= 48U);
static_assert(sizeof(FsViews) <= FsViewCapacity * (FsOpenSlots * 48U + 24U) + 8U);
} // Namespace Asteria::User::Component
#endif
