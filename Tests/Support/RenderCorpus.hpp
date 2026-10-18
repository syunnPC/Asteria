// SPDX-License-Identifier: Apache-2.0
#ifndef ASTERIA_TESTS_SUPPORT_RENDER_CORPUS_HPP_INCLUDED
#define ASTERIA_TESTS_SUPPORT_RENDER_CORPUS_HPP_INCLUDED

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Kernel/Security/Sha256.hpp"
#include "User/Graphics/PixelFormat.hpp"
#include "User/Graphics/Surface.hpp"

// Shared support for the rendering corpora of the desktop session: the C++ host renderers and the
// runner's Python model (Tools/Vm/AsteriaVm/RenderCorpus.py) read the same corpus text, render each
// case to a binary PPM, and check its SHA-256 against one committed digest file, so the two produce
// byte-identical images. Corpus text: one statement per line; blank lines and lines whose first
// non-blank character is '#' are ignored. Tokens are separated by blanks; a token that starts with
// '"' is a quoted string ending at the next unescaped '"', with the escapes \\ \" \n \r \t \e
// (0x1B) and \xHH. A case starts with `case <kind> <name>...` and ends with `end`. Digest files:
// one `<name> <64 lowercase hex digits>` per line, '#' comments. ASTERIA_RENDER_OUTPUT_DIR, when
// set, receives `<name>.ppm` for every case; ASTERIA_RENDER_DIGESTS_OUT, when set, receives the
// digests the renderer computed, in the digest file's format, to regenerate it.
namespace Asteria::TestSupport
{
inline constexpr std::size_t RenderMaximumTokens{48U};
inline constexpr std::size_t RenderMaximumDigests{128U};
inline constexpr std::size_t RenderNameBytes{64U};

/// Reads a whole file into buffer (capacity bytes, one kept for a final zero).
[[nodiscard]] inline bool ReadRenderFile(const char* const path, char* const buffer, const std::size_t capacity,
                                         std::size_t& size) noexcept
{
    size = 0U;
    std::FILE* const file = std::fopen(path, "rb");
    if (file == nullptr)
    {
        return false;
    }
    size = std::fread(buffer, 1U, capacity - 1U, file);
    const bool complete = std::feof(file) != 0;
    static_cast<void>(std::fclose(file));
    buffer[size] = '\0';
    return complete;
}

[[nodiscard]] inline bool WriteRenderFile(const char* const path, const std::uint8_t* const data,
                                          const std::size_t size) noexcept
{
    std::FILE* const file = std::fopen(path, "wb");
    if (file == nullptr)
    {
        return false;
    }
    const bool written = std::fwrite(data, 1U, size, file) == size;
    return (std::fclose(file) == 0) && written;
}

/// Reads a corpus line by line and splits it into tokens.
class CorpusReader final
{
public:
    CorpusReader(const char* const text, const std::size_t size) noexcept : m_text{text}, m_size{size}
    {
    }

