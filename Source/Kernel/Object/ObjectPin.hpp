// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_OBJECT_OBJECT_PIN_HPP_INCLUDED
#define ASTERIA_KERNEL_OBJECT_OBJECT_PIN_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Kernel/Object/CapabilityTypes.hpp"
#include "Kernel/Object/IKernelObject.hpp"

namespace Asteria::Kernel::Object
{
class ObjectRegistry;

enum class ObjectPinError : Foundation::UInt8
{
    Ok,
    InvalidArgument,
    OutputOccupied,
    ObjectNotFound,
    TypeMismatch,
    ReferenceExhausted,
};

/// Holds a persistent kernel-internal reference to one generation of an object.
/// Unlike CapabilityLease, this reference may be retained across dispatches. It grants no
/// authority and therefore must be created only after the caller has independently authorized
/// the relationship being installed.
/// Lifetime:
/// - The originating ObjectRegistry outlives this pin and every move-constructed successor.
/// - Callers serialize access to the pin object itself with move construction or Reset.
class ObjectPin final
{
public:
    constexpr ObjectPin() noexcept = default;

    /// Releases the reference with one wait-free atomic operation when valid.
    ~ObjectPin() noexcept;

    ObjectPin(const ObjectPin&) = delete;
    ObjectPin& operator=(const ObjectPin&) = delete;
    ObjectPin(ObjectPin&& other) noexcept;
    ObjectPin& operator=(ObjectPin&&) = delete;

    [[nodiscard]] bool IsValid() const noexcept;

    /// Preconditions:
    /// - IsValid is true.
    [[nodiscard]] ObjectId GetObjectId() const noexcept;

    /// Preconditions:
    /// - IsValid is true.
    [[nodiscard]] ObjectType GetObjectType() const noexcept;

    /// The returned reference must not be retained or used after this pin becomes invalid unless
    /// the caller independently holds another lease or pin for the same object generation.
    /// Preconditions:
    /// - IsValid is true.
    [[nodiscard]] IKernelObject& GetObject() const noexcept;

    /// Releases the held reference and returns this pin to its empty state.
    /// The caller has exclusive access to this pin.
    void Reset() noexcept;

private:
    friend class ObjectRegistry;

    ObjectRegistry* m_registry{nullptr};
    IKernelObject* m_object{nullptr};
    ObjectId m_objectId{};
    ObjectType m_objectType{ObjectType::Invalid};
};
} // Namespace Asteria::Kernel::Object

#endif // ASTERIA_KERNEL_OBJECT_OBJECT_PIN_HPP_INCLUDED
