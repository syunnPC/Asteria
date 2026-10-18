// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_FOUNDATION_INDEX_SEQUENCE_HPP_INCLUDED
#define ASTERIA_FOUNDATION_INDEX_SEQUENCE_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::Foundation
{
/// The indices 0 to Count - 1 as a pack, so a constructor can initialize an
/// array of objects that have no default constructor (each element named
/// with its index) whatever the array's length. The freestanding kernel has
/// no standard library, so it carries its own.
template <Size... Indices> struct IndexSequence
{
};

namespace Detail
{
template <Size Count, Size... Indices> struct MakeIndexSequence
{
    using Type = typename MakeIndexSequence<Count - 1U, Count - 1U, Indices...>::Type;
};

template <Size... Indices> struct MakeIndexSequence<0U, Indices...>
{
    using Type = IndexSequence<Indices...>;
};
} // namespace Detail

template <Size Count> using MakeIndexSequence = typename Detail::MakeIndexSequence<Count>::Type;
} // namespace Asteria::Foundation

#endif // ASTERIA_FOUNDATION_INDEX_SEQUENCE_HPP_INCLUDED
