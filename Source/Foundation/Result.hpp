// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_FOUNDATION_RESULT_HPP_INCLUDED
#define ASTERIA_FOUNDATION_RESULT_HPP_INCLUDED

namespace Asteria::Foundation
{
template <typename TValue, typename TError> class [[nodiscard]] Result final
{
    static_assert(noexcept(TValue{}));
    static_assert(noexcept(TError{}));
    static_assert(noexcept(TValue{*static_cast<const TValue*>(nullptr)}));
    static_assert(noexcept(TError{*static_cast<const TError*>(nullptr)}));
    static_assert(noexcept(static_cast<TValue*>(nullptr)->~TValue()));
    static_assert(noexcept(static_cast<TError*>(nullptr)->~TError()));

public:
    [[nodiscard]] static constexpr Result Success(const TValue& value) noexcept
    {
        return Result{value, TError{}, true};
    }

    [[nodiscard]] static constexpr Result Failure(const TError& error) noexcept
    {
        return Result{TValue{}, error, false};
    }

    [[nodiscard]] constexpr bool HasValue() const noexcept
    {
        return m_hasValue;
    }

    /// Returns the stored value.
    /// Preconditions:
    /// - HasValue is true.
    [[nodiscard]] constexpr const TValue& GetValue() const noexcept
    {
        return m_value;
    }

    /// Returns the stored error, or the default-constructed error when successful.
    [[nodiscard]] constexpr TError GetError() const noexcept
    {
        return m_error;
    }

    /// Returns the stored error without copying it.
    /// Preconditions:
    /// - HasValue is false.
    [[nodiscard]] constexpr const TError& GetErrorReference() const noexcept
    {
        return m_error;
    }

private:
    constexpr Result(const TValue& value, const TError& error, const bool hasValue) noexcept
        : m_value{value}, m_error{error}, m_hasValue{hasValue}
    {
    }

    TValue m_value;
    TError m_error;
    bool m_hasValue;
};
} // Namespace Asteria::Foundation

#endif // ASTERIA_FOUNDATION_RESULT_HPP_INCLUDED
