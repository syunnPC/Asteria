// SPDX-License-Identifier: Apache-2.0

#include "User/Imaging/Internal.hpp"
namespace Asteria::User::Imaging
{
Status DecodeIco(Bytes input, UInt32 requestedWidth, UInt32 requestedHeight, Buffer output, Workspace& work,
                 Image& image) noexcept
{
    using Detail::Dib;
    using Detail::Input;
    using Detail::Le16;
    using Detail::Le32;
    using Detail::Range;
    image = {};
    if (!Input(input))
    {
        return input.Count > MaximumInput ? Status::Limit : Status::InvalidBuffer;
    }
    if (requestedWidth == 0U || requestedHeight == 0U || requestedWidth > MaximumDimension ||
        requestedHeight > MaximumDimension)
    {
        return Status::Header;
    }
    if (input.Count < 6U)
    {
        return Status::Truncated;
    }
    if (Le16(input.Data) != 0U || Le16(input.Data + 2U) != 1U)
    {
        return Status::Signature;
    }
    const UInt32 count{Le16(input.Data + 4U)};
    if (count == 0U)
    {
        return Status::Header;
    }
    if (count > MaximumIcons)
    {
        return Status::Limit;
    }
    const Size directory{6U + static_cast<Size>(count) * 16U};
    if (directory > input.Count)
    {
        return Status::Truncated;
    }
    UInt32 best{0U};
    UInt32 bestScore{0xFFFFFFFFU};
    UInt32 bestArea{0U};
    for (UInt32 i{0U}; i < count; ++i)
    {
        const UInt8* entry{input.Data + 6U + i * 16U};
        const UInt32 w{entry[0] == 0U ? 256U : entry[0]};
        const UInt32 h{entry[1] == 0U ? 256U : entry[1]};
        const Size bytes{Le32(entry + 8U)};
        const Size offset{Le32(entry + 12U)};
        if (entry[3] != 0U || Le16(entry + 4U) > 1U)
        {
            return Status::Header;
        }
        const UInt32 depth{Le16(entry + 6U)};
        if (depth != 0U && depth != 1U && depth != 4U && depth != 8U && depth != 16U && depth != 24U && depth != 32U)
        {
            return Status::Header;
        }
        if (offset < directory || bytes == 0U || !Range(offset, bytes, input.Count))
        {
            return Status::Truncated;
        }
        for (UInt32 prior{0U}; prior < i; ++prior)
        {
            const UInt8* other{input.Data + 6U + prior * 16U};
            const Size start{Le32(other + 12U)};
            const Size length{Le32(other + 8U)};
            if (offset < start + length && start < offset + bytes)
            {
                return Status::Header;
            }
        }
        const UInt32 score{(w > requestedWidth ? w - requestedWidth : requestedWidth - w) +
                           (h > requestedHeight ? h - requestedHeight : requestedHeight - h)};
        if (score < bestScore || (score == bestScore && w * h > bestArea))
        {
            best = i;
            bestScore = score;
            bestArea = w * h;
        }
    }
    const UInt8* entry{input.Data + 6U + best * 16U};
    const Bytes payload{input.Data + Le32(entry + 12U), Le32(entry + 8U)};
    Status status{payload.Count >= 8U && payload.Data[0] == 137U ? DecodePng(payload, output, work, image)
                                                                 : Dib(payload, 0U, true, output, image)};
    if (status != Status::Ok)
    {
        return status;
    }
    const UInt32 w{entry[0] == 0U ? 256U : entry[0]};
    const UInt32 h{entry[1] == 0U ? 256U : entry[1]};
    if (image.Width != w || image.Height != h)
    {
        image = {};
        return Status::SizeMismatch;
    }
    return Status::Ok;
}
} // namespace Asteria::User::Imaging
