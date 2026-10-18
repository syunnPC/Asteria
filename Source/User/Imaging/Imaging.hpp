// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_IMAGING_IMAGING_HPP_INCLUDED
#define ASTERIA_USER_IMAGING_IMAGING_HPP_INCLUDED
#include "Foundation/Types.hpp"
namespace Asteria::User::Imaging
{
using Foundation::UInt8;
using Foundation::UInt16;
using Foundation::UInt32;
using Foundation::UInt64;
using Foundation::Int32;
using Foundation::Size;
// Bounded assets are decoded outside the broker. Caller storage must be
// disjoint and stable.
inline constexpr UInt32 MaximumDimension = 4096U;
inline constexpr UInt32 MaximumPixels = 1048576U;
inline constexpr Size MaximumInput = 32U * 1024U * 1024U;
inline constexpr Size MaximumInflated = 16U * 1024U * 1024U;
inline constexpr UInt32 WindowBytes = 32768U;
inline constexpr UInt32 MaximumChunks = 1024U;
inline constexpr UInt32 MaximumBlocks = 65536U;
inline constexpr UInt32 MaximumIcons = 64U;
inline constexpr UInt32 MaximumRow = MaximumDimension * 8U;
static_assert(MaximumRow == WindowBytes);
struct Bytes
{
    const UInt8* Data{};
    Size Count{};
};
struct Buffer
{
    UInt8* Data{};
    Size Count{};
};
enum class Status : UInt8
{
    Ok,
    End,
    InvalidBuffer,
    Truncated,
    Limit,
    Signature,
    Header,
    Unsupported,
    Order,
    Crc,
    Adler,
    Huffman,
    Distance,
    Palette,
    Filter,
    SizeMismatch,
    Mask,
    Hotspot
};
struct Image
{
    const UInt8* Pixels{};
    Size Bytes{};
    UInt32 Width{};
    UInt32 Height{};
    [[nodiscard]] bool Valid() const noexcept;
};
struct Huffman
{
    UInt16 Counts[16]{};
    UInt16 Symbols[288]{};
};
// Large caller-owned object: static/arena storage on targets, never stack.
class InflateReader final
{
public:
    [[nodiscard]] Status Begin(const Bytes* segments, UInt32 count, bool zlib = true) noexcept;
    [[nodiscard]] Status Next(UInt8& value) noexcept;
    [[nodiscard]] Size Produced() const noexcept
    {
        return m_produced;
    }

private:
    [[nodiscard]] bool Byte(UInt32& value) noexcept;
    [[nodiscard]] bool Bits(UInt32 count, UInt32& value) noexcept;
    [[nodiscard]] Status Tree(Huffman& tree, const UInt8* lengths, UInt32 count, bool single, bool empty) noexcept;
    [[nodiscard]] Status Symbol(const Huffman& tree, UInt32& value) noexcept;
    [[nodiscard]] Status Block() noexcept;
    [[nodiscard]] Status Dynamic() noexcept;
    [[nodiscard]] Status Finish() noexcept;
    [[nodiscard]] Status Emit(UInt8 value, UInt8& output) noexcept;
    const Bytes* m_segments{};
    UInt32 m_count{};
    UInt32 m_segment{};
    UInt32 m_bits{};
    UInt32 m_bitCount{};
    UInt32 m_blocks{};
    UInt32 m_windowLimit{};
    Size m_position{};
    Size m_produced{};
    UInt32 m_s1{};
    UInt32 m_s2{};
    UInt32 m_remaining{};
    UInt32 m_distance{};
    bool m_zlib{};
    bool m_final{};
    bool m_block{};
    bool m_stored{};
    bool m_ended{};
    Status m_status{Status::Header};
    UInt8 m_window[WindowBytes]{};
    UInt8 m_lengths[320]{};
    UInt8 m_codeLengths[19]{};
    Huffman m_literal{};
    Huffman m_distances{};
    Huffman m_codes{};
};
struct Workspace
{
    InflateReader Inflate{};
    Bytes Segments[MaximumChunks]{};
    UInt8 Rows[2][MaximumRow]{};
    UInt8 Palette[256][4]{};
};
[[nodiscard]] UInt32 Crc32(Bytes bytes) noexcept;
[[nodiscard]] Status Inflate(const Bytes* segments, UInt32 count, Buffer output, InflateReader& reader, Size& written,
                             bool zlib = true) noexcept;
[[nodiscard]] Status DecodePng(Bytes input, Buffer output, Workspace& work, Image& image) noexcept;
[[nodiscard]] Status DecodeBmp(Bytes input, Buffer output, Image& image) noexcept;
[[nodiscard]] Status DecodeDib(Bytes input, Buffer output, Image& image) noexcept;
[[nodiscard]] Status DecodeIco(Bytes input, UInt32 requestedWidth, UInt32 requestedHeight, Buffer output,
                               Workspace& work, Image& image) noexcept;
} // Namespace Asteria::User::Imaging
#endif
