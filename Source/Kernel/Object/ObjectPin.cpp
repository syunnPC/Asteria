// SPDX-License-Identifier: Apache-2.0

#include "ObjectPin.hpp"

#include "Kernel/Object/ObjectRegistry.hpp"

namespace Asteria::Kernel::Object
{
ObjectPin::~ObjectPin() noexcept
{
    Reset();
}

ObjectPin::ObjectPin(ObjectPin&& other) noexcept
    : m_registry{other.m_registry}, m_object{other.m_object}, m_objectId{other.m_objectId},
      m_objectType{other.m_objectType}
{
    other.m_registry = nullptr;
    other.m_object = nullptr;
    other.m_objectId = ObjectId{};
    other.m_objectType = ObjectType::Invalid;
}

bool ObjectPin::IsValid() const noexcept
{
    return (m_registry != nullptr) && (m_object != nullptr) && m_objectId.IsValid();
}

ObjectId ObjectPin::GetObjectId() const noexcept
{
    return m_objectId;
}

ObjectType ObjectPin::GetObjectType() const noexcept
{
    return m_objectType;
}

IKernelObject& ObjectPin::GetObject() const noexcept
{
    return *m_object;
}

void ObjectPin::Reset() noexcept
{
    ObjectRegistry* const registry = m_registry;
    const ObjectId objectId = m_objectId;

    m_registry = nullptr;
    m_object = nullptr;
    m_objectId = ObjectId{};
    m_objectType = ObjectType::Invalid;

    if (registry != nullptr)
    {
        registry->ReleaseReference(objectId);
    }
}
} // namespace Asteria::Kernel::Object
