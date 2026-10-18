# SPDX-License-Identifier: Apache-2.0
"""The desktop session's scene model (desktop session).

A port of the compositor's composition (Source/User/Session/Scene.cpp and
Decorations.cpp): the desktop, windows with their server-side decorations and
their clients' images, the outline of an outline move and the Alt+Tab
switcher. Its colours, geometry, button marks and title marks are parsed from
Palette.hpp and Decorations.hpp, its bitmap glyphs from Bold8x16Font.hpp and
its UI coverage masks from the worktree's C++ UiTextMasks tool. The oracle's
shared shaping/rasterization limits are in DesktopSessionLibraries.md. The rendering corpus
(Tests/User/Session/SceneCorpus.txt) checks it byte for byte against the C++
renderer. Later VM cases compare screendumps with it, fed the compositor's
`# session:` lines.

The model draws the whole output the naive way (Scene::RenderAll): every
primitive is a pure function of its coordinates and clip, so the order of
fills below is the C++ order and the result equals the compositor's
damage-driven composition.
"""
from __future__ import annotations

import dataclasses
import pathlib
import functools
import os
import subprocess
import re

from AsteriaVm import Display, RenderCorpus

PALETTE_HEADER = 'Source/User/Session/Palette.hpp'
DECORATIONS_HEADER = 'Source/User/Session/Decorations.hpp'
POLICY_HEADER = 'Source/User/Session/WindowManager.hpp'
Color = tuple[int, int, int]
Rect = tuple[int, int, int, int]  # X, y, width, height

COLOR_NAMES = (
    'Desktop', 'Face', 'Highlight', 'Light', 'Shadow', 'DarkShadow', 'ActiveTitle', 'ActiveTitleText',
    'ActiveSubtitle', 'InactiveTitle', 'InactiveTitleText', 'InactiveSubtitle', 'AttentionTitle', 'Glyph')
CONSTANT_NAMES = (
    'FrameWidth', 'TitleBarHeight', 'SeparatorHeight', 'FrameExtraWidth', 'FrameExtraHeight', 'TitleTextInset',
    'TitleTextTop', 'TitleTextGap', 'ButtonWidth', 'ButtonHeight', 'ButtonTop', 'ButtonRightInset', 'ButtonGap',
    'ResizeZone', 'CornerExtent', 'SwitcherHeight', 'SwitcherPadding', 'SwitcherMargin', 'SwitcherTextTop',
    'OutlineWidth', 'VolumeCodePoint', 'SubtitleReplacement', 'TitleNameBytes', 'SubtitleBytes')
GLYPH_WIDTH = 8
GLYPH_HEIGHT = 16


def _string(text: str, name: str) -> bytes:
    match = re.search(r'\b%s\[\]\s*\{\s*"([^"\\]*)"\s*\}' % re.escape(name), text)
    if match is None:
        raise RenderCorpus.CorpusError('no string %s' % name)
    return match.group(1).encode('ascii')


def _mark(text: str, name: str) -> tuple[int, int, list[int], int, int]:
    """(width, height, rows, left, top) of a button glyph."""
    width = RenderCorpus.header_constant(text, name + 'Width')
    height = RenderCorpus.header_constant(text, name + 'Height')
    left = RenderCorpus.header_constant(text, name + 'Left')
    top = RenderCorpus.header_constant(text, name + 'Top')
    match = re.search(r'\b%s\[%sHeight\]\s*\{(.*?)\};' % (re.escape(name), re.escape(name)), text, re.S)
    if match is None:
        raise RenderCorpus.CorpusError('no glyph %s' % name)
    rows = re.findall(r'0b([01]+)', match.group(1))
    if len(rows) != height or any(len(row) != width for row in rows):
        raise RenderCorpus.CorpusError('glyph %s is not %dx%d' % (name, width, height))
    return width, height, [int(row, 2) for row in rows], left, top


def intersect(first: Rect, second: Rect) -> Rect:
    left, top = max(first[0], second[0]), max(first[1], second[1])
    right = min(first[0] + first[2], second[0] + second[2])
    bottom = min(first[1] + first[3], second[1] + second[3])
    if right <= left or bottom <= top:
        return (0, 0, 0, 0)
    return (left, top, right - left, bottom - top)


def empty(rect: Rect) -> bool:
    return rect[2] <= 0 or rect[3] <= 0


