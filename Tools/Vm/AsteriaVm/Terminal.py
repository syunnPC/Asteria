# SPDX-License-Identifier: Apache-2.0
"""The runner's model of the desktop session's terminal.

A port of Source/User/Terminal (TerminalScreen, TerminalParser, the renderer)
used to compare screenshots of terminal windows pixel for pixel: a VM case
replays the bytes a terminal logged and renders them with this model. Its
constants, the code page 437 table, the palette and the font are parsed from
the C++ headers, so the model cannot drift from them; the rendering corpus
(Tests/User/Terminal/TerminalCorpus.txt) and its digests pin the two to
byte-identical images.
"""
from __future__ import annotations

import pathlib

from AsteriaVm import Display
from AsteriaVm.RenderCorpus import CorpusError, header_color, header_constant, header_text, glyph_mapping

SCREEN_HEADER = 'Source/User/Terminal/TerminalScreen.hpp'
PARSER_HEADER = 'Source/User/Terminal/TerminalParser.hpp'
RENDERER_HEADER = 'Source/User/Terminal/TerminalRenderer.hpp'
PALETTE_HEADER = 'Source/User/Session/Palette.hpp'

ESC, CAN, SUB, DEL, BEL = 0x1B, 0x18, 0x1A, 0x7F, 0x07
GROUND, ESCAPE, ESCAPE_INTERMEDIATE, CSI_ENTRY, CSI_PARAMETER, CSI_INTERMEDIATE, CSI_IGNORE, OSC_STRING, \
    IGNORED_STRING, STRING_ESCAPE = range(10)

Cell = tuple[int, int, int, int]  # Unicode code point, attributes, foreground, background


class TerminalModel:
    """The constants, tables, palette and font of the C++ terminal."""

    def __init__(self, repository: pathlib.Path):
        screen = header_text(repository, SCREEN_HEADER)
        parser = header_text(repository, PARSER_HEADER)
        renderer = header_text(repository, RENDERER_HEADER)
        palette = header_text(repository, PALETTE_HEADER)
        self.default_columns = header_constant(screen, 'TerminalDefaultColumns')
        self.default_rows = header_constant(screen, 'TerminalDefaultRows')
        self.maximum_columns = header_constant(screen, 'TerminalMaximumColumns')
        self.maximum_rows = header_constant(screen, 'TerminalMaximumRows')
        self.tab_width = header_constant(screen, 'TerminalTabWidth')
        self.default_color = header_constant(screen, 'TerminalDefaultColor')
        self.blank_code = header_constant(screen, 'TerminalBlankCode')
        self.bold = header_constant(screen, 'TerminalBold')
        self.underline = header_constant(screen, 'TerminalUnderline')
        self.reverse = header_constant(screen, 'TerminalReverse')
        self.response_capacity = header_constant(parser, 'TerminalResponseCapacity')
        self.maximum_parameters = header_constant(parser, 'TerminalMaximumParameters')
        self.maximum_parameter_value = header_constant(parser, 'TerminalMaximumParameterValue')
        self.cell_width = header_constant(renderer, 'TerminalCellWidth')
        self.cell_height = header_constant(renderer, 'TerminalCellHeight')
        self.underline_row = header_constant(renderer, 'TerminalUnderlineRow')
        self.frame_extra_width = header_constant(renderer, 'TerminalFrameExtraWidth')
        self.frame_extra_height = header_constant(renderer, 'TerminalFrameExtraHeight')
        if (self.cell_width, self.cell_height) != (8, 16):
            raise CorpusError('the terminal cell is %dx%d, expected 8x16' % (self.cell_width, self.cell_height))
        self.colors = [header_color(palette, 'TerminalColor%d' % index) for index in range(16)]
        self.default_foreground = header_color(palette, 'TerminalDefaultForeground')
        self.default_background = header_color(palette, 'TerminalDefaultBackground')
        self.cp437, self.replacement = glyph_mapping(repository)
        self.replacement_point = header_constant(header_text(repository, 'Source/User/Text/Utf8.hpp'), 'ReplacementCodePoint')
        self.font = Display.load_font(repository)

    def terminal(self, columns: int, rows: int) -> 'Terminal':
        return Terminal(self, columns, rows)

    def map_code_point(self, point: int) -> int:
        if 0x20 <= point <= 0x7E:
            return point
        return self.cp437.get(point, self.replacement)

    def grid_for_output(self, width: int, height: int) -> tuple[int, int]:
        """TerminalGridForOutput: the default grid or the largest that fits."""
        columns = max(0, width - self.frame_extra_width) >> 3
        rows = max(0, height - self.frame_extra_height) >> 4
        return (max(1, min(columns, self.default_columns)), max(1, min(rows, self.default_rows)))

    def cell_colors(self, cell: Cell, cursor: bool) -> tuple[tuple[int, int, int], tuple[int, int, int]]:
        """ResolveCellColors."""
        _code, attributes, foreground, background = cell
        if attributes & self.bold and foreground < 8:
            foreground += 8
        fore = self.colors[foreground] if foreground < 16 else self.default_foreground
        back = self.colors[background] if background < 16 else self.default_background
        if bool(attributes & self.reverse) != cursor:
            fore, back = back, fore
        return fore, back


