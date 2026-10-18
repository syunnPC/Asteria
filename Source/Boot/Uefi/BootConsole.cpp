// SPDX-License-Identifier: Apache-2.0

#include "Boot/Uefi/BootConsole.hpp"

#include "Boot/Uefi/GraphicsModeChoice.hpp"
#include "User/Graphics/BitmapFont.hpp"

namespace Asteria::Boot::Uefi
{
namespace
{
constexpr Foundation::UInt32 MaximumColumns{160U};
constexpr Foundation::UInt32 GlyphWidth{8U};
constexpr Foundation::UInt32 GlyphHeight{16U};
constinit GraphicsOutputProtocol* g_graphics{nullptr};
constinit GraphicsOutputBltPixel g_line[MaximumColumns * GlyphWidth * GlyphHeight]{};
constinit Foundation::UInt8 g_cells[MaximumColumns]{};
constinit Foundation::UInt32 g_columns{0U};
constinit Foundation::UInt32 g_rows{0U};
constinit Foundation::UInt32 g_column{0U};
constinit Foundation::UInt32 g_row{0U};
constinit Foundation::UInt16 g_firmwareChunk[96U]{};

void ClearLine() noexcept
{
    for (Foundation::UInt32 column{0U}; column < MaximumColumns; ++column)
    {
        g_cells[column] = ' ';
    }
    g_column = 0U;
}

bool NextLine() noexcept
{
    const auto& font = User::Graphics::Bold8x16;
    constexpr GraphicsOutputBltPixel foreground{255U, 255U, 255U, 0U};
    constexpr GraphicsOutputBltPixel background{0x40U, 0x20U, 0x10U, 0U};
    const Foundation::UInt32 width{g_columns * GlyphWidth};
    for (Foundation::UInt32 column{0U}; column < g_columns; ++column)
    {
        const Foundation::UInt32 code{g_cells[column]};
        const bool present{code >= font.FirstCode && code - font.FirstCode < font.Count};
        for (Foundation::UInt32 y{0U}; y < GlyphHeight; ++y)
        {
            const Foundation::UInt32 bits{present ? font.Data[(code - font.FirstCode) * GlyphHeight + y] : 0U};
            for (Foundation::UInt32 x{0U}; x < GlyphWidth; ++x)
            {
                g_line[y * width + column * GlyphWidth + x] = (bits & (128U >> x)) != 0U ? foreground : background;
            }
        }
    }
    // UEFI BltBufferToVideo: packed BGR-reserved RAM pixels, one changed row.
    // Firmware owns the device mapping/cache attribute on both ISAs. No
    // VideoToBuffer operation or framebuffer load occurs in this console.
    const Status status{g_graphics->Blt(g_graphics, g_line, GraphicsOutputBltOperation::BufferToVideo, 0U, 0U, 0U,
                                        g_row * GlyphHeight, width, GlyphHeight,
                                        width * sizeof(GraphicsOutputBltPixel))};
    g_row = g_row + 1U < g_rows ? g_row + 1U : 0U;
    ClearLine();
    return status == Success;
}

bool Put(const Foundation::UInt8 byte) noexcept
{
    if (byte == '\n')
    {
        return NextLine();
    }
    if (byte == '\r')
    {
        g_column = 0U;
        return true;
    }
    if (byte == '\b')
    {
        if (g_column != 0U)
        {
            --g_column;
        }
        return true;
    }
    if (byte == '\t')
    {
        const Foundation::UInt32 next{(g_column + 8U) & ~7U};
        g_column = next < g_columns ? next : g_columns;
        return true;
    }
    if (g_column == g_columns && !NextLine())
    {
        return false;
    }
    g_cells[g_column++] = byte;
    return true;
}
}

void ConfigureBootConsole(GraphicsOutputProtocol* const graphics) noexcept
{
    g_graphics = nullptr;
    g_row = 0U;
    ClearLine();
    if (graphics == nullptr || graphics->Blt == nullptr || !IsScanoutModeUsable(graphics->Mode))
    {
        return;
    }
    const auto& information = *graphics->Mode->Information;
    const Foundation::UInt32 columns{information.HorizontalResolution / GlyphWidth};
    g_columns = columns < MaximumColumns ? columns : MaximumColumns;
    g_rows = information.VerticalResolution / GlyphHeight;
    if (g_columns != 0U && g_rows != 0U)
    {
        g_graphics = graphics;
    }
}

bool IsBootConsoleConfigured() noexcept
{
    return g_graphics != nullptr;
}

bool WriteBootConsoleUnicode(SimpleTextOutputProtocol& console, Foundation::UInt16* const text) noexcept
{
    if (g_graphics != nullptr)
    {
        for (Foundation::Size index{0U}; text[index] != 0U; ++index)
        {
            const Foundation::UInt8 byte{text[index] <= 255U ? static_cast<Foundation::UInt8>(text[index])
                                                             : Foundation::UInt8{'?'}};
            if (!Put(byte))
            {
                g_graphics = nullptr;
                return console.OutputString != nullptr && console.OutputString(&console, text) == Success;
            }
        }
        return true;
    }
    return console.OutputString != nullptr && console.OutputString(&console, text) == Success;
}

bool WriteBootConsoleAscii(SimpleTextOutputProtocol& console, const char* text) noexcept
{
    while (*text != '\0')
    {
        Foundation::Size count{0U};
        while (*text != '\0' && count < 95U)
        {
            g_firmwareChunk[count++] = static_cast<Foundation::UInt8>(*text++);
        }
        g_firmwareChunk[count] = 0U;
        if (!WriteBootConsoleUnicode(console, g_firmwareChunk))
        {
            return false;
        }
    }
    return true;
}
} // namespace Asteria::Boot::Uefi