@dataclasses.dataclass
class Image:
    width: int
    height: int
    rgb: bytes


@dataclasses.dataclass
class Window:
    frame: Rect
    name: bytes = b''
    subtitle: bytes = b''
    volume: bool = False
    state: str = 'unfocused'  # Unfocused, focused or attention
    not_responding: bool = False
    maximized: bool = False
    close_pressed: bool = False
    maximize_pressed: bool = False
    image: Image | None = None


class SessionModel:
    """The constants of the C++ sources and the operations that use them."""

    def __init__(self, repository: pathlib.Path):
        palette = RenderCorpus.header_text(repository, PALETTE_HEADER)
        decorations = RenderCorpus.header_text(repository, DECORATIONS_HEADER)
        self.colors = {name: RenderCorpus.header_color(palette, name) for name in COLOR_NAMES}
        self.constants = {name: RenderCorpus.header_constant(decorations, name) for name in CONSTANT_NAMES}
        policy = RenderCorpus.header_text(repository, POLICY_HEADER)
        self.policy = {name: RenderCorpus.header_constant(policy, name) for name in
                       ('CascadeOrigin', 'CascadeStep', 'VisibleTitleWidth', 'SystemRaiseIgnoreUs',
                        'DoubleClickUs', 'DoubleClickDistance', 'OutlinePassUs')}
        self.marks = {name: _mark(decorations, name) for name in ('CloseGlyph', 'MaximizeGlyph', 'RestoreGlyph')}
        self.strings = {name: _string(decorations, name) for name in ('SubtitleMark', 'NotRespondingMark')}
        self.title_text_bytes = (4 + self.constants['TitleNameBytes'] + len(self.strings['SubtitleMark'])
                                 + self.constants['SubtitleBytes'] + len(self.strings['NotRespondingMark']))
        self.ui_constants = {name: RenderCorpus.header_constant(
            RenderCorpus.header_text(repository, 'Source/User/Session/UiText.hpp'), name)
            for name in ('UiPixelSize', 'UiTextHeight')}
        self.repository = repository
        self.ui_faces: tuple[bool, bool] | None = None
        self.font = Display.load_font(repository)
        self.glyphs, self.replacement = RenderCorpus.glyph_mapping(repository)

    # Geometry: Decorations.cpp's rectangles.
    def content_rect(self, frame: Rect) -> Rect:
        c = self.constants
        rect = (frame[0] + c['FrameWidth'], frame[1] + c['FrameWidth'] + c['TitleBarHeight'] + c['SeparatorHeight'],
                frame[2] - c['FrameExtraWidth'], frame[3] - c['FrameExtraHeight'])
        return rect if not empty(rect) else (0, 0, 0, 0)

    def title_bar_rect(self, frame: Rect) -> Rect:
        f, t = self.constants['FrameWidth'], self.constants['TitleBarHeight']
        rect = (frame[0] + f, frame[1] + f, frame[2] - 2 * f, min(frame[3] - 2 * f, t))
        return rect if not empty(rect) else (0, 0, 0, 0)

    def close_button_rect(self, frame: Rect) -> Rect:
        c = self.constants
        return (frame[0] + frame[2] - c['FrameWidth'] - c['ButtonRightInset'] - c['ButtonWidth'],
                frame[1] + c['FrameWidth'] + c['ButtonTop'], c['ButtonWidth'], c['ButtonHeight'])

    def maximize_button_rect(self, frame: Rect) -> Rect:
        c = self.constants
        return (frame[0] + frame[2] - c['FrameWidth'] - c['ButtonRightInset'] - 2 * c['ButtonWidth'] - c['ButtonGap'],
                frame[1] + c['FrameWidth'] + c['ButtonTop'], c['ButtonWidth'], c['ButtonHeight'])

    def maximized_frame(self, width: int, height: int) -> Rect:
        return (0, 0, width, height)

    def frame_around_content(self, x: int, y: int, width: int, height: int) -> Rect:
        c = self.constants
        return (x - c['FrameWidth'], y - (c['FrameExtraHeight'] - c['FrameWidth']), width + c['FrameExtraWidth'],
                height + c['FrameExtraHeight'])

    def switcher_rect(self, width: int, height: int, length: int) -> Rect:
        c = self.constants
        wanted = min(length, self.title_text_bytes) * GLYPH_WIDTH + 2 * c['SwitcherPadding']
        limit = width - c['SwitcherMargin'] if width > c['SwitcherMargin'] else width
        box_width = min(wanted, limit)
        x = (width - box_width) >> 1 if width > box_width else 0
        y = (height - c['SwitcherHeight']) >> 1 if height > c['SwitcherHeight'] else 0
        return (x, y, box_width, c['SwitcherHeight'])

    def title_text(self, window: Window) -> bytes:
        """The whole title (ComposeTitle), as the switcher shows it."""
        c = self.constants
        text = (chr(c['VolumeCodePoint']) + ' ').encode('utf-8') if window.volume else b''
        text += window.name[:c['TitleNameBytes']]
        if window.subtitle:
            text += self.strings['SubtitleMark'] + sanitize_subtitle(window.subtitle[:c['SubtitleBytes']], c)
        if window.not_responding:
            text += self.strings['NotRespondingMark']
        return text[:self.title_text_bytes]


