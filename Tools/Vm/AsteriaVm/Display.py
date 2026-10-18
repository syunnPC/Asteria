# SPDX-License-Identifier: Apache-2.0
"""The System Display Broker's console and cursor model.

After a run the runner compares a QEMU screendump (binary PPM) pixel for pixel
with `TextConsole` fed the root service's lines. TextConsole is a port of
Source/User/Console/TextConsole.hpp; its font and colors are parsed from the
repository's headers so the model cannot drift from them.
"""
from __future__ import annotations

import pathlib
import re

START_PREFIX = '# display: '
STOP_LINE = '# display: stopped'
START_PATTERN = re.compile(r'^# display: (\d+)x(\d+) (rgbx|bgrx), console (\d+)x(\d+)$')

FONT_HEADER = pathlib.Path('Source/User/Graphics/Bold8x16Font.hpp')
CONSOLE_HEADER = pathlib.Path('Source/User/Console/TextConsole.hpp')
CURSOR_HEADER = pathlib.Path('Source/User/Graphics/Composition/CursorImages.hpp')
POINTER_PATTERN = re.compile(r'^# pointer: (\d+),(\d+) sequence (\d+)$')
# User/Console/TextConsole.hpp's grid limits by ISA: columns, rows and cells.
# IA-64 restores width with a grant-bounded cell budget.
CONSOLE_LIMITS = {'Amd64': (640, 256, 65536), 'Ia64': (640, 256, 12288)}
TAB_WIDTH = 8
BLANK = 0x20
WHITESPACE = b' \t\r\n\v\f'
Color = tuple[int, int, int]


class DisplayError(Exception):
    """The repository's headers or a screendump could not be read."""


def _read(repository: pathlib.Path, relative: pathlib.Path) -> str:
    path = pathlib.Path(repository) / relative
    try:
        return path.read_text()
    except OSError as error:
        raise DisplayError('cannot read %s: %s' % (path, error)) from error


def _constant(text: str, name: str) -> int:
    match = re.search(r'\b%s\s*\{\s*(0[xX][0-9a-fA-F]+|\d+)[Uu]?\s*\}' % re.escape(name), text)
    if match is None:
        raise DisplayError('no constant %s in the font header' % name)
    return int(match.group(1), 0)


def load_font(repository: pathlib.Path) -> list[bytes]:
    """The glyph rows of Bold8x16Font.hpp: 256 entries indexed by code, each 16
    bytes (one per row, MSB leftmost); codes outside the font are 16 zero bytes."""
    text = _read(repository, FONT_HEADER)
    first = _constant(text, 'Bold8x16FirstCode')
    count = _constant(text, 'Bold8x16Count')
    width = _constant(text, 'Bold8x16Width')
    height = _constant(text, 'Bold8x16Height')
    if width != 8 or height != 16:
        raise DisplayError('the font is %dx%d, expected 8x16' % (width, height))
    start = text.index('Bold8x16Glyphs')
    body = text[text.index('{', text.index(']', start)) + 1:]
    body = body[:body.index('};')]
    body = re.sub(r'/\*.*?\*/', '', body, flags=re.S)
    body = re.sub(r'//[^\n]*', '', body)
    data = bytes(int(value, 16) for value in re.findall(r'0[xX][0-9a-fA-F]+', body))
    if len(data) != count * height:
        raise DisplayError('%d glyph bytes, expected %d' % (len(data), count * height))
    blank = bytes(height)
    glyphs = [blank] * 256
    for index in range(count):
        glyphs[first + index] = data[index * height:(index + 1) * height]
    return glyphs


def console_colors(repository: pathlib.Path) -> tuple[Color, Color]:
    """(foreground, background) RGB triples from TextConsole.hpp."""
    text = _read(repository, CONSOLE_HEADER)
    found = []
    for name in ('ConsoleForeground', 'ConsoleBackground'):
        match = re.search(r'\b%s\s*\{\s*\.Red\s*=\s*(\w+?)[Uu]?\s*,\s*\.Green\s*=\s*(\w+?)[Uu]?\s*,'
                          r'\s*\.Blue\s*=\s*(\w+?)[Uu]?\s*\}' % name, text)
        if match is None:
            raise DisplayError('no color %s in %s' % (name, CONSOLE_HEADER))
        found.append(tuple(int(group, 0) for group in match.groups()))
    return found[0], found[1]


