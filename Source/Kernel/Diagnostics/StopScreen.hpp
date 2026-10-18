// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_DIAGNOSTICS_STOP_SCREEN_HPP_INCLUDED
#define ASTERIA_KERNEL_DIAGNOSTICS_STOP_SCREEN_HPP_INCLUDED

#include "Kernel/Diagnostics/StopFont.hpp"
#include "Kernel/Diagnostics/StopTextWriter.hpp"

namespace Asteria::Kernel::Diagnostics
{
/// Terminal scanout writer: caller owns the stop latch, completed the freeze,
/// and mapped pitch * height * 4 bytes uncached, with pitch * height below
/// 2^32 pixels. No allocation or locks.
class StopScreen final : public StopTextWriter<StopScreen>
{
public:
    constexpr StopScreen(volatile Foundation::UInt32* pixels, const Foundation::UInt32 width,
                         const Foundation::UInt32 height, const Foundation::UInt32 pitch, const bool rgbx) noexcept
        : m_pixels{pixels}, m_width{width}, m_height{height}, m_pitch{pitch},
          m_background{rgbx ? 0x00A68C00U : 0x00008CA6U}
    {
    }
    void Clear() noexcept
    {
        for (Foundation::UInt32 y{0U}; y < m_height; ++y)
        {
            for (Foundation::UInt32 x{0U}; x < m_width; ++x)
            {
                m_pixels[y * m_pitch + x] = m_background;
            }
        }
    }
    void Put(const Foundation::UInt8 code) noexcept
    {
        if (code == '\n')
        {
            m_column = 0U;
            ++m_row;
        }
        else
        {
            Glyph(code);
        }
    }

private:
    void Glyph(const Foundation::UInt8 code) noexcept
    {
        if ((m_column < m_width / 8U) && (m_row < m_height / 16U) && code >= 32U && code < 127U)
        {
            for (Foundation::UInt32 y{0U}; y < 16U; ++y)
            {
                const Foundation::UInt8 bits = StopGlyphs[(code - 32U) * 16U + y];
                for (Foundation::UInt32 x{0U}; x < 8U; ++x)
                {
                    m_pixels[(m_row * 16U + y) * m_pitch + m_column * 8U + x] =
                        (bits & (0x80U >> x)) != 0U ? 0x00FFFFFFU : m_background;
                }
            }
        }
        ++m_column;
    }
    volatile Foundation::UInt32* m_pixels;
    Foundation::UInt32 m_width;
    Foundation::UInt32 m_height;
    Foundation::UInt32 m_pitch;
    Foundation::UInt32 m_background;
    Foundation::UInt32 m_column{0U};
    Foundation::UInt32 m_row{0U};
};
} // namespace Asteria::Kernel::Diagnostics

#endif // ASTERIA_KERNEL_DIAGNOSTICS_STOP_SCREEN_HPP_INCLUDED