class Canvas:
    """An output of packed RGB24 pixels with Scene.cpp's clipped primitives."""

    def __init__(self, model: SessionModel, width: int, height: int):
        self.model = model
        self.width = width
        self.height = height
        self.rgb = bytearray(width * height * 3)

    def fill(self, clip: Rect, x: int, y: int, width: int, height: int, color: Color) -> None:
        if width <= 0 or height <= 0:
            return
        left, top, cut_width, cut_height = intersect((x, y, width, height), clip)
        if cut_width <= 0 or cut_height <= 0:
            return
        run = bytes(color) * cut_width
        for row in range(top, top + cut_height):
            at = (row * self.width + left) * 3
            self.rgb[at:at + cut_width * 3] = run

    def glyph(self, clip: Rect, x: int, y: int, code: int, foreground: Color, background: Color) -> None:
        area = intersect((x, y, GLYPH_WIDTH, GLYPH_HEIGHT), clip)
        if empty(area):
            return
        bits_of = self.model.font[code & 0xFF]
        fore, back = bytes(foreground), bytes(background)
        for row in range(area[1], area[1] + area[3]):
            bits = bits_of[row - y]
            line = b''.join(fore if (bits >> (7 - (column - x))) & 1 else back
                            for column in range(area[0], area[0] + area[2]))
            at = (row * self.width + area[0]) * 3
            self.rgb[at:at + area[2] * 3] = line

    def copy(self, clip: Rect, image: Image, x: int, y: int, width: int, height: int) -> None:
        """Image pixels [0, width) x [0, height) to (x, y), clipped."""
        area = intersect((x, y, width, height), clip)
        if empty(area):
            return
        for row in range(area[1], area[1] + area[3]):
            source = ((row - y) * image.width + (area[0] - x)) * 3
            at = (row * self.width + area[0]) * 3
            self.rgb[at:at + area[2] * 3] = image.rgb[source:source + area[2] * 3]

    def bevel(self, clip: Rect, box: Rect, outer_top_left: Color, outer_bottom_right: Color,
              inner_top_left: Color, inner_bottom_right: Color) -> None:
        x, y, w, h = box
        self.fill(clip, x, y, w, 1, outer_top_left)
        self.fill(clip, x, y, 1, h, outer_top_left)
        self.fill(clip, x, y + h - 1, w, 1, outer_bottom_right)
        self.fill(clip, x + w - 1, y, 1, h, outer_bottom_right)
        self.fill(clip, x + 1, y + 1, w - 2, 1, inner_top_left)
        self.fill(clip, x + 1, y + 1, 1, h - 2, inner_top_left)
        self.fill(clip, x + 1, y + h - 2, w - 2, 1, inner_bottom_right)
        self.fill(clip, x + w - 2, y + 1, 1, h - 2, inner_bottom_right)