def load_cursor(repository: pathlib.Path) -> tuple[int, int, tuple[int, int], list[int]]:
    """Derive the owner's outlined arrow from the source mask and ARGB colors."""
    text = _read(repository, CURSOR_HEADER)
    match = re.search(r'ArrowMask\[16U\]\s*\{(.*?)\};', text, re.S)
    if match is None:
        raise DisplayError('no ArrowMask in cursor header')
    mask = [int(value, 2) for value in re.findall(r'0b([01]{8})', match.group(1))]
    if len(mask) != 16:
        raise DisplayError('arrow mask must have 16 rows')
    width, height = _constant(text, 'ArrowWidth'), _constant(text, 'ArrowHeight')
    white, outline = _constant(text, 'CursorWhite'), _constant(text, 'CursorOutline')
    hot = re.search(r'CursorImage Arrow\{.*?\.HotSpot\s*=\s*\{\.X\s*=\s*(\d+),\s*\.Y\s*=\s*(\d+)\}', text, re.S)
    if (width, height) != (10, 18) or hot is None:
        raise DisplayError('invalid arrow geometry or hot spot')

    def bit(x, y):
        return 0 <= x < 8 and 0 <= y < 16 and bool(mask[y] & (0x80 >> x))
    pixels = [white if bit(x - 1, y - 1) else outline if any(
        bit(x - 1 + dx, y - 1 + dy) for dx in (-1, 0, 1) for dy in (-1, 0, 1)) else 0
        for y in range(height) for x in range(width)]
    return width, height, (int(hot[1]), int(hot[2])), pixels


def pointer_position(lines: list[str], width: int, height: int) -> tuple[int, int]:
    position = width // 2, height // 2
    for line in lines:
        match = POINTER_PATTERN.fullmatch(line)
        if match:
            position = int(match[1]), int(match[2])
    return position


def draw_cursor(rgb: bytes, width: int, height: int, position: tuple[int, int], repository: pathlib.Path) -> bytes:
    image_width, image_height, hot, pixels = load_cursor(repository)
    result = bytearray(rgb)
    for y in range(image_height):
        for x in range(image_width):
            screen_x, screen_y = position[0] + x - hot[0], position[1] + y - hot[1]
            if not (0 <= screen_x < width and 0 <= screen_y < height):
                continue
            source = pixels[y * image_width + x]
            alpha = source >> 24
            at = (screen_y * width + screen_x) * 3
            for channel, shift in enumerate((16, 8, 0)):
                result[at + channel] = (((source >> shift) & 255) * alpha
                                        + result[at + channel] * (255 - alpha) + 127) // 255
    return bytes(result)


