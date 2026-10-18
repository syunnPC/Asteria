// SPDX-License-Identifier: Apache-2.0

#include "User/Imaging/CursorSet.hpp"
#include "User/Imaging/Internal.hpp"
namespace Asteria::User::Imaging
{
Status DecodeCursorSet(Bytes input, Buffer arena, Workspace& work, CursorSet& set) noexcept
{
    using Detail::Input;
    using Detail::Le16;
    using Detail::Le32;
    using Detail::Range;
    set.Count = 0U;
    if (!Input(input) || arena.Data == nullptr)
    {
        return input.Count > MaximumInput ? Status::Limit : Status::InvalidBuffer;
    }
    if (input.Count < CursorHeaderBytes)
    {
        return Status::Truncated;
    }
    if (Le32(input.Data) != 0x52534341U)
    {
        return Status::Signature;
    }
    if (Le16(input.Data + 4U) != CursorVersion)
    {
        return Status::Unsupported;
    }
    if (Le16(input.Data + 6U) != CursorRoleCount || Le32(input.Data + 12U) != 0U)
    {
        return Status::Header;
    }
    if (Le32(input.Data + 8U) != input.Count)
    {
        return Status::SizeMismatch;
    }
    Size pos{CursorHeaderBytes};
    Size used{0U};
    UInt32 count{0U};
    for (UInt32 role{0U}; role < CursorRoleCount; ++role)
    {
        if (!Range(pos, CursorRoleBytes, input.Count))
        {
            return Status::Truncated;
        }
        if (Le16(input.Data + pos) != role)
        {
            return Status::Order;
        }
        const UInt32 frames{Le16(input.Data + pos + 2U)};
        pos += CursorRoleBytes;
        if (frames == 0U || frames > MaximumFramesPerRole)
        {
            return Status::Limit;
        }
        set.First[role] = count;
        set.Length[role] = frames;
        for (UInt32 frame{0U}; frame < frames; ++frame)
        {
            if (!Range(pos, CursorFrameBytes, input.Count))
            {
                return Status::Truncated;
            }
            const UInt8* p{input.Data + pos};
            const UInt32 w{Le16(p)};
            const UInt32 h{Le16(p + 2U)};
            const UInt32 hx{Le16(p + 4U)};
            const UInt32 hy{Le16(p + 6U)};
            const UInt32 delay{Le32(p + 8U)};
            const UInt32 encoding{Le32(p + 12U)};
            const UInt32 bytes{Le32(p + 16U)};
            pos += CursorFrameBytes;
            if (w == 0U || h == 0U)
            {
                return Status::Header;
            }
            if (w > MaximumCursorDimension || h > MaximumCursorDimension)
            {
                return Status::Limit;
            }
            if (hx >= w || hy >= h)
            {
                return Status::Hotspot;
            }
            if ((frames == 1U && delay != 0U) || (frames > 1U && (delay == 0U || delay > MaximumFrameDelay)))
            {
                return Status::Header;
            }
            const Size pixels{static_cast<Size>(w) * h * 4U};
            if (!Range(used, pixels, arena.Count) || !Range(used, pixels, static_cast<Size>(MaximumPixels) * 4U))
            {
                return Status::Limit;
            }
            if (!Range(pos, bytes, input.Count))
            {
                return Status::Truncated;
            }
            CursorFrame& target{set.Frames[count++]};
            target.Pixels = {};
            if (encoding == 0U)
            {
                if (bytes != pixels)
                {
                    return Status::SizeMismatch;
                }
                for (Size i{0U}; i < pixels; ++i)
                {
                    arena.Data[used + i] = input.Data[pos + i];
                }
                target.Pixels = {arena.Data + used, pixels, w, h};
            }
            else if (encoding == 1U)
            {
                const Status s{DecodePng({input.Data + pos, bytes}, {arena.Data + used, pixels}, work, target.Pixels)};
                if (s != Status::Ok)
                {
                    return s;
                }
                if (target.Pixels.Width != w || target.Pixels.Height != h)
                {
                    return Status::SizeMismatch;
                }
            }
            else
            {
                return Status::Unsupported;
            }
            target.HotspotX = static_cast<UInt16>(hx);
            target.HotspotY = static_cast<UInt16>(hy);
            target.DelayMs = delay;
            target.Role = static_cast<CursorRole>(role);
            used += pixels;
            pos += bytes;
        }
    }
    if (pos != input.Count)
    {
        return Status::SizeMismatch;
    }
    set.Count = count;
    return Status::Ok;
}
} // namespace Asteria::User::Imaging