@functools.lru_cache(maxsize=512)
def _ui_masks(repository: pathlib.Path, faces: tuple[bool, bool], width: int, text: bytes,
              bold: bool, trusted: int, suffix: int) -> tuple:
    tool = os.environ.get('ASTERIA_UI_TEXT_TOOL')
    if not tool:
        candidates = [repository / 'Build/CMake' / preset / directory / 'UiTextMasks'
                      for preset in ('host-debug', 'host-release', 'amd64', 'ia64')
                      for directory in ('Tools', 'HostTools')]
        tool = next((str(path) for path in candidates if path.is_file()), None)
    if not tool:
        raise RuntimeError('build UiTextMasks before comparing scalable UI text')
    data = repository / 'Data/Fonts/Vegur'
    paths = [str(data / ('Vegur-' + style + '.otf')) if present else '-'
             for style, present in zip(('Bold', 'Regular'), faces)]
    result = subprocess.run([tool, *paths, str(width), 'bold' if bold else 'regular',
                             str(trusted), str(suffix), text.hex()], check=True, stdout=subprocess.PIPE)
    data, at, masks = result.stdout, 6, []
    if not data.startswith(b'UITM1\n'):
        raise ValueError('invalid UI mask stream')
    end = data.index(b'\n', at)
    if not data[at:end].startswith(b'width '):
        raise ValueError('missing shaped UI width')
    natural_width, at = int(data[at+6:end]), end+1
    while at < len(data):
        end = data.index(b'\n', at)
        line, at = data[at:end], end+1
        if line == b'end':
            if at != len(data):
                raise ValueError('trailing UI mask data')
            return natural_width, tuple(masks)
        words = line.split()
        if len(words) != 13 or words[0] != b'mask':
            raise ValueError('invalid UI glyph mask')
        values = tuple(map(int, words[1:]))
        w, h = values[4:6]
        if not 0 <= w <= 512 or not 0 <= h <= 512 or at+w*h >= len(data) or data[at+w*h] != 10:
            raise ValueError('invalid UI glyph coverage extent')
        masks.append((values, data[at:at+w*h]))
        at += w*h+1
    raise ValueError('incomplete UI mask stream')


def _paint_ui(canvas: Canvas, clip: Rect, box: Rect, text: bytes, bold: bool,
              trusted: int, suffix: int, foreground: Color, subtitle: Color, background: Color) -> None:
    if box[2] <= 0 or empty(intersect(clip, box)):
        return
    _, masks = _ui_masks(canvas.model.repository, canvas.model.ui_faces, box[2], text, bold, trusted, suffix)
    for values, coverage in masks:
        x, y, bearing_x, bearing_y, w, h, cx, cy, cw, ch, layer, opaque = values
        glyph = (box[0]+x+bearing_x, box[1]+y+bearing_y, w, h)
        glyph_clip = intersect(clip, (box[0]+cx, box[1]+cy, cw, ch))
        left, top, width, height = intersect(glyph_clip, glyph)
        color = subtitle if layer else foreground
        for py in range(top, top+height):
            for px in range(left, left+width):
                alpha = coverage[(py-glyph[1])*w+px-glyph[0]]
                at = (py*canvas.width+px)*3
                if opaque:
                    canvas.rgb[at:at+3] = bytes(background)
                if alpha:
                    for channel in range(3):
                        canvas.rgb[at+channel] = (color[channel]*alpha + canvas.rgb[at+channel]*(255-alpha)+127)//255



def _window_bevel(model: SessionModel) -> tuple[Color, Color, Color, Color]:
    c = model.colors
    return c['Light'], c['DarkShadow'], c['Highlight'], c['Shadow']


def _paint_button(canvas: Canvas, clip: Rect, button: Rect, pressed: bool, mark: str) -> None:
    c = canvas.model.colors
    if pressed:
        canvas.bevel(clip, button, c['DarkShadow'], c['Highlight'], c['Shadow'], c['Light'])
    else:
        canvas.bevel(clip, button, c['Highlight'], c['DarkShadow'], c['Light'], c['Shadow'])
    canvas.fill(clip, button[0] + 2, button[1] + 2, button[2] - 4, button[3] - 4, c['Face'])
    width, height, rows, left, top = canvas.model.marks[mark]
    offset = 1 if pressed else 0
    x, y = button[0] + left + offset, button[1] + top + offset
    for row in range(height):
        column = 0
        while column < width:
            if not (rows[row] >> (width - 1 - column)) & 1:
                column += 1
                continue
            start = column
            while column < width and (rows[row] >> (width - 1 - column)) & 1:
                column += 1
            canvas.fill(clip, x + start, y + row, column - start, 1, c['Glyph'])


class _TextRun:
    def __init__(self, canvas: Canvas, clip: Rect, x: int, y: int, limit: int):
        self.canvas, self.clip, self.x, self.y, self.limit, self.column = canvas, clip, x, y, limit, 0

    def write(self, text: bytes, foreground: Color, background: Color) -> None:
        for character in text.decode('utf-8', errors='replace'):
            code = self.canvas.model.glyphs.get(ord(character), self.canvas.model.replacement)
            if self.column >= self.limit:
                return
            self.canvas.glyph(self.clip, self.x + self.column * GLYPH_WIDTH, self.y, code, foreground, background)
            self.column += 1


