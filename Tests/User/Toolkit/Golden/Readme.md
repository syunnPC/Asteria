<!-- SPDX-License-Identifier: Apache-2.0 -->

# Toolkit rendering goldens

P6 RGB images test project-authored controls using the session `Palette` and
[Vegur fonts](../../../../Data/Fonts/Vegur/NOTICE.md). State atlases use six columns:
normal, hot, pressed, focused, disabled and checked/selected. The 29 rows cover
controls, containers, menus, tooltips and multiline/password text boxes.
States without distinct visuals repeat the default appearance.

The fallback atlas checks Unicode-to-CP437 mapping; the Vegur atlas checks font
rendering and missing-glyph fallback. `TextScroll`, `ComboPopup` and `MenuPopup`
cover scrolling/overlays. `DemoForm`, `MessageBox` and mock-backed `FileDialog`
cover window/dialog composition. Separate pixel assertions check clipping,
RGBA/BGRA scaling, theme replacement and layout extents.

`RenderingTests`/`ExtendedTests` compare all RGB channels, dimensions and EOF.
Normal tests never overwrite goldens. To regenerate after intentional changes:

```sh
Build/CMake/host-debug/Tests/User/Toolkit/RenderingTests --record
Build/CMake/host-debug/Tests/User/Toolkit/ExtendedTests --record
```

Review bevels, indicators, disabled text, selection, clipping, carets and geometry
before accepting updates. Goldens do not claim compatibility with Windows pixels.