class TextConsole:
    """A cell grid and cursor; a port of the C++ TextConsole's Write and cells."""

    def __init__(self, width: int, height: int, isa: str = 'Amd64'):
        maximum_columns, maximum_rows, maximum_cells = CONSOLE_LIMITS[isa]
        self.columns = min(width // 8, maximum_columns)
        self.rows = min(height // 16, maximum_rows, maximum_cells // self.columns if self.columns else 0)
        if self.columns == 0 or self.rows == 0:
            self.columns = self.rows = 0
        self.cells = [[BLANK] * self.columns for _ in range(self.rows)]
        self.column = 0
        self.row = 0

    def _newline(self) -> None:
        self.column = 0
        if self.row + 1 < self.rows:
            self.row += 1
        else:
            del self.cells[0]
            self.cells.append([BLANK] * self.columns)

    def write(self, data: bytes) -> None:
        if self.columns == 0:
            return
        for code in data:
            if code == 0x0A:
                self._newline()
            elif code == 0x0D:
                self.column = 0
            elif code == 0x09:
                self.column = min((self.column // TAB_WIDTH + 1) * TAB_WIDTH, self.columns)
            elif code == 0x08:
                if self.column != 0:
                    self.column -= 1
            else:
                if self.column >= self.columns:
                    self._newline()
                self.cells[self.row][self.column] = code
                self.column += 1

    def cell(self, row: int, column: int) -> int:
        """The code of a cell; blank outside the grid."""
        if 0 <= row < self.rows and 0 <= column < self.columns:
            return self.cells[row][column]
        return BLANK

    def render(self, width: int, height: int, font: list[bytes], colors: tuple[Color, Color]) -> bytes:
        """The whole screen as RGB24: background everywhere, the grid drawn at (0, 0)."""
        foreground, background = bytes(colors[0]), bytes(colors[1])
        strips: dict[tuple[int, int], bytes] = {}  # Row bytes of (code, glyph row), built on first use

        def strip(code: int, line: int) -> bytes:
            key = (code, line)
            built = strips.get(key)
            if built is None:
                bits = font[code][line]
                built = strips[key] = b''.join(
                    foreground if (bits >> (7 - bit)) & 1 else background for bit in range(8))
            return built

        margin = background * (width - self.columns * 8)
        lines: list[bytes] = []
        for row in range(self.rows):
            cells = self.cells[row]
            for line in range(16):
                lines.append(b''.join([strip(code, line) for code in cells]) + margin)
        lines.extend([background * width] * max(0, height - len(lines)))
        return b''.join(lines[:height])


def _ppm_payload(data: bytes) -> tuple[int, int, bytes]:
    """Decode the header; callers decide which row storage is permitted."""
    if data[:2] != b'P6':
        raise DisplayError('not a binary PPM (P6)')
    position = 2
    tokens: list[bytes] = []
    while len(tokens) < 3:
        while position < len(data) and data[position] in WHITESPACE:
            position += 1
        if position >= len(data):
            raise DisplayError('truncated PPM header')
        if data[position] == 0x23:  # A comment runs to the end of the line
            while position < len(data) and data[position] not in b'\r\n':
                position += 1
            continue
        end = position
        while end < len(data) and data[end] not in WHITESPACE and data[end] != 0x23:
            end += 1
        tokens.append(data[position:end])
        position = end
    try:
        width, height, maxval = (int(token) for token in tokens)
    except ValueError as error:
        raise DisplayError('malformed PPM header %r' % tokens) from error
    if maxval != 255:
        raise DisplayError('PPM maxval %d, expected 255' % maxval)
    position += 1  # Exactly one whitespace byte follows maxval
    if width <= 0 or height <= 0:
        raise DisplayError('PPM dimensions must be positive')
    return width, height, data[position:]


def parse_ppm(data: bytes) -> tuple[int, int, bytes]:
    """(width, height, packed RGB bytes) of a binary P6 image with maxval 255."""
    width, height, pixels = _ppm_payload(data)
    if len(pixels) != width * height * 3:
        raise DisplayError('PPM has %d pixel bytes, expected %d' % (len(pixels), width * height * 3))
    return width, height, pixels


def write_ppm(width: int, height: int, rgb: bytes) -> bytes:
    return b'P6\n%d %d\n255\n' % (width, height) + rgb


def parse_start(line: str) -> tuple[int, int, str, int, int] | None:
    """(width, height, format, columns, rows) of a start line, or None."""
    match = START_PATTERN.match(line)
    if match is None:
        return None
    return int(match.group(1)), int(match.group(2)), match.group(3), int(match.group(4)), int(match.group(5))


def console_lines(lines: list[str]) -> list[str] | None:
    """The lines written to the screen: from the start line (inclusive) up to
    the stop line (exclusive). None when the channel has no start line."""
    for start, line in enumerate(lines):
        if line.startswith(START_PREFIX) and line != STOP_LINE:
            end = lines.index(STOP_LINE, start + 1) if STOP_LINE in lines[start + 1:] else len(lines)
            return lines[start:end]
    return None


def _describe_cell(actual: bytes, font: list[bytes], background: bytes, foreground: bytes) -> str:
    """What a screendump cell (16 rows of 8 RGB pixels) shows."""
    rows = bytearray()
    for line in range(16):
        bits = 0
        for pixel in range(8):
            offset = line * 24 + pixel * 3
            value = actual[offset:offset + 3]
            if value == foreground:
                bits |= 0x80 >> pixel
            elif value != background:
                return 'colors that are neither foreground nor background'
        rows.append(bits)
    for code in range(256):
        if font[code] == bytes(rows):
            return 'the glyph of 0x%02X' % code if code else 'a blank cell'
    return 'a pattern matching no glyph'


def _printable(code: int) -> str:
    return "0x%02X '%c'" % (code, code) if 0x20 <= code < 0x7F else '0x%02X' % code


def display_problems(ppm: bytes, lines: list[str], repository: pathlib.Path, limit: int = 5,
                     isa: str = 'Amd64') -> list[str]:
    """Why the screendump differs from the console fed the root service's lines
    (empty when every pixel is identical)."""
    shown = console_lines(lines)
    if shown is None:
        return ['no %r start line in the root service output' % START_PREFIX]
    start = parse_start(shown[0])
    if start is None:
        return ['malformed display start line %r' % shown[0]]
    width, height, _format, columns, rows = start
    try:
        font = load_font(repository)
        foreground, background = console_colors(repository)
        screen_width, screen_height, actual = parse_ppm(ppm)
    except DisplayError as error:
        return [str(error)]
    if (screen_width, screen_height) != (width, height):
        return ['screendump is %dx%d, the mode is %dx%d' % (screen_width, screen_height, width, height)]
    console = TextConsole(width, height, isa)
    if (console.columns, console.rows) != (columns, rows):
        return ['the start line says console %dx%d, expected %dx%d for the mode' % (
            columns, rows, console.columns, console.rows)]
    for line in shown:
        if line == '# session-probe: cleared trusted console':
            console = TextConsole(width, height, isa)
        if not line.startswith('# pointer:'):
            console.write((line + '\n').encode('latin-1', 'replace'))
    expected = console.render(width, height, font, (foreground, background))
    try:
        expected = draw_cursor(expected, width, height, pointer_position(lines, width, height), repository)
    except DisplayError as error:
        return [str(error)]
    if expected == actual:
        return []
    stride = width * 3
    differing = margin = 0
    cells: dict[tuple[int, int], int] = {}
    left, top, right, bottom = width, height, -1, -1
    for y in range(height):
        a, b = actual[y * stride:(y + 1) * stride], expected[y * stride:(y + 1) * stride]
        if a == b:
            continue
        for x in range(width):
            if a[x * 3:x * 3 + 3] != b[x * 3:x * 3 + 3]:
                differing += 1
                left, right, top, bottom = min(left, x), max(right, x), min(top, y), max(bottom, y)
                if x < console.columns * 8 and y < console.rows * 16:
                    key = (y // 16, x // 8)
                    cells[key] = cells.get(key, 0) + 1
                else:
                    margin += 1
    problems = ['%d of %d pixels differ; bounding box x %d..%d, y %d..%d' % (
        differing, width * height, left, right, top, bottom)]
    for row, column in sorted(cells)[:limit]:
        pixels = bytearray()
        for y in range(row * 16, row * 16 + 16):
            pixels += actual[y * stride + column * 24:y * stride + column * 24 + 24]
        problems.append('cell (row %d, column %d) expected %s, %d pixels differ, the screen shows %s' % (
            row, column, _printable(console.cell(row, column)), cells[(row, column)],
            _describe_cell(bytes(pixels), font, bytes(background), bytes(foreground))))
    if len(cells) > limit:
        problems.append('%d more cells differ' % (len(cells) - limit))
    if margin:
        problems.append('%d pixels differ outside the grid' % margin)
    return problems


def stop_stage_name(stop, repository: pathlib.Path, isa: str = "Amd64") -> str:
    from AsteriaVm.Stops import vocabulary
    names = vocabulary(repository)
    if stop.Code not in (names['BootStage'], names['ApplicationProcessorStage']):
        return ''
    text = (repository / ('Source/Platform/%s/Diagnostics/%sStopStage.hpp' % (isa, isa))).read_text()
    table = isa + ('ApplicationProcessorStageNames' if stop.Code == names['ApplicationProcessorStage'] else 'BootStageNames')
    body = text.split(table + '[]{', 1)[1].split('};', 1)[0]
    stages = {int(number): name for number, name in re.findall(r'\{(\d+)U, "(\w+)"\}', body)}
    return ' ' + stages.get(stop.Parameters[0], 'UnknownStage')


def watchdog_component_name(repository: pathlib.Path, kind: int) -> str:
    from AsteriaVerify import Layout
    header_path = pathlib.Path('Source/Abi/Watchdog.hpp')
    header = _read(repository, header_path)
    constants = Layout.constants(header + _read(repository, pathlib.Path('Source/Abi/ComponentBootstrap.hpp')))
    constants['WatchdogUpdatingComponent'] = (1 << 64) - 1
    for case, name in re.findall(r'\bcase\s+(\w+)\s*:\s*return\s+"([^"]+)"\s*;', header):
        value = int(case[:-1]) if case.endswith('U') and case[:-1].isdigit() else constants.get(case)
        if value == kind:
            return name
    fallback = re.search(r'\bdefault\s*:\s*return\s+"([^"]+)"\s*;', header)
    if fallback is None:
        raise DisplayError('%s: WatchdogComponentName default label missing' % header_path)
    return fallback[1]


def kernel_load_delta(kernel_elf: pathlib.Path, lines: list[str]) -> int:
    from AsteriaVerify.Elf import ElfFile
    from AsteriaVm.Observers import Amd64Image
    image, problems = Amd64Image.loaded_image(lines)
    if image is None:
        raise DisplayError('; '.join(problems))
    return image.base - ElfFile(kernel_elf).symbol('AsteriaImageBase').value


def stop_text_bounds(kernel_elf: pathlib.Path, lines: list[str] | None = None) -> tuple[int, int]:
    from AsteriaVerify.Elf import ElfFile
    elf = ElfFile(kernel_elf)
    delta = kernel_load_delta(kernel_elf, lines) if lines is not None else 0
    return elf.symbol('AsteriaKernelTextStart').value + delta, elf.symbol('AsteriaKernelTextEnd').value + delta


def stop_text(stops, cpus: int, repository: pathlib.Path, isa: str = "Amd64", watchdog=None, text_bounds=None) -> str:
    """Screen text derived independently from the evidence and common vocabulary."""
    from AsteriaVm.Stops import vocabulary
    names = {code: name for name, code in vocabulary(repository).items()}
    record = stops.Processors[stops.Winner]
    stop, snapshot = record.Stop, record.Snapshot
    h = lambda value: '0x%016X' % value
    lines = ['Asteria stopped', 'Code 0x%08X %s' % (stop.Code, names[stop.Code]),
             'P1 %s%s  P2 %s' % (h(stop.Parameters[0]), stop_stage_name(stop, repository, isa), h(stop.Parameters[1])),
             'P3 %s  P4 %s' % tuple(h(value) for value in stop.Parameters[2:]),
             'CPU %d' % stops.Winner,
             ]
    if isa == 'Ia64':
        lines += ['Vector %s  ISR %s' % (h(snapshot.VectorOffset), h(snapshot.Isr)),
                  'IIP %s  IPSR %s' % (h(snapshot.Iip), h(snapshot.Ipsr)),
                  'IFA %s  IIM %s' % (h(snapshot.Ifa), h(snapshot.Iim)),
                  'IIPA %s  b0 %s' % (h(snapshot.Iipa), h(snapshot.B0)),
                  'RR %s  RID %s' % (h(snapshot.RegionRegister), h(snapshot.Rid))]
    else:
        lines += ['Vector %s  Error %s' % (h(snapshot.Vector), h(snapshot.Error)),
                  'RIP %s  CS %s' % (h(snapshot.Rip), h(snapshot.Cs)),
                  'RFLAGS %s' % h(snapshot.Rflags), 'RSP %s' % h(snapshot.Rsp),
                  'CR2 %s  CR3 %s' % (h(snapshot.Cr2), h(snapshot.Cr3))]
    for cpu in range(cpus):
        lines.append('CPU %d %s %s' % (cpu, {0: 'running', 1: 'stopping', 2: 'frozen', 3: 'unconfirmed'}.get(stops.DrawStates[cpu], 'unknown'),
                     'not online' if not stops.OnlineMask & (1 << cpu) else ('confirmed' if stops.ConfirmedMask & (1 << cpu) else 'unconfirmed')))
        if watchdog is not None and stops.OnlineMask & (1 << cpu):
            sample = watchdog.Processors[cpu]
            if watchdog.StalledMask & (1 << cpu):
                lines[-1] += ' stalled'
            lines[-1] += ' age %d ms %s' % (sample.AgeMilliseconds, watchdog_component_name(repository, sample.Component))
            location = '  %s %s' % ('captured' if sample.Captured else 'last heartbeat', h(sample.Location))
            if text_bounds and text_bounds[0] <= sample.Location < text_bounds[1]:
                location += ' KERNEL.ELF .text+' + h(sample.Location - text_bounds[0])
            lines.append(location)
    lines += ['Online %s'  % h(stops.OnlineMask), 'Uptime %s ticks %s' % ('ITC' if isa == 'Ia64' else 'TSC', h(stops.UptimeTicks)),
              'DMA not quiesced', 'Build ']
    return '\n'.join(lines)


def stop_display_problems(ppm: bytes, stops, repository: pathlib.Path, cpus: int, isa: str = "Amd64", watchdog=None, text_bounds=None) -> list[str]:
    width, height, actual = parse_ppm(ppm)
    # StopScreen clips each line to the mode's full grid, without console wrapping.
    console = TextConsole(width, height)
    console.columns, console.rows = width // 8, height // 16
    console.cells = [[BLANK] * console.columns for _ in range(console.rows)]
    rendered_lines = stop_text(stops, cpus, repository, isa, watchdog, text_bounds).split('\n')
    for row, line in enumerate(rendered_lines):
        if row < console.rows:
            console.cells[row][:min(len(line), console.columns)] = list(line[:console.columns].encode('ascii'))
    expected = console.render(width, height, load_font(repository), ((255, 255, 255), (0, 140, 166)))
    # Identity changes without changing verified code; mask its character row.
    build_row = next(row for row, line in enumerate(rendered_lines) if line.startswith('Build '))
    begin, end = build_row * 16 * width * 3, (build_row + 1) * 16 * width * 3
    actual = actual[:begin] + expected[begin:end] + actual[end:]
    if actual == expected:
        return []
    pixels = sum(actual[offset:offset + 3] != expected[offset:offset + 3]
                 for offset in range(0, len(actual), 3))
    return ['%d stop-screen pixels differ' % pixels]


def resolution_problems(lines: list[str], expected: tuple[int, int] | None) -> list[str]:
    """Independent EDID/policy oracle in addition to the pixel comparison."""
    if expected is None:
        return []
    starts = [START_PATTERN.fullmatch(line) for line in lines if line.startswith(START_PREFIX)]
    modes = [(int(match[1]), int(match[2])) for match in starts if match]
    return [] if modes == [expected] else ['display resolution %r, expected %r' % (modes, expected)]


def normalize_qmp_ppm(data: bytes) -> bytes:
    """Convert QMP's four-byte-aligned RGB24 rows to standard packed P6.

    QEMU 10.2 screendump retains RGB24 row padding at widths such as 1366.
    Accept only packed rows or that exact storage extent; compare every real
    pixel unchanged. General PPM parsing remains strict.
    """
    width, height, pixels = _ppm_payload(data)
    row = width * 3
    stride = (row + 3) & ~3
    if len(pixels) == row * height:
        return data
    if len(pixels) != stride * height:
        raise DisplayError('QMP PPM has %d pixel bytes, expected %d packed or %d aligned' %
                           (len(pixels), row * height, stride * height))
    packed = b''.join(pixels[offset:offset + row] for offset in range(0, len(pixels), stride))
    return write_ppm(width, height, packed)


def cursor_position(rgb, width, height, cursor):
    """Locate the source arrow by all its opaque pixels, without client logs."""
    cw, ch, hot, pixels = cursor
    opaque = [(x, y, bytes(((value >> 16) & 255, (value >> 8) & 255, value & 255)))
              for y in range(ch) for x in range(cw)
              if (value := pixels[y * cw + x]) >> 24]
    ax, ay, anchor = opaque[0]
    at = 0
    found = []
    while (at := rgb.find(anchor, at)) >= 0:
        if at % 3 == 0:
            y, x = divmod(at // 3, width)
            x, y = x - ax, y - ay
            if (0 <= x and 0 <= y and x + cw <= width and y + ch <= height
                    and all(rgb[((y + dy) * width + x + dx) * 3:
                                ((y + dy) * width + x + dx) * 3 + 3] == color
                            for dx, dy, color in opaque)):
                found.append((x + hot[0], y + hot[1]))
        at += 1
    return found[0] if len(found) == 1 else None