def paint_frame(canvas: Canvas, clip: Rect, window: Window) -> None:
    """Decorations.cpp's PaintFrame."""
    model = canvas.model
    c, k = model.colors, model.constants
    area = intersect(intersect((0, 0, canvas.width, canvas.height), clip), window.frame)
    if empty(area):
        return
    x, y, w, h = window.frame
    canvas.bevel(area, window.frame, *_window_bevel(model))
    f = k['FrameWidth']
    canvas.fill(area, x + 2, y + 2, w - 4, f - 2, c['Face'])
    canvas.fill(area, x + 2, y + h - f, w - 4, f - 2, c['Face'])
    canvas.fill(area, x + 2, y + f, f - 2, h - 2 * f, c['Face'])
    canvas.fill(area, x + w - f, y + f, f - 2, h - 2 * f, c['Face'])
    canvas.fill(area, x + f, y + f + k['TitleBarHeight'], w - 2 * f, k['SeparatorHeight'], c['Face'])

    bar = model.title_bar_rect(window.frame)
    bar_area = intersect(area, bar)
    if empty(bar_area):
        return
    unfocused = window.state == 'unfocused'
    background = {'focused': c['ActiveTitle'], 'attention': c['AttentionTitle']}.get(window.state, c['InactiveTitle'])
    text = c['InactiveTitleText'] if unfocused else c['ActiveTitleText']
    subtitle = c['InactiveSubtitle'] if unfocused else c['ActiveSubtitle']
    canvas.fill(bar_area, bar[0], bar[1], bar[2], bar[3], background)

    maximize = model.maximize_button_rect(window.frame)
    text_left = bar[0] + k['TitleTextInset']
    text_right = maximize[0] - k['TitleTextGap']
    columns = (text_right - text_left) // GLYPH_WIDTH if text_right > text_left else 0
    if model.ui_faces is not None:
        title = model.title_text(window)
        trusted = len(window.name[:k['TitleNameBytes']]) + (4 if window.volume else 0)
        suffix = len(model.strings['NotRespondingMark']) if window.not_responding else 0
        _paint_ui(canvas, bar_area, (text_left, bar[1] + k['TitleTextTop'], max(0, text_right-text_left), model.ui_constants['UiTextHeight']),
                  title, True, trusted, suffix, text, subtitle, background)
    else:
        run = _TextRun(canvas, bar_area, text_left, bar[1] + k['TitleTextTop'], columns)
        if window.volume:
            run.write((chr(k['VolumeCodePoint']) + ' ').encode('utf-8'), text, background)
        run.write(window.name[:k['TitleNameBytes']], text, background)
        if window.subtitle:
            run.write(model.strings['SubtitleMark'], subtitle, background)
            run.write(sanitize_subtitle(window.subtitle[:k['SubtitleBytes']], k), subtitle, background)
        if window.not_responding:
            run.write(model.strings['NotRespondingMark'], text, background)

    _paint_button(canvas, bar_area, maximize, window.maximize_pressed,
                  'RestoreGlyph' if window.maximized else 'MaximizeGlyph')
    _paint_button(canvas, bar_area, model.close_button_rect(window.frame), window.close_pressed, 'CloseGlyph')


def paint_window(canvas: Canvas, clip: Rect, window: Window) -> None:
    """Scene.cpp's PaintWindow."""
    if empty(intersect(window.frame, clip)):
        return
    paint_frame(canvas, clip, window)
    model = canvas.model
    content = model.content_rect(window.frame)
    area = intersect(content, clip)
    if empty(area):
        return
    image = window.image
    width = min(image.width, content[2]) if image else 0
    height = min(image.height, content[3]) if image else 0
    if image:
        canvas.copy(area, image, content[0], content[1], width, height)
    face = model.colors['Face']
    canvas.fill(area, content[0] + width, content[1], content[2] - width, content[3], face)
    canvas.fill(area, content[0], content[1] + height, width, content[3] - height, face)