class Terminal:
    """A port of User/Terminal's Terminal: screen, parser and answers."""

    def __init__(self, model: TerminalModel, columns: int, rows: int):
        self.model = model
        self.columns = max(1, min(columns, model.maximum_columns))
        self.rows = max(1, min(rows, model.maximum_rows))
        self.responses = bytearray()
        self.dropped = 0
        self.state = GROUND
        self.private = 0
        self.parameters: list[int] = []
        self.code_point = 0
        self.remaining = 0
        self.lower, self.upper = 0x80, 0xBF
        self.reset()

    # The screen (TerminalScreen).

    def _default_rendition(self) -> tuple[int, int, int]:
        return (0, self.model.default_color, self.model.default_color)

    def _blank(self) -> Cell:
        return (self.model.blank_code, 0, self.model.default_color, self.rendition[2])

    def reset(self) -> None:
        self.rendition = self._default_rendition()
        self.saved_rendition = self.rendition
        self.cells = [[self._blank() for _ in range(self.columns)] for _ in range(self.rows)]
        self.row = self.column = 0
        self.saved_row = self.saved_column = 0
        self.top, self.bottom = 0, self.rows - 1
        self.wrap_pending = False
        self.autowrap = True
        self.cursor_visible = True
        self.application_cursor_keys = False

    def resize(self, columns: int, rows: int) -> None:
        columns = max(1, min(columns, self.model.maximum_columns))
        rows = max(1, min(rows, self.model.maximum_rows))
        default = self.model.default_color
        blank = (self.model.blank_code, 0, default, default)
        cells = []
        for row in range(rows):
            kept = self.cells[row][:columns] if row < self.rows else []
            cells.append(kept + [blank] * (columns - len(kept)))
        self.cells, self.columns, self.rows = cells, columns, rows
        self.row = min(self.row, rows - 1)
        self.column = min(self.column, columns - 1)
        self.saved_row = min(self.saved_row, rows - 1)
        self.saved_column = min(self.saved_column, columns - 1)
        self.top, self.bottom = 0, rows - 1
        self.wrap_pending = False

    def print(self, code: int) -> None:
        if self.wrap_pending:
            self.wrap_pending = False
            if self.autowrap:
                self.column = 0
                self.line_feed()
        attributes, foreground, background = self.rendition
        self.cells[self.row][self.column] = (code, attributes, foreground, background)
        if self.column + 1 < self.columns:
            self.column += 1
        else:
            self.wrap_pending = self.autowrap

    def backspace(self) -> None:
        self.wrap_pending = False
        if self.column:
            self.column -= 1

    def horizontal_tab(self) -> None:
        self.wrap_pending = False
        stop = (self.column & ~(self.model.tab_width - 1)) + self.model.tab_width
        self.column = min(stop, self.columns - 1)

    def line_feed(self) -> None:
        self.wrap_pending = False
        if self.row == self.bottom:
            self._shift_up(self.top, self.bottom, 1)
        elif self.row + 1 < self.rows:
            self.row += 1

    def reverse_index(self) -> None:
        self.wrap_pending = False
        if self.row == self.top:
            self._shift_down(self.top, self.bottom, 1)
        elif self.row:
            self.row -= 1

    def carriage_return(self) -> None:
        self.wrap_pending = False
        self.column = 0

    def cursor_up(self, count: int) -> None:
        self.wrap_pending = False
        limit = self.top if self.row >= self.top else 0
        self.row = self.row - count if self.row - limit > count else limit

    def cursor_down(self, count: int) -> None:
        self.wrap_pending = False
        limit = self.bottom if self.row <= self.bottom else self.rows - 1
        self.row = self.row + count if limit - self.row > count else limit

    def cursor_forward(self, count: int) -> None:
        self.wrap_pending = False
        self.column = min(self.column + count, self.columns - 1)

    def cursor_backward(self, count: int) -> None:
        self.wrap_pending = False
        self.column = max(self.column - count, 0)

    def set_cursor(self, row: int, column: int) -> None:
        self.wrap_pending = False
        self.row = min(row, self.rows - 1)
        self.column = min(column, self.columns - 1)

    def _blank_cells(self, row: int, first: int, end: int) -> None:
        blank = self._blank()
        for column in range(first, min(end, self.columns)):
            self.cells[row][column] = blank

    def erase_in_display(self, selector: int) -> None:
        if selector == 0:
            self._blank_cells(self.row, self.column, self.columns)
            for row in range(self.row + 1, self.rows):
                self._blank_cells(row, 0, self.columns)
        elif selector == 1:
            for row in range(self.row):
                self._blank_cells(row, 0, self.columns)
            self._blank_cells(self.row, 0, self.column + 1)
        elif selector == 2:
            for row in range(self.rows):
                self._blank_cells(row, 0, self.columns)

    def erase_in_line(self, selector: int) -> None:
        if selector == 0:
            self._blank_cells(self.row, self.column, self.columns)
        elif selector == 1:
            self._blank_cells(self.row, 0, self.column + 1)
        elif selector == 2:
            self._blank_cells(self.row, 0, self.columns)

    def erase_characters(self, count: int) -> None:
        self._blank_cells(self.row, self.column, self.column + min(count, self.columns - self.column))

    def insert_characters(self, count: int) -> None:
        self.wrap_pending = False
        shift = min(count, self.columns - self.column)
        if not shift:
            return
        line = self.cells[self.row]
        line[self.column + shift:] = line[self.column:self.columns - shift]
        self._blank_cells(self.row, self.column, self.column + shift)

    def delete_characters(self, count: int) -> None:
        self.wrap_pending = False
        shift = min(count, self.columns - self.column)
        if not shift:
            return
        line = self.cells[self.row]
        line[self.column:self.columns - shift] = line[self.column + shift:]
        self._blank_cells(self.row, self.columns - shift, self.columns)

    def _inside_region(self) -> bool:
        return self.top <= self.row <= self.bottom

    def insert_lines(self, count: int) -> None:
        if not count or not self._inside_region():
            return
        self.wrap_pending = False
        self._shift_down(self.row, self.bottom, count)
        self.column = 0

    def delete_lines(self, count: int) -> None:
        if not count or not self._inside_region():
            return
        self.wrap_pending = False
        self._shift_up(self.row, self.bottom, count)
        self.column = 0

    def set_margins(self, top: int, bottom: int) -> None:
        if top >= bottom or bottom >= self.rows:
            return
        self.top, self.bottom = top, bottom
        self.set_cursor(0, 0)

    def save_cursor(self) -> None:
        self.saved_row, self.saved_column, self.saved_rendition = self.row, self.column, self.rendition

    def restore_cursor(self) -> None:
        self.rendition = self.saved_rendition
        self.set_cursor(self.saved_row, self.saved_column)

    def _shift_up(self, first: int, last: int, count: int) -> None:
        if first > last or last >= self.rows or not count:
            return
        shift = min(count, last - first + 1)
        for row in range(first, last + 1 - shift):
            self.cells[row] = list(self.cells[row + shift])
        for row in range(last + 1 - shift, last + 1):
            self.cells[row] = [self._blank()] * self.columns

    def _shift_down(self, first: int, last: int, count: int) -> None:
        if first > last or last >= self.rows or not count:
            return
        shift = min(count, last - first + 1)
        for row in range(last, first + shift - 1, -1):
            self.cells[row] = list(self.cells[row - shift])
        for row in range(first, first + shift):
            self.cells[row] = [self._blank()] * self.columns

    # The parser (TerminalParser).

    def write(self, data: bytes) -> None:
        for byte in data:
            self.feed(byte)

    def take_responses(self) -> bytes:
        taken, self.responses = bytes(self.responses), bytearray()
        return taken

    def feed(self, byte: int) -> None:
        state = self.state
        if state != GROUND and byte in (CAN, SUB):
            self.state = GROUND
            return
        if state == GROUND:
            self._ground(byte)
        elif state == ESCAPE:
            self._escape(byte)
        elif state == ESCAPE_INTERMEDIATE:
            self._escape_intermediate(byte)
        elif state in (CSI_ENTRY, CSI_PARAMETER):
            self._csi(byte)
        elif state == CSI_INTERMEDIATE:
            self._csi_intermediate(byte)
        elif state == CSI_IGNORE:
            self._csi_ignore(byte)
        elif state in (OSC_STRING, IGNORED_STRING):
            if byte == ESC:
                self.state = STRING_ESCAPE
            elif byte == BEL and state == OSC_STRING:
                self.state = GROUND
        else:  # STRING_ESCAPE
            if byte == 0x5C:
                self.state = GROUND
            else:
                self.state = ESCAPE
                self._escape(byte)

    def _execute(self, byte: int) -> None:
        if byte == 0x08:
            self.backspace()
        elif byte == 0x09:
            self.horizontal_tab()
        elif byte in (0x0A, 0x0B, 0x0C):
            self.line_feed()
        elif byte == 0x0D:
            self.carriage_return()

    def _ground(self, byte: int) -> None:
        if self.remaining:
            if self.lower <= byte <= self.upper:
                self.code_point = (self.code_point << 6) | (byte & 0x3F)
                self.lower, self.upper = 0x80, 0xBF
                self.remaining -= 1
                if not self.remaining:
                    self._print_code_point(self.code_point)
                return
            self.remaining = 0
            self.print(self.model.replacement_point)
        if byte == ESC:
            self.state = ESCAPE
        elif byte < 0x20:
            self._execute(byte)
        elif byte < DEL:
            self.print(byte)
        elif byte > DEL:
            self._start_utf8(byte)

    def _start_utf8(self, byte: int) -> None:
        self.lower, self.upper = 0x80, 0xBF
        if 0xC2 <= byte <= 0xDF:
            self.code_point, self.remaining = byte & 0x1F, 1
        elif 0xE0 <= byte <= 0xEF:
            self.code_point, self.remaining = byte & 0x0F, 2
            if byte == 0xE0:
                self.lower = 0xA0
            elif byte == 0xED:
                self.upper = 0x9F
        elif 0xF0 <= byte <= 0xF4:
            self.code_point, self.remaining = byte & 0x07, 3
            if byte == 0xF0:
                self.lower = 0x90
            elif byte == 0xF4:
                self.upper = 0x8F
        else:
            self.print(self.model.replacement_point)

    def _print_code_point(self, point: int) -> None:
        if 0x80 <= point <= 0x9F:
            return
        self.print(point)

    def _escape(self, byte: int) -> None:
        if byte == ESC or byte == DEL:
            return
        if byte < 0x20:
            self._execute(byte)
            return
        if byte < 0x30:
            self.state = ESCAPE_INTERMEDIATE
            return
        self.state = GROUND
        if byte > DEL:
            self._ground(byte)
        elif byte == ord('['):
            self.private, self.parameters = 0, []
            self.state = CSI_ENTRY
        elif byte == ord(']'):
            self.state = OSC_STRING
        elif byte in b'PX^_':
            self.state = IGNORED_STRING
        elif byte == ord('7'):
            self.save_cursor()
        elif byte == ord('8'):
            self.restore_cursor()
        elif byte == ord('D'):
            self.line_feed()
        elif byte == ord('M'):
            self.reverse_index()
        elif byte == ord('E'):
            self.carriage_return()
            self.line_feed()
        elif byte == ord('c'):
            self.reset()

    def _escape_intermediate(self, byte: int) -> None:
        if byte == ESC:
            self.state = ESCAPE
        elif byte < 0x20:
            self._execute(byte)
        elif byte < 0x30 or byte == DEL:
            pass
        else:
            self.state = GROUND
            if byte > DEL:
                self._ground(byte)

    def _csi(self, byte: int) -> None:
        if byte == ESC:
            self.state = ESCAPE
        elif byte < 0x20:
            self._execute(byte)
        elif byte == DEL:
            pass
        elif byte > DEL:
            self.state = GROUND
            self._ground(byte)
        elif 0x30 <= byte <= 0x39:
            if not self.parameters:
                self.parameters = [0]
            value = self.parameters[-1] * 10 + byte - 0x30
            self.parameters[-1] = min(value, self.model.maximum_parameter_value)
            self.state = CSI_PARAMETER
        elif byte == ord(';'):
            self.state = CSI_PARAMETER
            if not self.parameters:
                self.parameters = [0]
            if len(self.parameters) == self.model.maximum_parameters:
                self.state = CSI_IGNORE
            else:
                self.parameters.append(0)
        elif byte == ord(':'):
            self.state = CSI_IGNORE
        elif 0x3C <= byte <= 0x3F:
            if self.state == CSI_ENTRY:
                self.private = byte
                self.state = CSI_PARAMETER
            else:
                self.state = CSI_IGNORE
        elif byte < 0x30:
            self.state = CSI_INTERMEDIATE
        else:
            self.state = GROUND
            self._dispatch(byte)

    def _csi_intermediate(self, byte: int) -> None:
        if byte == ESC:
            self.state = ESCAPE
        elif byte < 0x20:
            self._execute(byte)
        elif byte < 0x30 or byte == DEL:
            pass
        elif byte < 0x40:
            self.state = CSI_IGNORE
        else:
            self.state = GROUND
            if byte > DEL:
                self._ground(byte)

    def _csi_ignore(self, byte: int) -> None:
        if byte == ESC:
            self.state = ESCAPE
        elif byte < 0x20:
            self._execute(byte)
        elif byte < 0x40 or byte == DEL:
            pass
        else:
            self.state = GROUND
            if byte > DEL:
                self._ground(byte)

    def _count(self, index: int) -> int:
        if index >= len(self.parameters) or self.parameters[index] == 0:
            return 1
        return self.parameters[index]

    def _selector(self, index: int) -> int:
        return self.parameters[index] if index < len(self.parameters) else 0

    def _dispatch(self, final: int) -> None:
        if self.private == ord('?'):
            if final in (ord('h'), ord('l')):
                for value in self.parameters:
                    if value == 1:
                        self.application_cursor_keys = final == ord('h')
                    elif value == 7:
                        self.autowrap = final == ord('h')
                    elif value == 25:
                        self.cursor_visible = final == ord('h')
            return
        if self.private:
            return
        final_char = chr(final)
        count = self._count(0)
        if final_char == '@':
            self.insert_characters(count)
        elif final_char == 'A':
            self.cursor_up(count)
        elif final_char == 'B':
            self.cursor_down(count)
        elif final_char == 'C':
            self.cursor_forward(count)
        elif final_char == 'D':
            self.cursor_backward(count)
        elif final_char == 'E':
            self.cursor_down(count)
            self.carriage_return()
        elif final_char == 'F':
            self.cursor_up(count)
            self.carriage_return()
        elif final_char == 'G':
            self.set_cursor(self.row, count - 1)
        elif final_char in 'Hf':
            self.set_cursor(count - 1, self._count(1) - 1)
        elif final_char == 'J':
            self.erase_in_display(self._selector(0))
        elif final_char == 'K':
            self.erase_in_line(self._selector(0))
        elif final_char == 'L':
            self.insert_lines(count)
        elif final_char == 'M':
            self.delete_lines(count)
        elif final_char == 'P':
            self.delete_characters(count)
        elif final_char == 'S':
            self._shift_up(self.top, self.bottom, count)
        elif final_char == 'T':
            self._shift_down(self.top, self.bottom, count)
        elif final_char == 'X':
            self.erase_characters(count)
        elif final_char == 'd':
            self.set_cursor(count - 1, self.column)
        elif final_char == 'm':
            self._select_graphic_rendition()
        elif final_char in 'cn':
            self._report(final_char)
        elif final_char == 'r':
            bottom = self._selector(1)
            if bottom == 0 or bottom > self.rows:
                bottom = self.rows
            self.set_margins(count - 1, bottom - 1)

    def _select_graphic_rendition(self) -> None:
        attributes, foreground, background = self.rendition
        default = self.model.default_color
        values = self.parameters or [0]
        index = 0
        while index < len(values):
            value = values[index]
            if value == 0:
                attributes, foreground, background = 0, default, default
            elif value == 1:
                attributes |= self.model.bold
            elif value == 4:
                attributes |= self.model.underline
            elif value == 7:
                attributes |= self.model.reverse
            elif value == 22:
                attributes &= ~self.model.bold
            elif value == 24:
                attributes &= ~self.model.underline
            elif value == 27:
                attributes &= ~self.model.reverse
            elif 30 <= value <= 37:
                foreground = value - 30
            elif value == 39:
                foreground = default
            elif 40 <= value <= 47:
                background = value - 40
            elif value == 49:
                background = default
            elif 90 <= value <= 97:
                foreground = value - 90 + 8
            elif 100 <= value <= 107:
                background = value - 100 + 8
            elif value in (38, 48):
                form = self._selector(index + 1)
                if form == 5:
                    index += 2
                elif form == 2:
                    index += 4
            index += 1
        self.rendition = (attributes, foreground, background)

    def _report(self, final: str) -> None:
        selector = self._selector(0)
        if final == 'c':
            if len(self.parameters) > 1 or selector:
                return
            answer = b'\x1b[?1;2c'
        elif selector == 5:
            answer = b'\x1b[0n'
        elif selector == 6:
            answer = b'\x1b[%d;%dR' % (self.row + 1, self.column + 1)
        else:
            return
        if len(self.responses) + len(answer) > self.model.response_capacity:
            self.dropped += 1
            return
        self.responses += answer

    # The renderer.

    def cell(self, row: int, column: int) -> Cell:
        if 0 <= row < self.rows and 0 <= column < self.columns:
            return self.cells[row][column]
        default = self.model.default_color
        return (self.model.blank_code, 0, default, default)

    def render_rgb(self) -> bytes:
        """RenderTerminal at origin (0, 0) into a grid-sized image, as packed
        RGB24 rows: columns x 8 by rows x 16 pixels, the cursor included."""
        model = self.model
        strips: dict[tuple, bytes] = {}
        lines: list[bytes] = []
        for row in range(self.rows):
            looks = []
            for column in range(self.columns):
                cell = self.cells[row][column]
                cursor = self.cursor_visible and row == self.row and column == self.column
                fore, back = model.cell_colors(cell, cursor)
                looks.append((model.map_code_point(cell[0]), bytes(fore), bytes(back), bool(cell[1] & model.underline)))
            for line in range(model.cell_height):
                parts = []
                for look in looks:
                    key = look + (line,)
                    strip = strips.get(key)
                    if strip is None:
                        code, fore, back, underlined = look
                        bits = 0xFF if underlined and line == model.underline_row else model.font[code][line]
                        strip = strips[key] = b''.join(fore if (bits >> (7 - bit)) & 1 else back for bit in range(8))
                    parts.append(strip)
                lines.append(b''.join(parts))
        return b''.join(lines)


def render_case(model: TerminalModel, case) -> tuple[int, int, bytes]:
    """(width, height, RGB) of a RenderCorpus case of kind 'terminal'."""
    if case.kind != 'terminal' or len(case.arguments) != 3 or case.arguments[2] not in ('rgbx', 'bgrx'):
        raise CorpusError('case %s is not a terminal case' % case.name)
    terminal = model.terminal(int(case.arguments[0]), int(case.arguments[1]))
    for number, tokens in case.statements:
        if tokens[0] == 'write' and len(tokens) == 2 and isinstance(tokens[1], bytes):
            terminal.write(tokens[1])
        elif tokens[0] == 'resize' and len(tokens) == 3:
            terminal.resize(int(tokens[1]), int(tokens[2]))
        else:
            raise CorpusError('line %d: unknown terminal statement' % number)
    return terminal.columns * model.cell_width, terminal.rows * model.cell_height, terminal.render_rgb()