    /// Moves to the next statement; false at the end of the text or on a
    /// malformed line (an unterminated string or too many tokens).
    [[nodiscard]] bool NextLine() noexcept
    {
        while (m_next < m_size)
        {
            const std::size_t start = m_next;
            std::size_t end = start;
            while ((end < m_size) && (m_text[end] != '\n'))
            {
                ++end;
            }
            m_next = end + 1U;
            ++m_line;
            if (!Split(start, end))
            {
                m_malformed = true;
                return false;
            }
            if ((m_count != 0U) && (m_text[m_starts[0U]] != '#'))
            {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool Malformed() const noexcept
    {
        return m_malformed;
    }

    [[nodiscard]] int Line() const noexcept
    {
        return m_line;
    }

    [[nodiscard]] std::size_t Count() const noexcept
    {
        return m_count;
    }

    /// Token index equals word (an unquoted token).
    [[nodiscard]] bool Is(const std::size_t index, const char* const word) const noexcept
    {
        if (index >= m_count)
        {
            return false;
        }
        const std::size_t length = std::strlen(word);
        return (m_lengths[index] == length) && (std::memcmp(m_text + m_starts[index], word, length) == 0);
    }

    /// An unquoted token as a zero-terminated copy (at most capacity - 1 bytes).
    [[nodiscard]] bool Word(const std::size_t index, char* const out, const std::size_t capacity) const noexcept
    {
        if ((index >= m_count) || (m_lengths[index] >= capacity) || (m_text[m_starts[index]] == '"'))
        {
            return false;
        }
        std::memcpy(out, m_text + m_starts[index], m_lengths[index]);
        out[m_lengths[index]] = '\0';
        return true;
    }

    /// An unquoted decimal or 0x-prefixed hexadecimal token.
    [[nodiscard]] bool Number(const std::size_t index, std::uint64_t& value) const noexcept
    {
        char word[32U]{};
        if (!Word(index, word, sizeof(word)))
        {
            return false;
        }
        char* end{nullptr};
        const unsigned long long parsed = std::strtoull(word, &end, 0);
        if ((end == word) || (*end != '\0') || (word[0U] == '-'))
        {
            return false;
        }
        value = parsed;
        return true;
    }

    /// A signed decimal token.
    [[nodiscard]] bool Signed(const std::size_t index, std::int64_t& value) const noexcept
    {
        char word[32U]{};
        if (!Word(index, word, sizeof(word)))
        {
            return false;
        }
        char* end{nullptr};
        const long long parsed = std::strtoll(word, &end, 10);
        if ((end == word) || (*end != '\0'))
        {
            return false;
        }
        value = parsed;
        return true;
    }

    /// A quoted token's decoded bytes (at most capacity).
    [[nodiscard]] bool Text(const std::size_t index, std::uint8_t* const out, const std::size_t capacity,
                            std::size_t& length) const noexcept
    {
        length = 0U;
        if ((index >= m_count) || (m_text[m_starts[index]] != '"'))
        {
            return false;
        }
        const char* const text = m_text + m_starts[index];
        const std::size_t end = m_lengths[index] - 1U;
        for (std::size_t at{1U}; at < end; ++at)
        {
            if (length == capacity)
            {
                return false;
            }
            if (text[at] != '\\')
            {
                out[length++] = static_cast<std::uint8_t>(text[at]);
                continue;
            }
            ++at;
            switch (text[at])
            {
            case '\\':
                out[length++] = '\\';
                break;
            case '"':
                out[length++] = '"';
                break;
            case 'n':
                out[length++] = '\n';
                break;
            case 'r':
                out[length++] = '\r';
                break;
            case 't':
                out[length++] = '\t';
                break;
            case 'e':
                out[length++] = 0x1BU;
                break;
            case 'x':
            {
                if (at + 2U >= end)
                {
                    return false;
                }
                const int high = HexDigit(text[at + 1U]);
                const int low = HexDigit(text[at + 2U]);
                if ((high < 0) || (low < 0))
                {
                    return false;
                }
                out[length++] = static_cast<std::uint8_t>((high << 4) | low);
                at += 2U;
                break;
            }
            default:
                return false;
            }
        }
        return true;
    }

private:
    [[nodiscard]] static int HexDigit(const char digit) noexcept
    {
        if ((digit >= '0') && (digit <= '9'))
        {
            return digit - '0';
        }
        if ((digit >= 'a') && (digit <= 'f'))
        {
            return digit - 'a' + 10;
        }
        if ((digit >= 'A') && (digit <= 'F'))
        {
            return digit - 'A' + 10;
        }
        return -1;
    }

    [[nodiscard]] bool Split(const std::size_t start, const std::size_t end) noexcept
    {
        m_count = 0U;
        std::size_t at = start;
        while (at < end)
        {
            if ((m_text[at] == ' ') || (m_text[at] == '\t') || (m_text[at] == '\r'))
            {
                ++at;
                continue;
            }
            if (m_count == RenderMaximumTokens)
            {
                return false;
            }
            const std::size_t token = at;
            if (m_text[at] == '"')
            {
                ++at;
                while ((at < end) && (m_text[at] != '"'))
                {
                    at += (m_text[at] == '\\') ? 2U : 1U;
                }
                if (at >= end)
                {
                    return false;
                }
                ++at;
            }
            else
            {
                while ((at < end) && (m_text[at] != ' ') && (m_text[at] != '\t') && (m_text[at] != '\r'))
                {
                    ++at;
                }
            }
            m_starts[m_count] = token;
            m_lengths[m_count] = at - token;
            ++m_count;
        }
        return true;
    }

    const char* m_text;
    std::size_t m_size;
    std::size_t m_next{0U};
    int m_line{0};
    bool m_malformed{false};
    std::size_t m_count{0U};
    std::size_t m_starts[RenderMaximumTokens]{};
    std::size_t m_lengths[RenderMaximumTokens]{};
};

/// The RGB triple a pixel word of format stores (the inverse of PackColor).
inline void UnpackRenderPixel(const User::Graphics::PixelFormat format, const std::uint32_t word,
                              std::uint8_t* const rgb) noexcept
{
    const std::uint8_t low = static_cast<std::uint8_t>(word);
    const std::uint8_t middle = static_cast<std::uint8_t>(word >> 8U);
    const std::uint8_t high = static_cast<std::uint8_t>(word >> 16U);
    rgb[0U] = (format == User::Graphics::PixelFormat::Rgbx) ? low : high;
    rgb[1U] = middle;
    rgb[2U] = (format == User::Graphics::PixelFormat::Rgbx) ? high : low;
}

/// Encodes a memory surface as a binary PPM ("P6\n<w> <h>\n255\n" then RGB
/// rows) into out; returns the size, or zero when out is too small.
[[nodiscard]] inline std::size_t EncodeRenderPpm(const User::Graphics::MemorySurface& surface, std::uint8_t* const out,
                                                 const std::size_t capacity) noexcept
{
    char header[48U]{};
    const int headerLength = std::snprintf(header, sizeof(header), "P6\n%u %u\n255\n", surface.Width, surface.Height);
    if (headerLength <= 0)
    {
        return 0U;
    }
    const std::size_t pixels = static_cast<std::size_t>(surface.Width) * surface.Height;
    const std::size_t total = static_cast<std::size_t>(headerLength) + (pixels * 3U);
    if (total > capacity)
    {
        return 0U;
    }
    std::memcpy(out, header, static_cast<std::size_t>(headerLength));
    std::uint8_t* rgb = out + headerLength;
    for (std::uint32_t y{0U}; y < surface.Height; ++y)
    {
        const std::uint32_t* const row = surface.Pixels + (static_cast<std::size_t>(y) * surface.Stride);
        for (std::uint32_t x{0U}; x < surface.Width; ++x)
        {
            UnpackRenderPixel(surface.Format, row[x], rgb);
            rgb += 3U;
        }
    }
    return total;
}

/// The lowercase hexadecimal SHA-256 of data.
inline void RenderSha256(const std::uint8_t* const data, const std::size_t size, char (&hex)[65U]) noexcept
{
    Kernel::Security::Sha256 hash{};
    hash.Update(data, size);
    std::uint8_t digest[Kernel::Security::Sha256DigestBytes]{};
    hash.Finish(digest);
    static constexpr char Digits[]{"0123456789abcdef"};
    for (std::size_t index{0U}; index < Kernel::Security::Sha256DigestBytes; ++index)
    {
        hex[index * 2U] = Digits[digest[index] >> 4U];
        hex[(index * 2U) + 1U] = Digits[digest[index] & 15U];
    }
    hex[64U] = '\0';
}

/// The committed digests of a corpus, and the digests a run computed.
class RenderDigests final
{
public:
    /// Loads a digest file; false when it cannot be read or is malformed.
    [[nodiscard]] bool Load(const char* const path) noexcept
    {
        static char text[RenderMaximumDigests * 160U];
        std::size_t size{0U};
        if (!ReadRenderFile(path, text, sizeof(text), size))
        {
            return false;
        }
        CorpusReader reader{text, size};
        while (reader.NextLine())
        {
            if ((reader.Count() != 2U) || (m_count == RenderMaximumDigests) ||
                !reader.Word(0U, m_names[m_count], RenderNameBytes) ||
                !reader.Word(1U, m_digests[m_count], sizeof(m_digests[m_count])) ||
                (std::strlen(m_digests[m_count]) != 64U))
            {
                return false;
            }
            ++m_count;
        }
        return !reader.Malformed();
    }

    /// The committed digest of a case, or null.
    [[nodiscard]] const char* Find(const char* const name) const noexcept
    {
        for (std::size_t index{0U}; index < m_count; ++index)
        {
            if (std::strcmp(m_names[index], name) == 0)
            {
                return m_digests[index];
            }
        }
        return nullptr;
    }

    [[nodiscard]] std::size_t Count() const noexcept
    {
        return m_count;
    }

    /// Records a computed digest for ASTERIA_RENDER_DIGESTS_OUT.
    void Record(const char* const name, const char (&hex)[65U]) noexcept
    {
        const std::size_t used = std::strlen(m_out);
        static_cast<void>(std::snprintf(m_out + used, sizeof(m_out) - used, "%s %s\n", name, hex));
    }

    /// Writes the recorded digests where ASTERIA_RENDER_DIGESTS_OUT names,
    /// after header (a comment block); true when the variable is unset.
    [[nodiscard]] bool WriteRecorded(const char* const header) const noexcept
    {
        const char* const path = std::getenv("ASTERIA_RENDER_DIGESTS_OUT");
        if (path == nullptr)
        {
            return true;
        }
        std::FILE* const file = std::fopen(path, "wb");
        if (file == nullptr)
        {
            return false;
        }
        const bool written = (std::fputs(header, file) >= 0) && (std::fputs(m_out, file) >= 0);
        return (std::fclose(file) == 0) && written;
    }

private:
    char m_names[RenderMaximumDigests][RenderNameBytes]{};
    char m_digests[RenderMaximumDigests][72U]{};
    std::size_t m_count{0U};
    char m_out[RenderMaximumDigests * 160U]{};
};

/// Writes `<directory>/<name>.ppm` when ASTERIA_RENDER_OUTPUT_DIR is set;
/// true when it is unset or the write succeeded.
[[nodiscard]] inline bool WriteRenderOutput(const char* const name, const std::uint8_t* const ppm,
                                            const std::size_t size) noexcept
{
    const char* const directory = std::getenv("ASTERIA_RENDER_OUTPUT_DIR");
    if (directory == nullptr)
    {
        return true;
    }
    char path[512U]{};
    const int length = std::snprintf(path, sizeof(path), "%s/%s.ppm", directory, name);
    if ((length <= 0) || (static_cast<std::size_t>(length) >= sizeof(path)))
    {
        return false;
    }
    return WriteRenderFile(path, ppm, size);
}
} // Namespace Asteria::TestSupport

#endif // ASTERIA_TESTS_SUPPORT_RENDER_CORPUS_HPP_INCLUDED