def paint_outline(canvas: Canvas, clip: Rect, outline: Rect) -> None:
    area = intersect(intersect((0, 0, canvas.width, canvas.height), clip), outline)
    if empty(area):
        return
    x, y, w, h = outline
    line, color = canvas.model.constants['OutlineWidth'], canvas.model.colors['DarkShadow']
    canvas.fill(area, x, y, w, line, color)
    canvas.fill(area, x, y + h - line, w, line, color)
    canvas.fill(area, x, y, line, h, color)
    canvas.fill(area, x + w - line, y, line, h, color)


def paint_switcher(canvas: Canvas, clip: Rect, text: bytes) -> None:
    model = canvas.model
    c, k = model.colors, model.constants
    text = text[:model.title_text_bytes]
    suffix = len(model.strings['NotRespondingMark']) if text.endswith(model.strings['NotRespondingMark']) else 0
    if model.ui_faces is None:
        box = model.switcher_rect(canvas.width, canvas.height, len(text.decode('utf-8', errors='replace')))
    else:
        natural, _ = _ui_masks(model.repository, model.ui_faces, 0, text, False, len(text), 0)
        limit = canvas.width-k['SwitcherMargin'] if canvas.width > k['SwitcherMargin'] else canvas.width
        width = min(natural+2*k['SwitcherPadding'], limit)
        box = ((canvas.width-width)//2, max(0, (canvas.height-k['SwitcherHeight'])//2), width, k['SwitcherHeight'])
    area = intersect(intersect((0, 0, canvas.width, canvas.height), clip), box)
    if empty(area):
        return
    canvas.bevel(area, box, *_window_bevel(model))
    canvas.fill(area, box[0] + 2, box[1] + 2, box[2] - 4, box[3] - 4, c['Face'])
    columns = (box[2] - 2 * k['SwitcherPadding']) // GLYPH_WIDTH if box[2] > 2 * k['SwitcherPadding'] else 0
    if model.ui_faces is not None:
        _paint_ui(canvas, area, (box[0]+k['SwitcherPadding'], box[1]+k['SwitcherTextTop'],
                  max(0, box[2]-2*k['SwitcherPadding']), model.ui_constants['UiTextHeight']), text, False, len(text), suffix, c['Glyph'], c['Glyph'], c['Face'])
    else:
        _TextRun(canvas, area, box[0] + k['SwitcherPadding'], box[1] + k['SwitcherTextTop'], columns).write(
            text, c['Glyph'], c['Face'])



def render(model: SessionModel, width: int, height: int, windows: list[Window],
           switcher: bytes | None = None, outline: Rect | None = None) -> bytes:
    """The whole output as packed RGB24 (Scene::RenderAll); windows bottom first."""
    canvas = Canvas(model, width, height)
    output = (0, 0, width, height)
    canvas.fill(output, 0, 0, width, height, model.colors['Desktop'])
    for window in windows:
        paint_window(canvas, output, window)
    if outline is not None:
        paint_outline(canvas, output, outline)
    if switcher is not None:
        paint_switcher(canvas, output, switcher)
    return bytes(canvas.rgb)


def pattern_image(seed: int, width: int, height: int) -> Image:
    """The corpus's test pattern; SceneCorpusTests.cpp computes the same."""
    rgb = bytearray(width * height * 3)
    at = 0
    for y in range(height):
        for x in range(width):
            rgb[at] = (x * 3 + seed * 7) & 255
            rgb[at + 1] = (y * 5 + seed * 11) & 255
            rgb[at + 2] = ((x ^ y) + seed * 13) & 255
            at += 3
    return Image(width, height, bytes(rgb))


def solid_image(color: Color, width: int, height: int) -> Image:
    return Image(width, height, bytes(color) * (width * height))


def _number(token: RenderCorpus.Token, line: int) -> int:
    try:
        return int(str(token), 0)
    except ValueError as error:
        raise RenderCorpus.CorpusError('line %d: %r is not a number' % (line, token)) from error


def render_case(model: SessionModel, case: RenderCorpus.Case, terminal_images=None) -> tuple[int, int, bytes]:
    """A `scene` case of the corpus: (width, height, RGB24). terminal_images,
    when given, maps a terminal corpus case name to its rendered Image."""
    if len(case.arguments) != 3:
        raise RenderCorpus.CorpusError('scene %s: expected <width> <height> <format>' % case.name)
    width, height = _number(case.arguments[0], 0), _number(case.arguments[1], 0)
    model.ui_faces = (True, True)
    windows: list[Window] = []
    switcher: bytes | None = None
    outline: Rect | None = None
    for line, tokens in case.statements:
        verb = tokens[0]
        if verb == 'window':
            if len(tokens) >= 3 and tokens[1] == 'maximized':
                frame, state, flags = model.maximized_frame(width, height), str(tokens[2]), tokens[3:]
                maximized = True
            else:
                frame = tuple(_number(token, line) for token in tokens[1:5])
                state, flags, maximized = str(tokens[5]), tokens[6:], False
            if state not in ('focused', 'unfocused', 'attention'):
                raise RenderCorpus.CorpusError('line %d: unknown state %s' % (line, state))
            window = Window(frame=frame, state=state, maximized=maximized)
            for flag in flags:
                if flag == 'notresponding':
                    window.not_responding = True
                elif flag == 'volume':
                    window.volume = True
                elif flag == 'close-pressed':
                    window.close_pressed = True
                elif flag == 'maximize-pressed':
                    window.maximize_pressed = True
                else:
                    raise RenderCorpus.CorpusError('line %d: unknown flag %s' % (line, flag))
            windows.append(window)
        elif verb == 'font':
            model.ui_faces = (False, False) if tokens[1] == 'fallback' else (True, True)
        elif verb == 'title':
            windows[-1].name, windows[-1].subtitle = bytes(tokens[1]), bytes(tokens[2])
        elif verb == 'image':
            kind = tokens[1]
            if kind == 'pattern':
                windows[-1].image = pattern_image(*(_number(token, line) for token in tokens[2:5]))
            elif kind == 'solid':
                values = [_number(token, line) for token in tokens[2:7]]
                windows[-1].image = solid_image(tuple(values[:3]), values[3], values[4])
            elif kind == 'terminal':
                if terminal_images is None or str(tokens[2]) not in terminal_images:
                    raise RenderCorpus.CorpusError('line %d: no terminal case %s' % (line, tokens[2]))
                windows[-1].image = terminal_images[str(tokens[2])]
            elif kind == 'none':
                windows[-1].image = None
            else:
                raise RenderCorpus.CorpusError('line %d: unknown image %s' % (line, kind))
        elif verb == 'switcher':
            switcher = bytes(tokens[1])
        elif verb == 'outline':
            outline = tuple(_number(token, line) for token in tokens[1:5])
        else:
            raise RenderCorpus.CorpusError('line %d: unknown statement %s' % (line, verb))
    return width, height, render(model, width, height, windows, switcher, outline)


def sanitize_subtitle(text: bytes, constants: dict[str, int]) -> bytes:
    return ''.join(chr(constants['SubtitleReplacement']) if (ord(point) < 0x20 or 0x7F <= ord(point) <= 0x9F
                    or ord(point) == constants['VolumeCodePoint']) else point
                   for point in text.decode('utf-8', errors='replace')).encode('utf-8')


def probe_output(repository: pathlib.Path, width: int, height: int, serial: int) -> bytes:
    """desktop session probe: sub-screen opaque output over the console background."""
    text = (repository / 'Tests/Guest/SessionProbe.hpp').read_text()
    ow = RenderCorpus.header_constant(text, 'ProbeWidth')
    oh = RenderCorpus.header_constant(text, 'ProbeHeight')
    ww = RenderCorpus.header_constant(text, 'ProbeWindowWidth')
    wh = RenderCorpus.header_constant(text, 'ProbeWindowHeight')
    _, background = Display.console_colors(repository)
    desktop = RenderCorpus.header_color(RenderCorpus.header_text(repository, PALETTE_HEADER), 'Desktop')
    rgb = bytearray(bytes(background) * (width * height))
    for y in range(min(height, oh)):
        for x in range(min(width, ow)):
            color = desktop
            if 48 <= x < 48 + ww and 32 <= y < 32 + wh:
                sx, sy = x - 48, y - 32
                color = ((sx + serial * 17) & 255, (sy * 2 + serial * 11) & 255,
                         ((sx ^ sy) + serial * 23) & 255)
            at = (y * width + x) * 3
            rgb[at:at + 3] = bytes(color)
    return Display.draw_cursor(bytes(rgb), width, height, (96, 72), repository)


def client_pattern_image(width: int, height: int, serial: int, seed: int) -> Image:
    """pattern client: serial 2 copies serial 1 and damages 24x18."""
    header = (pathlib.Path(__file__).resolve().parents[3] / 'Tests/Guest/PatternClient.hpp').read_text()
    pw = RenderCorpus.header_constant(header, 'PatternPatchWidth')
    ph = RenderCorpus.header_constant(header, 'PatternPatchHeight')
    rgb = bytearray(width * height * 3)
    for y in range(height):
        for x in range(width):
            frame = 1 if serial == 2 and (x >= pw or y >= ph) else serial
            at = (y * width + x) * 3
            rgb[at:at + 3] = bytes(((x + frame * 17 + seed * 31) & 255,
                                   (y * 2 + frame * 11 + seed * 7) & 255,
                                   ((x ^ y) + frame * 23 + seed * 13) & 255))
    return Image(width, height, bytes(rgb))


def transcript_output(repository: pathlib.Path, width: int, height: int, lines: list[str],
                      cursor: tuple[int, int]) -> bytes:
    """Replay the last complete Presented snapshot; incomplete passes are errors."""
    model = SessionModel(repository)
    model.ui_faces = None
    windows: list[Window] = []
    complete: list[Window] | None = None
    output: tuple[int, int] | None = None
    pending = None
    by_slot = {}
    for line in lines:
        if not line.startswith('# session: '):
            continue
        tokens = line[len('# session: '):].split()
        if tokens[0] == 'font':
            if len(tokens) != 3 or tokens[1] not in ('Vegur-Bold', 'Bold8x16-fallback') or tokens[2] not in ('Vegur-Regular', 'Bold8x16-fallback'):
                raise ValueError('invalid UI font transcript')
            model.ui_faces = tokens[1] == 'Vegur-Bold', tokens[2] == 'Vegur-Regular'
        elif tokens[0] == 'begin':
            if pending is not None or len(tokens) != 4:
                raise ValueError('incomplete session snapshot')
            pending, ow, oh = map(int, tokens[1:])
            output = ow, oh
            windows, by_slot = [], {}
        elif tokens[0] == 'window':
            if pending is None or len(tokens) not in (11, 12):
                raise ValueError('window outside snapshot')
            slot, x, y, w, h, state, klass, iw, ih, serial = map(int, tokens[1:11])
            flags = int(tokens[11]) if len(tokens) == 12 else 0
            if slot in by_slot or state not in (0, 1, 2) or klass not in (1, 2):
                raise ValueError('invalid session window')
            window = Window((x, y, w, h), volume=klass == 2,
                            state=('unfocused', 'focused', 'attention')[state],
                            not_responding=bool(flags & 1), maximized=bool(flags & 2),
                            close_pressed=bool(flags & 4), maximize_pressed=bool(flags & 8),
                            image=client_pattern_image(iw, ih, serial, slot // 4 + 1))
            by_slot[slot] = window
            windows.append(window)
        elif tokens[0] == 'title':
            if pending is None or len(tokens) != 4 or int(tokens[1]) not in by_slot:
                raise ValueError('title outside snapshot')
            window = by_slot[int(tokens[1])]
            window.name, window.subtitle = (b'' if value == '-' else bytes.fromhex(value)
                                           for value in tokens[2:])
        elif tokens[0] == 'presented':
            if pending is None or int(tokens[1]) != pending or len(tokens) != 6:
                raise ValueError('Presented does not match snapshot')
            if any(not window.name for window in windows):
                raise ValueError('window missing trusted title')
            complete, pending = windows, None
    if pending is not None or complete is None or output is None:
        raise ValueError('missing complete Presented snapshot')
    ow, oh = output
    if ow > width or oh > height:
        raise ValueError('session exceeds scanout')
    session = render(model, ow, oh, complete)
    _, background = Display.console_colors(repository)
    rgb = bytearray(bytes(background) * width * height)
    for y in range(oh):
        rgb[y * width * 3:(y * width + ow) * 3] = session[y * ow * 3:(y + 1) * ow * 3]
    return Display.draw_cursor(bytes(rgb), width, height, cursor, repository)


def complete_transcript(lines):
    """Keep the latest complete snapshot, including its pointer/event fences."""
    first = None
    font = None
    selected_font = None
    result = []
    for index, line in enumerate(lines):
        if line.startswith('# session: font '):
            font = line
        if line.startswith('# session: begin '):
            first = index
            selected_font = font
        if line.startswith('# session: presented ') and first is not None:
            result = ([selected_font] if selected_font is not None else []) + lines[first:index + 1]
    return result
