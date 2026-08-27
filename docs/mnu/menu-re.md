# MNU/MNS menu UI: engine correspondence and equivalence

How Joint Operations parses, lays out, sounds, and draws its `.mnu` menus, as
witnessed in the original engine, and how `engine/formats/mnu` (incl. the mnu_xml reader), `engine/formats/mns`,
and `godot/src/mnu` correspond to it.

Reverse-engineered from `Jointops.exe` (Joint Operations: Combined Arms, imagebase
`0x400000`, IDB `Jointops.exe.kong.i64`). All addresses are absolute in that image.
This is the menu-slice grill (2026-06-09), closing the render/sound divergences the
2026-06-01 format pass deferred. The 2026-06-23 render grill added the coordinate system,
per-item image/color rendering, spinlist arrows, the combo dropdown, the marquee CBIN
credits, the monogram (parsed-but-not-drawn), and the `DRAW_FRAME` frame-draw gate — see
the sections below. The 2026-08-09 draw-walk grill witnessed the complete render
dispatch (the scene walk, the per-widget Draw vtable, the per-state appearance records,
the visual-state vocabulary and pump, the text/edit/caret path, and the cursor draw) for
the ADR 0033 R2 menu compiler — see "Widget render dispatch".

---

## Parse pipeline

`UIScene_LoadAndParseContent @ 0x63c830` loads the `.mnu` bytes, strips a UTF-8 BOM,
expands `%VAR%` over the WHOLE buffer (`NapiXML_ExpandVariablesInText @ 0x63a000`),
then SAX-parses the UTF-16 tag stream (`XML_ParseWithBOMDetection @ 0x76a690`). Tag
literals are wide; the element handler is `CUIElement_ParseXMLDefinition @ 0x648120`,
and the type factory is `CUIScene_CreateWidgetByType @ 0x64f630`.

| Concern | Original | OpenNova |
|---|---|---|
| Element parse | `CUIElement_ParseXMLDefinition @ 0x648120` | `engine/formats/mnu` parse; layout in `MenuFrameCompiler::solve_rect` (`engine/runtime/menu/menu_frame.cpp`) |
| Type factory | `CUIScene_CreateWidgetByType @ 0x64f630` | `mnu::parse_window_type` / `window_type_name` |
| Scene node attrs | `parse_scene_node_attributes @ 0x639630` | `mnu::Screen` (NA/MU + WI children) |
| `%VAR%` expand | `NapiXML_ExpandVariablesInText @ 0x63a000` | `substitute_var` per field (ADR 0005) |
| XML entities | `XML_ParseCharEntity @ 0x769cc0`, table @ 0x85a628 | `decode_entity` in `mnu_xml.cpp` |
| Char entity emit | (table @ 0x85a628, 4 decodable) | `escape_xml` in `mnu.cpp` |

### Widget types `[orig: @ 0x64f630]`

Full-word `wcsicmp` on `TYPE`. Tokens: `STATIC BUTTON SCROLL EDIT MULTILINE_EDIT
RADIO LIST SPINLIST CHECKBOX TABLE GLB_TABLE RADIOEDIT MARQUEE_WND COMBOBOX LAN_LIST
GOPHER`; an unknown token builds a generic `CWnd` (children + attrs still parsed).
`mnu::WindowType` now models all 16 tokens; an unmodelled token is preserved verbatim
on `Window::type_token` so it round-trips instead of degrading to `"unknown"`.

### Authored Actions `[orig: CUIElement_ParseXMLDefinition @ 0x648ee2; CUIWidget_HandleScriptedAction @ 0x6497f0]`

The parser assigns sixteen authored type codes in this order: `SCREEN`, `WINDOW`,
`URL`, `FORM_POST`, `GLB_LOAD`, `GLB_LOADANDPING`, `GLB_FILTER`,
`GLB_FILTER_NUM`, `GLB_PING`, `GLB_JOIN`, `TAB`, `POP_SCREEN`, `APPMSG`,
`LAN_SEARCH`, `LAN_JOIN`, and `MNX` (`@ 0x648f4a..0x6490e9`). The Action also
preserves `FILE`, `FIELD`, `SOURCE`, `TARGET_FORM`, `EXTERNAL_BROWSER`,
`TOGGLE`, and `TEST=LT|LE|EQ|GE|GT`. `WINDOW` states are `HIDE`, `SHOW`,
`ENABLE`, and `DISABLE`; `TOGGLE` inverts the chosen shown/enabled property
(`[orig: state switch @ 0x6498f7..0x6499c9]`).

The dispatcher handles screen selection, Window state, URL, form submission,
the GLB/LAN/application shell operations, and pop. Type `TAB` is a special
focus-target action on the form event subtype (`@ 0x649c17..0x649c55`), not
the common menu convention called a Tab; shipped Tabs are sibling panels
shown/hidden by ordinary `WINDOW` Actions. The committed JO fixtures use only
`SCREEN`, `WINDOW`, `URL`, and `POP_SCREEN`, but the remaining authored types
are still format data and must round-trip and remain available to their hosts.
`QUIT` is not a parsed Action type; exit buttons are shell Commands bound by
Window name (ADR 0001).

## Layout `[orig: @ 0x648120 parse tail; adjust_rect_to_text_size @ 0x6575f0]`

POSITION is absolute parent-relative edges (`LEFT/TOP/RIGHT/BOTTOM`, with
`ULX/ULY/WIDTH/HEIGHT` aliases folding to the same fields); missing edges read 0.
Three stages, no per-type default sizes:

1. The authored rect.
2. A degenerate axis (`right<=left` / `bottom<=top`) falls back to the largest
   IMAGE appearance: width from the texture, height from the `HEIGHT` attr (sprite
   frame height) else the texture.
3. Text widgets (the vtable family sharing the text-widget parse `@ 0x657c30` ->
   `adjust_rect_to_text_size @ 0x6575f0`) size a still-degenerate axis from the
   measured string, anchored per `JUSTIFY`/`VJUSTIFY` (left/centre/right edge;
   top/centre/bottom).

The element texture is stretched INTO the resulting rect
(`CUIElement_DrawStretchedTexture @ 0x647d40`). A plain IMAGE uses UV 0..1;
when `MAP_STATE` and `HEIGHT` are authored, `MAP_STATE` selects that
`HEIGHT`-tall source row and only that band is stretched into the destination
`[orig: CUIElement_ParseXMLDefinition @ 0x648120;
CUIElement_DrawTextureNative @ 0x647e40 -> CTextureManager_DrawScaledRect
@ 0x654e60]`. Reimpl: `MenuFrameCompiler::solve_rect` uses the cropped HEIGHT
as the image extent, and `emit_state_texture` carries the same row as normalized
UVs; checkboxes stretch like every other element (the old keep-aspect was a
reference-repo choice). With no resolvable font the compiler measures a nominal
8x16 glyph box so headless layout stays deterministic.

## Frame `[orig: CUIElement_DrawFrame @ 0x64a210; init_border_materials @ 0x646f70]`

The stencil is a grid of `SIZE x SIZE` tiles (canonically 4*SIZE square):
row 0 = top corners + top edge (+ fill tile at col 3), row 1 = left/right edges,
row 2 = bottom row. The drawer paints a center fill quad (the BRUSH, tiled) plus 8
INDEPENDENT border pieces: corners at `SIZE`, edges stretched between them, the whole
border hanging OUTSIDE the window rect by `SIZE` and pulled back by the authored
`STENCIL INSETX/INSETY` (floats at elem+0x288/+0x28C, default 0). Every quad is
modulated by `0x7F7F7F`, which is neutral in retail's modulate-2x fixed-function
path and therefore produces no visible tint `[orig: CUIElement_DrawFrame
@ 0x64a210; init_border_materials @ 0x646f70]`. Reimpl:
`MenuFrameCompiler::emit_frame` emits effective white (`0xFFFFFFFF`) for the
Godot ordinary-multiply applier, preserving the retail result rather than
darkening the BRUSH and eight STENCIL quads to half intensity.
The old 4x4 mirrored-corner NinePatch bake with hardcoded 16/24 insets is retired.

A frame draws ONLY when the window's `DRAW_FRAME` flag is set: the render gate is
`if (elem+0x134) DrawFrame(...)` in `CStaticWnd_Render @ 0x657b10`, evaluated BEFORE the
ungated appearance/texture passes — so a window's own `<APPEARANCE>` image still draws when
DRAW_FRAME is clear. A window may carry a `<FRAME>` purely to hand its stencil/brush down to
framed descendants without drawing one itself: the shipped `jo_game.mnu` / `jo_options.mnu`
root `MAIN` defines the camo `BOXTILE` brush + `BORDER2` stencil but has NO DRAW_FRAME, so it
draws no frame, while its `MAIN_WRAPPER` / `OPTIONS_WRAPPER` children carry DRAW_FRAME and
draw the inherited frame. Reimpl: the compiler's draw walk gates ALL frame drawing (own and
inherited) on `w.draw_frame`. The old reimpl drew a window's own `<FRAME>` unconditionally,
which tiled the camo brush across the whole 800x600 root — the full-window camo behind the
in-game ESC menu (and under `letterbox.tga` on the options screen); fixed.

When NEITHER the stencil nor the brush texture resolves, the original draws nothing:
every draw in `CUIElement_DrawFrame` is guarded by a successful texture load
(`sub_654370 >= 0`). The reimpl matches this at runtime (no panel). The former
ONED placeholder was removed with menu authoring. The old opaque dark
ColorRect fallback (the "big black box" on the in-game ESC menu when `BORDER2.tga`/
`BOXTILE.tga` were absent) is retired.

### MONOGRAM is parsed but NOT drawn

`<FRAME><MONOGRAM>` is parsed and round-tripped, but the shipped engine never renders
the menu monogram. The window render path draws frame + appearance(s) + text + children
only `[orig: CStaticWnd_Render @ 0x657b10]`, and `CUIElement_DrawFrame @ 0x64a210` has no
monogram pass. The only `monogram.tga` reference in the binary is the loading screen
(`Game_StartMission @ 0x525aa3`), not menus. The earlier reimpl heuristic (a centered
`Monogram` `TextureRect`) produced a stray glyph in the middle of every framed panel and
is removed. The deferred "subtype 0x600" note below is resolved: there is no menu
monogram draw to witness.

## Coordinate system / scale `[orig: CUIScene_SetScreenScale @ 0x639480]`

Menus are authored in a **fixed 800x600 virtual design space** and scaled to the actual
back-buffer with **independent X/Y factors (anamorphic fill)** — no aspect preservation,
no letterbox bars, origin (0,0). On a widescreen display the 4:3 menu is stretched
horizontally, as in the retail game. The scene scale is computed on every video-mode
change:

```
scene.scaleX = screenWidth  * 0.00125      // = 1/800   [orig: CUIScene_SetScreenScale @ 0x639480]
scene.scaleY = screenHeight * 0.0016666667 // = 1/600
```

and propagated to every widget by `CWnd_SetScaleRecursive @ 0x646c60` (writes
`elem+264`=scaleX, `elem+268`=scaleY, recursing to children); the caller is
`apply_video_mode_change @ 0x55a590`, which passes the real new resolution. At draw, each
element rect is multiplied by the scale and truncated to int
(`CUIElement_DrawStretchedTexture @ 0x647d40`), with ancestor offsets accumulated up the
parent chain (`CWnd_AccumulateAncestorOffset @ 0x6465e0`).

Reimpl: the menu-root CanvasItem is given an 800x600 box and a non-uniform
`scale = (screenW/800, screenH/600)`, position 0 — authored coords stay in 800x600 design
space (`menu_shell.gd::_recompute_fit`). The old reimpl
used a hardcoded 640x480 board with uniform letterbox + centering, which overhung and
mis-centered the 800x600 `jo_game.mnu` (the badly-placed ESC menu) and letterboxed every
menu. `D-MNU-4`: the original truncates each scaled quad to int per element; the reimpl
applies one float CanvasItem scale, a sub-pixel divergence (accepted).

## Widget render dispatch (2026-08-09 draw-walk grill)

The complete per-frame menu draw, witnessed end to end for the ADR 0033 R2 menu
compiler (`engine/runtime/menu` `MenuFrameCompiler` — the reimpl for everything
in this section).

**Frame entry.** The menu is a game MODE (struct `@ 0x83b404`): init =
`Menu_InitShellResources @ 0x552500`, update = `Menu_UpdateFrame @ 0x5528a0`
(nav push, message pump, `scene_end_frame` on `g_GameMenu @ 0x2551100`, LAN/
preview/admin pumps), render = `Menu_RenderFrame @ 0x54b7c0`: device-lost
handling, gate on `g_GameMenu` + `g_menu_render_dirty @ 0x2551114`, clear,
BeginScene, Bink update, `CUIScene_DrawScreensAndCursor @ 0x63bf60`, Present.

**Scene walk** `[orig: CUIScene_DrawScreensAndCursor @ 0x63bf60]`: every screen
in the scene container (`scene+20`: `{+4 array, +8 count}`) draws via vtable+24
in FORWARD index order (each screen gates on its own shown flag; the input pump
`scene_end_frame @ 0x63e600` walks the same array in REVERSE, so front-most =
last drawn = first picked). Then the MOUSE CURSOR: texture = the override
`@ 0x31C16E4`, else `g_ui_frame_cursor_texture @ 0x31C16E0` (stamped per frame
by `widget_process_mouse_event` from the hovered widget's inherited `+276`
cursor, else the screen default), drawn at the raw mouse position at native
texture size with **scale 1.0 — the cursor never scales with the menu** —
white, or `0xFF7F7F7F` under `g_ui_half_bright_mode @ 0x31C3760`.

**Draw vtable (+24) per widget type** (vtables via
`CUIScene_CreateWidgetByType @ 0x64f630` ctors):

| Type(s) | Draw | Order |
| --- | --- | --- |
| generic CWnd, GLB_TABLE, GOPHER | `CUIElement_Draw @ 0x64a8a0` | appearance -> frame -> children (note: appearance BEFORE frame) |
| STATIC, BUTTON | `CStaticWnd_Render @ 0x657b10` | frame -> appearance -> text -> children |
| RADIO | `CRadioWnd_Render @ 0x656e20` | checked (+772) forces state 3 around the WHOLE static render — appearance AND label colors |
| CHECKBOX | `CCheckWnd_Render @ 0x64ae20` | frame -> appearance (checked forces slot 3 DIRECTLY, no fallback — an unauthored selected slot has zero flags and draws nothing) -> label (raw state colors) -> children |
| EDIT | `CEditWnd_Render @ 0x6619e0` | see the edit paragraph |
| MULTILINE_EDIT | `CMEditWnd_Render @ 0x6608e0` | frame -> appearance (NO focus forcing, unlike the single-line edit) -> the wrapped drawer -> children (the embedded scrollbar); see the multiline paragraph below |
| LIST, MULTI, LAN_LIST | `CListWnd_DrawItems @ 0x643f30` | frame -> appearance -> rows -> children (scrollbar) |
| SPINLIST | `CSpinListWnd_Render @ 0x64b220` | item cell (below); arrows are child windows |
| COMBOBOX | `CComboWnd_Render @ 0x65bfd0` | closed cell, then children in array order |
| TABLE | `CUITable_Render @ 0x6411d0` | see the table paragraph below (an earlier revision cited `0x6410d0`, a transcription slip — that address is inside the rule-line helper) |
| SCROLL | `CScrollWnd_Render @ 0x64c5c0` | frame -> COLOR/OUTLINE on the full rect + IMAGE on the inset middle track -> child BUTTONs in shuttle/up/down painter order (constructed by `CUIScrollbar_CreateChildWindows @ 0x64d330`) |
| MARQUEE | `CMarqueeWnd_Render @ 0x65cf90` | frame -> appearance -> the credits scroller (below) -> children |
| RADIOEDIT | `@ 0x65d310` | interior not yet walked (D-MNU-13) |

**Per-state appearance records.** Four 28-byte state records interleaved from
`elem+8` (state index picks `elem + 28*state`): flags at `+8` (bit0 COLOR,
bit1 IMAGE, bit2 CURSOR/custom, bit3 OUTLINE — the parse tokens `@ 0x648382..`),
COLOR fill color at `+12`, OUTLINE color at `+16`, IMAGE texture at `+20`,
image draw flags at `+24`. Pass order per state: COLOR fill (stretched quad,
`CUIElement_DrawStretchedTexture @ 0x647d40` with the entry color), IMAGE
(`CUIElement_DrawTextureNative @ 0x647e40` -> `CTextureManager_DrawScaledRect
@ 0x654e60` — STRETCHED into the scaled element rect; "native" size applies
only when a caller passes a texture-sized rect, e.g. the spinlist image cell;
white or half-bright gray). For an authored IMAGE atlas, `MAP_STATE` is the
zero-based row and `HEIGHT` is its source-band height; the compiler preserves
that pair through state selection and emits `v0 = MAP_STATE*HEIGHT/textureH`,
`v1 = (MAP_STATE+1)*HEIGHT/textureH` before stretching `[orig:
CUIElement_ParseXMLDefinition @ 0x648120; CUIElement_DrawTextureNative
@ 0x647e40 -> CTextureManager_DrawScaledRect @ 0x654e60]`. OUTLINE
(`CUIElement_DrawOutlineRect @ 0x647fc0` —
four 1px lines, top edge to right-1, gated on height), CUSTOM
(`CUIElement_DispatchCustomDrawEvent @ 0x647f10` — event 1 with the scaled
rect through the vtable+28 sink; shell-owned, the compiler emits nothing).
Note the event ids: the appearance CUSTOM pass fires event **1**; the table
custom CELL fires **0x8000002** (both `push 0x8000002` sites live inside
`CUITable_Render @ 0x6411d0`) — same vtable+28 sink, different ids, routed to
handlers purely by the callback class mask (`1 << HIBYTE(eventId)`: event 1 →
bit0, table cells → bit8; see "The custom-draw appearance hook" below).

**Visual state vocabulary** — the same indices everywhere: **0 = DEFAULT,
1 = DISABLED, 2 = MOUSEOVER, 3 = SELECTED/pressed**. Witnessed three ways:
the APPEARANCE STATE attribute parse (`@ 0x6483d4..0x64845e`), the runtime
setter `CWnd_SetVisualState @ 0x646340` (writes `+232`/`+236`; availability
mask at `+4` — a state with no authored appearance falls back to 0, and with
no default appearance to −1 = no appearance pass at all, text keeps default
colors), and the FONT color-pair switch in `draw_text_with_cursor @ 0x6533b0`
(case 1 -> pair [6,7] disabled, 2 -> [2,3] mouseover, 3 -> [4,5] selected,
else [0,1] default — pairs in FONT parse order). The per-frame writer is
`widget_process_mouse_event @ 0x647a00` (vtable+20): disabled (`+228 == 0`)
-> 1; hit + button down -> 3; hit + button up -> 2; else 0; front-most sibling
wins via the reverse child walk + the per-frame claim `scene+16`; the hovered
widget lands in `g_ui_mouseover_wnd @ 0x31C16DC`. (The transient sound states
MOUSEIN=1/MOUSEOUT=2/SELECTED=3 at `+240` are a separate vocabulary.)

**Text pass** `[orig: CStaticWnd_DrawLabel @ 0x656fb0]` (static family; button
and radio inherit it): the effective font + 8 colors resolve at DRAW time via
`CWnd_GetFontAndColors @ 0x646a70` (vtable+64) — self-then-parent walk to the
first widget with a nonzero font handle (`+120`), colors at `+124..+152` in
parse order. Layout in DESIGN space at scale 1: avail = rect width − 2×EDGE
(`+756`), < 1 draws nothing; a too-wide string truncates to the largest prefix
below the span (the prefix-measure loop `@ 0x657199`); justify word `+744`
(low nibble 1=center/2=right else left, high 0x10=vcenter/0x20=vbottom else
top) anchors from the (truncated) width; x += EDGE + xoff(`+748`), y +=
yoff(`+752`); the wrap flag `+760` routes to the wrapped drawer
(`draw_text_wrapped_clipped @ 0x653D60` — walked 2026-08-10, see the
multiline paragraph). The sink `font_cache_draw_text_scaled
@ 0x653170` forces alpha 0xFF (unless flags&0x10000), halves the color under
`g_ui_half_bright_mode`, scales the ANCHOR by the element scale pair, and
forwards scaleX/scaleY into `CGameFont_DrawText @ 0x6752c0` (via the cdecl
trampoline `@ 0x676290`) — **menu glyphs scale with the anamorphic 800x600
pair**, so text stretches with the screen like every quad. Only the
foreground color of the selected pair is consumed (BG preservation-only, as
already recorded).

**Edit widgets** `[orig: CEditWnd_Render @ 0x6619e0]`: keyboard focus is
`g_ui_focus_wnd @ 0x31C16D4` (set `UI_SetFocusWnd @ 0x646420`, cleared
`@ 0x646430`, keyboard events route to it in
`dispatch_keyboard_event_to_children @ 0x63ad10`, cleared on screen switch
`@ 0x63b7be` — this closes the 2026-07-16 open item: `0x31C16D4` is the FOCUS
widget, `0x31C16DC` the mouseover widget, per the debug formatter
`@ 0x6394f0`). A focused, non-readonly (`+776`) edit FORCES visual state 2 —
mouseover appearance and colors are the retail "active field" look. PASSWORD
(`+780`) swaps in a cached all-`'*'` mask buffer (`+788`). The caret: char
index `+764`, kept visible by the scroll window (`update_edit_scroll_range
@ 0x661790` over start `+792`/end `+796`, fitting via
`EditWnd_CountCharsFitting @ 0x6616b0`); blinks on a 1024 ms `GetTickCount`
cycle — drawn only while `(tick & 0x3FF) > 0x200`; drawn by
`draw_text_with_cursor @ 0x6533b0` as an UNDERSCORE at the caret char's x
(left-run width + `spacing+1` gap terms), x-stretched to that char's width
(`charW / underscoreW`); the caret at end-of-text measures as `'_'` itself.

**Multiline edit + the wrapped-text drawer** (walked 2026-08-10; the original
class is `_MEditWnd`, vtable `@ 0x7e259c`, derived from CEditWnd —
`CMEditWnd_Construct @ 0x660780`). `CMEditWnd_Render @ 0x6608e0`: shown gate ->
clip -> frame -> the standard appearance passes for the RAW visual state
(**no focus forcing** — the single-line sibling's focused-state-2 rule does
not apply) -> font/colors via vtable+64 -> one call into the wrapped drawer ->
viewport restore -> children. PASSWORD (`+780`) swaps the cached `'*'` mask
(`+788`); the caret (`+764`) draws only while focused, non-readonly, and
`(GetTickCount() & 0x3FF) > 0x200`.

`sub_653D60` is NOT the drawer — it is an arg-marshaling wrapper appending
`flags=0x40000` (bottom clip); renamed `draw_text_wrapped_clipped @ 0x653D60`.
The core is **`draw_text_wrapped @ 0x653710`** (its decompiler parameter names
are historical mislabels — the IDB function comment carries the true mapping).
The wrap loop, exactly: chars accumulate into one static line buffer, the
prefix measured at the WIDGET scale pair against `trunc(wrapW * scaleX)`; a
space memoizes `lastSpace` (index 0 doubles as "none" — a space at index 0
never registers); `accum <= threshold` + LF-or-NUL breaks at the char
(**only `0x0A` breaks — CR is appended, measured, and drawn like any glyph**;
the medit Enter inserts `"\r\n"`); overflow breaks at `lastSpace` (the space
consumed), else at the char exclusively (the overflowing char starts the next
line). Line flush: lines numbered from 1, `lineNo <= firstVisibleLine` lines
are consumed silently with NO y advance; a drawn line measures at scale 1.0
for justify (4=center, 5=right, else left) and the y advance, draws through
`font_cache_draw_text_scaled @ 0x653170` (colorA only, opaque), then the
`0x40000` mode returns when `curY + lineH` would pass the bottom. The caret
pass draws `"|"` centered at `x + accum - w("|")/2` BEFORE the char appends —
it mixes the scaled accumulator into the design-space pen and ignores
line-skipping (both original quirks, preserved). A `0x20000` mode (break at
width then discard the rest of the source line to the next LF, no bottom
clip) serves the TABLE cell path (`calculate_aligned_text_rect @ 0x63ec50` ->
`CTableWnd_DrawCell @ 0x640be0` / `CUITable_Render @ 0x6411d0`); MARQUEE does
not use the drawer.

The vertical scroll is LINE-based: `widget+3912` (`widget[978]`) holds the
first-visible-line count, fed by an embedded `CScrollWnd` child at `+816`
named `"MEDITWND_SCROLL"` (`CMEditWnd_CreateScrollChild @ 0x661260`; default
right-edge strip `(parentW-22, 0, parentW, parentH)`; skinned by `SCROLLBAR`
child nodes forwarded from `CMEditWnd_ParseXmlProperties @ 0x660890`). The
range recomputes ONLY on Init/SetText (`CMEditWnd_UpdateScrollRange
@ 0x661180` — typing does not refresh it, a witnessed staleness quirk) via the
measure twin **`font_cache_count_wrapped_lines @ 0x653b90`** (identical break
rules at scale 1.0): page = `fit-1`, range `[0, total-fit]`
(`CScrollWnd_SetPageSize @ 0x64CE10`, `CScrollWnd_SetRangeAndClamp
@ 0x64D490`), scrollbar hidden when the content fits; scroll events
(`0x4000001`) write `widget[978]` verbatim (`CMEditWnd_HandleEvent
@ 0x660F40`). Keys (`CMEditWnd_HandleKeyEvent @ 0x661020`): Left/Right/Home/
End/Backspace/Delete as the single-line edit; **Enter inserts `"\r\n"`**
(`CMEditWnd_InsertString @ 0x660DC0`); no Up/Down/PgUp/PgDn — vertical
navigation exists only through the scrollbar. Insert paths
(`CMEditWnd_InsertChars @ 0x660C60`) share the read-only/numeric/max-length
gates of the single-line edit.

Reimpl: `MenuFrameCompiler::emit_multiline_edit`/`emit_wrapped_text` +
`multiline_line_counts` (`engine/runtime/menu/menu_frame.cpp`), pinned by
`test_multiline_wrap` in `tests/menu/menu_frame_compiler_test`;
`MenuWidgetState.scroll_row` carries the first-visible-line count. The authored
default-state track/arrows/shuttle and line-derived thumb now compile for its
direct `<SCROLLBAR>` child; a missing/zero-width child rect uses the witnessed
rightmost 22px/full-height fallback and the child stays hidden while all lines
fit `[orig: CMEditWnd_CreateScrollChild @ 0x661260;
CMEditWnd_UpdateScrollRange @ 0x661180; CScrollWnd_SetPageSize @ 0x64CE10;
CScrollWnd_SetRangeAndClamp @ 0x64D490]`. Child-button hover/press and the
multiline edit's own arrow/shuttle interaction remain D-MNU-13 residue (the
pump's row owners are List/Multi/LanList/Table and the open combo popup;
the multiline edit is not wired into scrollbar or wheel interaction — see
"Mouse wheel" below for the witnessed retail wheel dead-end and the
D-MNU-18 reimpl divergence). The block-alignment leg
(the whole-unwrapped-text measure gating v-center/bottom) is not compiled —
no shipped multiline edit authors it.

**List rows** `[orig: CListWnd_DrawItems @ 0x643f30]` (extends the earlier
combo-grill row model): 32-byte item records (text `+0`, STYLE INDEX `+12` —
a 0..3 state slot, −1 none, set by selection/hover logic; visible flag bit 1
at `+16`; per-row justify `+20`; x/y text offsets `+24`/`+28`; image `+48`).
Each visible row first draws the ITEMS per-state appearance record for its
style index (28-byte stride at `+828`) into the row rect (inflated −1
horizontally; when the child scrollbar is shown, the right edge is reduced by
the conditional `SB_EDGE_PAD` at `+4088`, not by the scrollbar width) — that
appearance IS the selection/hover highlight — then the row text with
colorIndex = the same style index (a selected row renders the selected FONT
pair). Rows run from scroll start `+796` for `+800` visible rows; height = font
"W" else MIN_ITEM_HEIGHT (`+804 >= 0`); truncation uses rect − 2×EDGE − the
conditional `SB_EDGE_PAD` `[orig: CListWnd_DrawItems @ 0x643f30]`.
In the typed reimpl schema, direct LIST/MULTI/LAN_LIST widgets read that sibling
top-level `<MIN_ITEM_HEIGHT>`, while a COMBO reads the same field from its
nested `<LIST_BOX>` `[orig: CListWnd_ParseXMLDefinition @ 0x645770;
CListWnd_DrawItems @ 0x643f30]`. TABLE is deliberately separate: its header
height remains the FONT "W" measure, its body row height takes the top-level
`MIN_ITEM_HEIGHT` when authored, and only the body area determines the visible
page `[orig: CTableWnd_ParseXMLContentDefinition @ 0x6427d0;
CUITable_Render @ 0x6411d0]`.

**Scrollbar visuals and owner geometry.** A SCROLL's own COLOR and OUTLINE
passes cover the full widget rect, but its IMAGE pass covers only the middle
track after one part extent is removed at each end. The part extent defaults
to the constructor's **20**; a standalone SCROLL's widget HEIGHT/WIDTH setting
overrides it, while arrow texture dimensions never choose it. The child
painter order is **SHUTTLE, SCROLLUP, SCROLLDOWN** `[orig: CScrollWnd_Construct
@ 0x64c450; CUIScrollWidget_ParseExtendedXMLDef @ 0x64c6d0;
CScrollWnd_Render @ 0x64c5c0; scroll COLOR sink @ 0x64ce70; scroll IMAGE sink
@ 0x64cf70; CUIElement_DrawOutlineRect @ 0x647fc0;
CUIScrollbar_CreateChildWindows @ 0x64d330]`.

For a bound owner, the shuttle is proportional to the current range/page with
a 20px minimum, moves with the clamped value, and the owner hides the child
when its content fits `[orig: CUIScrollbar_CalcThumbRect @ 0x64cba0;
CScrollWnd_SetPageSize @ 0x64ce10; CScrollWnd_SetRangeAndClamp @ 0x64d490;
CListWnd_DrawItems @ 0x643f30; CMEditWnd_UpdateScrollRange @ 0x661180]`.
An absent or zero-width embedded POSITION falls back to the owner's rightmost
**22px for its full height**—including the table header—rather than deriving
width or end-cap extent from a texture `[orig: CListWnd_CreateScrollChild
@ 0x6444c0; CMEditWnd_CreateScrollChild @ 0x661260; CTableWnd_Init
@ 0x640790]`. A combo's `SB_EDGE_PAD` only narrows its row/highlight/text
content; it neither locates nor sizes the scrollbar `[orig:
CListWnd_ParseXMLDefinition @ 0x645770; CListWnd_DrawItems @ 0x643f30]`.

Standalone range state is now carried end to end: `MenuWidgetState` owns
min/max/page/value, `MenuFrame::set_widget_scroll_range` exposes it to Godot,
and `MenuDriver.set_widget_scroll_range` retains and forwards it. `MenuShell`
seeds the witnessed Options controls (page is retail's inclusive page field)
as GAMMA **5..20, page 2**;
SOUNDFXVOLUME, DIALOGVOLUME, and MUSICVOLUME **0..255, page 10**; and
MOUSE_SENSITIVITY **4..511, page 10** `[orig: options_screen_init @ 0x554800;
UI_PopulateRenderAndAudioSettings @ 0x55c830]`. Persisted render/audio/input
values are not yet modeled, so each current value temporarily starts at its
minimum; that is an explicit reimpl fallback, not a claim about retail's saved
setting. Direct scrollbar input is PORTED into the compiler's mouse pump
(2026-08-11): the pump runs the witnessed interaction ahead of the claim
walk — arrows step -/+1 (ctor default), a track press pages toward the
click, a shuttle press captures an anchor and drags through the travel
ratio (ftol-truncated), and the pressed part keeps the mouse until release
like retail's child-window capture `[orig: CScrollWnd_HandleEvent
@ 0x64d050 — arrows @ 0x64d2d9/0x64d31a, track @ 0x64d0f0/0x64d10e, anchor
@ 0x64d1cb..0x64d217, drag @ 0x64d231..0x64d2aa; ctor defaults step 1
@ 0x64c4cf, page 10 @ 0x64c4d9]`. Independent per-part hover/pressed states
and named scroll events remain D-MNU-13 residue.

Reimpl: `engine/runtime/menu` (`MenuFrameCompiler`) compiles a parsed
`mnu::Screen` + a typed per-widget state snapshot (hover/press/disabled/
checked/focus/caret/value/selection, runtime item rows, table rows, marquee
lines — `MenuWidgetState`, keyed by pre-order index) into a `MenuDrawList`
(quad, outline-line, and GameFont-run payload arrays plus one interleaved
`draw_ops` painter sequence). `MenuFrame` consumes `draw_ops`, so a later
widget's quad stays later than an earlier widget's text or outline instead of
the payload type deciding the layer. This preserves the witnessed forward
scene/child walk and each widget vtable's cross-kind pass order `[orig:
CUIScene_DrawScreensAndCursor @ 0x63bf60; CUIElement_Draw @ 0x64a8a0;
CStaticWnd_Render @ 0x657b10]`, with the witnessed per-element int truncation
of scaled coordinates (the original quantization — the compiled path closes
D-MNU-4's divergence). The compiler also owns the mouse pump, the interaction
geometry queries
(row/popup/arrow/table hit tests over the same layout math), the hotkey scan,
and the edit-input module. The Godot applier (`MenuFrame`,
`godot/src/mnu/menu_frame.cpp`) uploads textures/fonts and rasterizes
the list; `MenuDriver` (`godot/game/menu_driver.gd`) orchestrates navigation,
actions, popups, and sounds over it — since the 2026-08-10 shell cutover this
is the ONE menu path (the MnuMenu Control tree is deleted). Pinned by
`tests/menu/menu_frame_compiler_test`.

- **D-MNU-13 (draw-walk residue — deferred interiors):** the TABLE, SCROLL,
  MARQUEE interiors and the wrapped-text drawer are now walked AND compiled
  (their paragraphs below). Still unwalked: the RADIOEDIT render
  (`@ 0x65d310`; its event interaction IS witnessed —
  `RadioEditWnd_handle_event @ 0x65d540`, first activation selects the radio,
  re-activation swaps radio->edit with focus + caret at end, leaving copies
  the edit text back to the radio label — no shipped JO menu authors a
  RADIOEDIT, so neither half is compiled). Compiled-path follow-ups: table
  image/SUBST/custom cells, the per-row color override (`row+28 & 4` swapping
  the `row+24` color slot with `row+32`), locked rows (state 1), and row
  overflow wrap; marquee image nodes + the 50px edge fade band. (The per-ROW-
  STATE `ITEMS` appearance passes and row-state text colors ARE compiled —
  see the table paragraph.) The compiled
  path now emits the authored at-rest track, `SHUTTLE`, `SCROLLUP`, and
  `SCROLLDOWN` for standalone SCROLL widgets and direct `<SCROLLBAR>` blocks
  used by LIST/MULTI/LAN_LIST, TABLE, MULTILINE_EDIT, and a combo's LIST_BOX.
  It preserves the full-rect COLOR/OUTLINE versus inset IMAGE geometry, the
  20px default/authored WIDTH-or-HEIGHT part extent, and shuttle/up/down child
  order `[orig: CScrollWnd_Construct @ 0x64c450;
  CUIScrollWidget_ParseExtendedXMLDef @ 0x64c6d0; CScrollWnd_Render
  @ 0x64c5c0; CUIScrollbar_CreateChildWindows @ 0x64d330]`. Embedded owners
  drive the proportional, minimum-20px thumb from their row/line range and
  `scroll_row`, hide it when content fits, and use a rightmost 22px fallback
  spanning the owner's full height when no usable POSITION width is authored `[orig:
  CUIScrollbar_CalcThumbRect @ 0x64cba0; CScrollWnd_SetPageSize @ 0x64ce10;
  CScrollWnd_SetRangeAndClamp @ 0x64d490; CListWnd_CreateScrollChild
  @ 0x6444c0; CMEditWnd_CreateScrollChild @ 0x661260; CTableWnd_Init
  @ 0x640790]`.

  Standalone min/max/page/value now crosses `MenuWidgetState` -> `MenuFrame` ->
  `MenuDriver`; `MenuShell` seeds the five named Options controls with their
  witnessed ranges/pages `[orig: options_screen_init @ 0x554800;
  UI_PopulateRenderAndAudioSettings @ 0x55c830]`. Persisted setting values are
  not modeled yet, so current=min is the explicit temporary fallback.
  Arrow clicks, track paging, and the shuttle drag/capture are PORTED into
  the compiler's mouse pump (2026-08-11 — the pump claims the pressed part
  until release, standalone sliders change their range value, embedded
  owners change `scroll_row`, and `MenuFrame.scroll_value_changed` relays
  the result `[orig: CScrollWnd_HandleEvent @ 0x64d050]`). An OPEN combo's
  popup scrollbar joins the same pump (2026-08-11 — `pump_popup_mouse`
  restricted to the open combo mirrors the popup-exclusive dispatch, and
  the popup's scrollbar child sees the sample ahead of row picking `[orig:
  dispatch_mouse_event @ 0x63ab00 g_ui_open_popup_wnd; CListWnd child walk
  @ 0x643f30]`). Remaining scrollbar residue is the constructed child
  BUTTONs' independent hover/selected/pressed states and named scroll
  events. Spin arrows compile with their default-state art (their
  independent hover states are separate child-widget state the compiled
  path does not yet model).

  **Mouse wheel (witnessed dead-end; D-MNU-18 divergence, 2026-08-12).**
  Retail JO ships NO functioning menu wheel scroll. The full plumbing:
  `Game_WindowProc` routes `WM_MOUSEWHEEL` (0x20A) into the input layer,
  which accumulates the signed HIWORD delta in `scroll_delta` and fires
  every registered mouse callback once per full ±120 with direction masks
  `0x100` (up tick) / `0x200` (down tick), remainder persisting
  `[orig: Input_DispatchMouseEvent @ 0x761571..0x7615d0]`. The shell-menu
  callback maps BOTH tick masks to the same `WM 0x20A`, discarding the
  direction `[orig: Menu_ShellMouseCallback @ 0x54b8c6]`; the scene
  dispatcher turns that into widget event `0x100000B`
  `[orig: dispatch_mouse_event @ 0x63ab72]`, and NO handler consumes it —
  exhaustive image scans for direct and relative event-id compares find
  only move/L-down/L-up/dblclk decoding in every named handler (list
  @ 0x643cb0, table @ 0x642400 `CTableWnd_HandleNamedEvent` — defined
  2026-08-12 from an unfunctionized gap — scroll @ 0x64d050, combo
  @ 0x65c190, checkbox/button/spin/medit/edit, plus the 0x54xxxx screen
  handlers). The in-game menu callback (pre.mnu scene and the in-world
  weapon.mnu armory) has no wheel case at all — ticks return unhandled
  `[orig: Menu_InGameMouseCallback @ 0x568760, registered @ 0x5687e5 /
  0x569424]`. The wheel's only functioning menu-adjacent consumer is the
  CONTROLS remap capture, mapping tick masks to binding masks
  0x100→0x400 / 0x200→0x800 `[orig: @ 0x55c7bb..0x55c7d5]`.

  Reimpl (D-MNU-18, ratified `PERMANENT` 2026-08-12 by maintainer
  decision — ADR 0022 / ledger register): `MenuFrameCompiler::pump_mouse_wheel` scrolls one row per
  tick (the CScrollWnd arrow step), routed the way the witnessed dispatch
  routes every mouse event — the open popup exclusively, else the
  front-most row owner (List/Multi/LanList/Table) under the point;
  `MenuShell`/`ArmoryPresenter` feed Godot wheel notches as ticks. The
  multiline edit stays unwired (D-MNU-13 residue). Pinned by
  `menu_frame_compiler_test` and `menu_driver_test.gd`.

## The menu backdrop (Bink underlay) `[orig: UI_CreateMenuBinkVideos @ 0x54b590; BinkVideo_UpdateAllSlots @ 0x5676f0]` (grilled 2026-08-10)

The animated main-menu backdrop is NOT the custom appearance: main.mnu's MAIN
window (`<APPEARANCE type="custom">`, rect (0,75)-(800,525)) and the
LOGO_SPLASH_HDR/FTR windows register no handler — their custom events fire
into the void, the region stays unpainted, and three looping Bink movies
drawn BEFORE the widget scene walk show through (the shipped fixtures comment
them `<!-- bink panels begin-->`). Mechanism, all witnessed:

- **Slots** (`g_bink_slots @ 0x25E5758`, 72-byte stride, max 4; handles
  `g_bink_slot_main/header/footer @ 0x252DD9C/98/94`): created by
  `UI_CreateMenuBinkVideos @ 0x54b590` from `Menu_InitShellResources
  @ 0x552500` (fresh boot AND return-from-game, BEFORE the first .mnu parse)
  and from `apply_video_mode_change @ 0x55a590` (recreate at the new scale):
  `main.bik` (0,75)-(800,525), `header.bik` (0,0)-(800,75), `footer.bik`
  (0,525)-(800,600), loop=1 each; `expansion\<exp>\<file>` preferred when
  `File_ExistsOnDisk @ 0x562d80`; a failed `BinkOpen` leaves the slot empty
  silently. Design rects scale device-ward via
  `CUIScene_ScaleRectDesignToDevice @ 0x63b210` (the same anamorphic 800x600
  pair as every widget, int-truncated).
- **Per menu frame** (`Menu_RenderFrame @ 0x54b7c0`: clear, BeginScene,
  `BinkVideo_UpdateAllSlots @ 0x5676f0`, scene walk, Present — the update's
  ONLY caller, so menu movies never tick in-game): per open slot, `BinkWait`
  paces on the movie's own clock (the quad still re-draws every frame); when
  a frame is due, `BinkVideoSlot_RenderFrameToTexture @ 0x567540`
  (`BinkDoFrame` → LockRect → `BinkCopyToBufferRect`) then
  `BinkVideoSlot_Draw @ 0x5674d0` — shader apply, SetTexture, ONE stretched
  quad (`fill_fullscreen_quad_vertices @ 0x678db0`); loop = the
  `FrameNum = 0` poke when the last frame passes; texture is movie-native
  size (pow2-padded with UV crop when the device requires).
- **Per-screen visibility** (draw-only gate — `BinkVideoSlot_Draw` checks
  the +0x44 visible flag; a hidden movie keeps decoding): STARTUP shows the
  center movie alone (`UI_OnStartupScreenActivate @ 0x5557f0`); every other
  shipped screen enables the header+footer strips and disables main
  (`UI_EnableHeaderFooterBinkStrips @ 0x556b50` + the per-screen inlined
  trios). Teardown at menu exit: `Menu_TeardownShellAndCloseBinkVideos
  @ 0x54e430` / `BinkVideo_CloseMenuBackgrounds @ 0x54b780`.
- **Audio**: `BinkSetSoundSystem(BinkOpenDirectSound)` at slot create — a
  movie's audio track plays as authored, independent of the menu music
  volume (no BinkSetVolume/Pause/Goto in the import surface).
- The intro path is separate: `Game_PlayIntroVideos @ 0x5637a0` plays
  `prolog.BIK`/`intro.BIK` through a BLOCKING player, not the slot machinery.

**Reimpl** (2026-08-22): policy engine-side in `engine/runtime/menu/
menu_video.h` (slot table, STARTUP/strips gate, expansion-first resolution;
`menu_video` ctest), with the portable video-only BIKi decoder in
`engine/formats/bink`. The `MenuVideoUnderlay` device leg (`godot/src/mnu`)
opens the selected `.bik` directly, uploads decoded RGBA frames to a Godot
texture, draws under the MenuFrame surface, and keeps hidden slots advancing.
It is wired by the menu shell at setup/expansion-change/in-game boundaries;
open/decode failures leave the selected slot empty and are counted via
`get_failed_count()`. The old `.bik` -> `.ogv` importer workaround and its
FFmpeg dependency are gone. This first slice covers the shipped video-only
`main.bik`, `header.bik`, and `footer.bik`; Bink audio and the separate
blocking intro path remain deferred.

**The decoder's witness** (2026-08-25, `binkw32.dll` 1.5u — a separate image
from `Jointops.exe`, imagebase `0x30000000`, listing `binkw32_1_5u.asm`; every
address in this paragraph is that image's unless marked). The per-frame entry
is `BinkVideo_DecodeFrame @ 0x3001F260` (reached from `BinkDoFrame`), which
decodes the Y, V, U planes through `BinkVideo_DecodePlane @ 0x3001D2C0`; the
bundle readers, block decoders and residue walk in `engine/formats/bink/
bink.cpp` are that function's inlined legs (its bundle callbacks are the
`sub_3001C300` / `sub_3001C5E0` pointers, the tree reads `sub_3001C030` /
`sub_3001C0A0`), and the IDCT is its callee `sub_3001F3E0 @ 0x3001F3E0` (the
`0xB50` / `0xEC8` / `0x8A9` multipliers at its imul sites). The bitstream
tables are the data at `0x3004AC48` (tree lengths), `0x3004AD50` (tree codes),
`0x3004AE60` (coefficient scan), `0x3004AEA8` (spatial scans), `0x3004B2C0` /
`0x3004D300` (intra / inter quantizers). **Colour conversion is the DLL's, not
the game's**: `BinkVideoSlot_RenderFrameToTexture @ 0x567540` (Jointops.exe)
calls `BinkCopyToBufferRect(handle, bits, pitch, h, 0, 0, flags | 0x80000000
BINKCOPYALL) @ 0x5675b2` with the slot's surface code from the D3DFMT →
BINKSURFACE table `@ 0x5679be..0x567a2b` (`D3DFMT_X8R8G8B8` (22) →
`BINKSURFACE32` (3), the first `CheckDeviceFormat` hit `@ 0x567a61`; the 565 /
555 / A8R8G8B8 / 4444 / 1555 rows follow). Inside the DLL
`BinkCopyToBufferRect @ 0x30013220` calls `YUV_init(surface & 0xF) @
0x30019F00` (the call `@ 0x30013365`) and dispatches the `YUV_blit_32bpp` table
`dword_30064700` (`sub_30029270 @ 0x30029270` scalar, MMX twins). `YUV_init`
builds the law the port's `yuv_to_rgb` reproduces: luma `dword_30059178[Y] =
trunc(clamp(Y − 16, 0, 219) · 38154 / 32768)` (`imul 950Ah; cdq; and edx,
7FFFh; add; sar 15` — a truncating divide; the MMX blits fold it as `(Y − 16
sat) << 2`, `pmulhw 19077` (= 38154 / 2, `qword_30055040`) with the 16 in
`qword_30055038`), and four chroma ramps in the same fixed point: `R += (V −
128) · 52299 / 32768` (`dword_300632A8`), `G += −(V − 128) · 26639 / 32768`
(`dword_30062AA8`) `+ −(U − 128) · 12837 / 32768` (`dword_30062EA8`), `B += (U
− 128) · 66101 / 32768` (`dword_300626A8`), every channel clamped 0..255
through the per-luma clamp rows `dword_30059998[Y]` (the MMX twins clamp with
the `0x7F00` paddsw/psubusw pair). Consequences the port now carries: full-scale
luma 235 lands on **254**, mid-grey (128, 128, 128) on 130, and the ramps are
NOT BT.601's 298/409/100/208/516 with round-half-up (the previous
implementation, replaced 2026-08-25; `bink` ctest pins nine (Y, U, V) triples).
Byte order is the one device fold: retail packs `(R<<16)|(G<<8)|B` into the
X8R8G8B8 dword, the port's frame is top-down RGBA8 with an opaque alpha.

## The custom-draw appearance hook (event 1) `[orig: CUIElement_DispatchCustomDrawEvent @ 0x647f10]` (grilled 2026-08-10)

The 4th appearance pass builds `{widget, name, device L,T,R,B}` (ancestor
offsets accumulated `@ 0x6465e0`, element scale applied, edges int-truncated)
and calls `vtable+28(this, 1, &payload)`. The default sink
(`CWnd_EmitEventToNamedHandlerAndCallbacks @ 0x646970`) forwards to the
authored-ACTION handler (ignores event 1) then walks the widget-local
callback chain (`this+272`, nodes `{classMask, fn, ctx}`, invoked when
`classMask & (1 << HIBYTE(eventType))`). The shell binds handlers by name:
`CUIScene_RegisterControlCallback @ 0x63c060` rows `(screen, control, mask,
fn, ctx)` are attached to widgets by `CUIScene_BindControlCallbacks
@ 0x63af80` after every content parse (null control = whole screen; null
screen+control = the scene-level node hosting `UI_DispatchScreenEvent
@ 0x54e6a0`). A handler draws immediately, in walk order — over that
widget's other passes, under its children.

Complete event-1 consumer roster (registrar census):

| Screen / control | Handler | Draws |
| --- | --- | --- |
| CMAP / MAP, ORDERS_MAP | `CMapWindow_HandleEvent @ 0x5497f0` | the command map view (mouse-class events pan/zoom/place) |
| CMAP / CHAT_MSGS | `CMap_OnChatMsgsCustomDraw @ 0x5482d0` | `HUD_DrawConsoleMessages()` in the widget slot |
| DEATH / MAP | `command_map_overlay_input_handler @ 0x554310` | the deploy-screen map view (D-HUD-19) |
| ITEM_DATABASE / ITEM_DISPLAY | `UI_RenderEntityModelPreview @ 0x552a80` | 3D item model preview |
| VEHICLE / ITEM_DISPLAY | `render_avatar_preview_3d @ 0x563c10` | 3D vehicle preview |
| PLAYER_INFO / PLAYER_PREVIEW | `PlayerInfo_RenderPlayerPreview3D @ 0x5609c0` | the avatar 3D preview (zoom/spin from `update_player_preview_animation @ 0x55dba0`) |

The menu screens (STARTUP MAIN, LOGO_SPLASH_HDR/FTR, OPTIONS CREDITS) have NO
registration — verified by extracting every registrar's rows. jo_options
CREDITS authors `<APPEARANCE type="custom">7f3f0000</APPEARANCE>`; nothing
consumes the value (the credits scroller is the MARQUEE sibling).

## Table render `[orig: CUITable_Render @ 0x6411d0]`

The table clips (`CWnd_ApplyClipViewport @ 0x6472a0`), draws its frame and the
standard four-pass appearance, resolves the font via vtable+64 and measures
`"W"` for the default row/header heights (used when the authored heights are
negative), then walks:

- **Header:** per column (180-byte column defs; width at `col+124`, cell type
  at `col+108`, rule string at `col+176`): type 0/1/4 draws the aligned header
  label (`calculate_aligned_text_rect @ 0x63ec50`); when the column leaves
  more than 16px of headroom past the label, the RULE DIVIDER draws in
  `0xFF7F7F7F` via `draw_rule_line @ 0x6410a0` — per character `c` of the rule
  string, one centered horizontal segment of width `rect_w - (c - 'a' + 1)`
  at successive y rows starting `strlen/2` above the anchor: a
  character-PROFILED taper. Type 2 dispatches the custom draw event
  (`0x8000002`). Columns advance by width + the column gap.
- **Data rows:** 40-byte rows; the scroll window is first-visible + visible
  count; rows with `row+28 & 0xA` skip. `row+24` is the row's OWN state
  (`CTableWnd_SetRowSelected @ 0x63f5f0`: 0 default, 1 locked — immune to
  select/clear, 3 selected; the click handler `@ 0x642550..0x64259d`
  single-select clears every non-locked row to 0 then sets the hit row to 3,
  multiselect toggles 3<->0). Cell TEXT draws with the widget's state color
  indexed by that row state — `row+28 & 1` forces state 1 — never by the
  widget-level mouseover visual (`@ 0x64189a..0x6418da`); the header always
  pushes state 0 (`@ 0x641446`). `row+28 & 4` swaps a per-row COLOR override
  (`row+32`, into the `row+24` slot) around the row's draw. Per cell by
  column type: 0 text, 1/4 image (`draw_aligned_texture @ 0x6409e0`,
  per-column alignment array), 2 custom callback. Row ITEM appearance
  records (28-byte at `this+824`, keyed by `row+24`: COLOR fill / IMAGE /
  OUTLINE bits — options.mnu CONTROL_MAPPING authors the default-state
  OUTLINE grid + selected-state COLOR bar) draw behind non-custom cells,
  per CELL rect. A column overflowing the right edge WRAPS the row down by
  one row height.
- Then the viewport restores and children draw (vtable+24).

## Marquee credits scroller `[orig: CMarqueeWnd_Render @ 0x65cf90 -> render_scrolling_credits @ 0x65ca00]`

Three passes over the node linked list (next at `node+220`):

1. **Scroll:** each node's y (`node+0xAC`, mirrored at `node+172`) decreases
   by the widget's rate (`this+0x2DC`) per frame; when the LAST node passes
   the top threshold, EVERY node resets to its initial layout y
   (`node+0xA0`) — the whole credits roll loops as one unit.
2. **Fading images** (`node+208` texture, fade flag `node+212 != 0`):
   horizontally centered on the widget's center; a 50px band at the clip top
   and bottom ramps alpha linearly (`dist / 50`, clamped 0..1) onto
   half-gray `0x7F7F7F`; fully inside the band draws opaque, outside skips.
   Drawn via `CTextureManager_DrawScaledRect @ 0x654e60` at the element
   scale pair.
3. **Text + non-fading images:** non-fade images draw fixed `0xFF7F7F7F`
   while inside the clip band. Text nodes format the node text, then REMAP
   two authored separator characters (`this[185*4]` -> space,
   `this[186*4]` -> comma), resolve the per-node font (name at `node+128`,
   `CFontCache_LoadOrGetFont @ 0x652f70`), justify by `node+216` (0 left,
   2 right inset 5, else centered), copy the node's 8-dword color block
   (`node+176`), and draw through the scaled text sink.

## Widget item rendering `[orig: CSpinListWnd_Render @ 0x64b220; CUISpinList_ParseXMLDefinition @ 0x64bd10]`

`<ITEMS>`/`<ITEM type="id|image|color">` rows render three ways; the selected item is
drawn by the widget's render method:

- **id / text**: the string (id resolved through the RTXT table), drawn as text.
- **image**: the element TEXT is a texture filename, loaded into the item entry
  (`entry+48`) at parse (`CUISpinList_ParseXMLDefinition`). The selected image draws at its
  **native size**, aligned in the widget rect per JUSTIFY/VJUSTIFY (spinlist default is
  centre/centre) `[orig: CSpinListWnd_Render @ 0x64b220 -> CUIElement_DrawTextureNative
  @ 0x647e40, aligned by the native texture extents]`.
- **color**: the element TEXT is a base-16 `RRGGBB` value (`wcstoul(text, 16)` at parse),
  drawn as a **full-rect swatch** forced opaque (`color | 0xFF000000`)
  `[orig: CSpinListWnd_Render @ 0x64b220]`.

Reimpl: `resolve_item` -> `MnuItemVisual {kind, text, texture, color}`; the shared
`mnu_render_item_cell` (`mnu_item_cell.h`) hosts a Label / native-centered TextureRect /
full-rect ColorRect and shows the one matching the current item, swapping as the spinlist
cycles. The old code ran every item through `resolve_item_text` and drew the raw string,
so an image item showed its filename (`cross01.tga`) and a color item showed its hex
(`FFFFFF`) as text — the crosshair-preview and color-picker bugs. The same model backs
combo/list items (the shell can populate text-only at runtime). `D-MNU-5`: shipped menus use
image/color items only in spinlists; the combo closed cell still renders text-only (its
LIST_BOX items are all `type="id"`).

### Checkbox button presentation `[orig: sub_64AD90 @ 0x64ad90; CCheckWnd_DrawLabel @ 0x64aa20]`

`AS_BUTTON` changes checkbox presentation, not its event contract. A normal
checkbox moves its label to the right of the checkbox-art rect; `AS_BUTTON`
keeps the label inside the full Window rect and honors its horizontal
justification. Clicks still toggle the checked value, and checked rendering
forces the selected appearance (`CCheckWnd_Render @ 0x64ae20`, state index 3).
The runtime therefore presents it as a full-rect toggle button rather than a
checkbox with a detached indicator.

### `%VAR%` colors in APPEARANCE

A `<APPEARANCE type="color">` value is frequently a stylesheet variable (e.g.
`%COLOR_BLACK%` on a LIST_BOX background, `%TRIM_COLOR%` on an outline). The reimpl now
resolves the var through the stylesheet before parsing the hex
(`get_appearance_color` -> `resolve_color`, `[orig: NapiXML_ExpandVariablesInText
@ 0x63a000]`). Previously every `%VAR%` color appearance silently failed, leaving combo
dropdown popups, container backgrounds, and outlines transparent (the "stacked-inline /
overlapping" video-options dropdowns).

### FONT background colors are preservation-only

The FONT parser stores four foreground/background pairs (default, mouseover,
selected, and disabled) at element offsets `+0x7c..+0x98`
`[orig: CUIElement_ParseXMLDefinition @ 0x648d14..0x648e64]`, and inherited
fonts copy all eight values `[orig: CWnd_GetFontAndColors @ 0x646a70]`.
`draw_text_with_cursor @ 0x6533b0` selects the pair for the active state, but
the common `font_cache_draw_text_scaled @ 0x653170` path consumes only the
foreground member and never reads the paired background. Shipped JO menus
normally author black BG values. The reimplementation therefore preserves every
BG field for lossless round-tripping but intentionally does not paint a text
background.

## Spinlist arrows `[orig: CSpinListWnd_CreateUpDownChildren @ 0x64b8b0]`

`<SPINUP>`/`<SPINDOWN>` are full child windows of the spinlist (objects at `this+816` /
`this+1588`), each carrying its own `<POSITION>` + `<APPEARANCE>`. The POSITION is
**parent-relative to the spinlist** (the original adds them as children and accumulates
ancestor offsets at draw, `CWnd_AccumulateAncestorOffset @ 0x6465e0`), so e.g.
`XHAIR_COLOR` (a 45px box at LEFT=210) places its right arrow at LEFT=56 (just right of
the box) and its left arrow at LEFT=-27 (just left). Reimpl: `add_spin_button` uses the
authored coords directly and sizes a missing far edge from the appearance texture (the
three-stage POSITION fallback). The old code subtracted the spinlist origin, throwing the
arrows ~150px left and producing the doubled/misplaced look.

## Combo dropdown `[orig: CComboWnd @ 0x65be40; CComboWnd_Render @ 0x65bfd0; CComboWnd_ParseXMLDefinition @ 0x65c0d0]`

A combobox is a closed `CButtonWnd` (`this+764`, showing the selected item) plus an
embedded `CListWnd` popup (`this+1536`) `[orig: CComboWnd ctor @ 0x65be40]`. The closed
FACE is the embedded list's SELECTION rendered as the combo's own label: the render
temporarily swaps the widget text slot to `row_text(list, selected_row(list))`, draws
via `CStaticWnd_DrawLabel` (the combo's own STRING layout — shipped options.mnu combos
author an EMPTY widget STRING and rows only inside `<LIST_BOX>`), then restores
`[orig: CComboWnd_Render @ 0x65c05b..0x65c083]`. Reimpl: `combo_face_text` over the
same row collection the popup renders, emitted through the label path (2026-08-11 —
the widget-level-ITEMS-only face left every options.mnu combo blank). The parse
`[orig: CComboWnd_ParseXMLDefinition @ 0x65c0d0]` delegates the `<LIST_BOX>` content to
that embedded list, whose own window RECT (`this+13`) is set from the authored
`<LIST_BOX>` `<POSITION>` — combo-relative, in 800x600 design space. So the dropdown is a
**fixed authored rect**, not a runtime-computed box: it can sit below, beside, or above
the combo (`player.mnu` PLAYERVOICE authors a negative `TOP` to open upward).

The list render `[orig: CListWnd_DrawItems @ 0x643f30]` lays rows out inside `row_rect =
this+13`, advancing by `row_height` per row and truncating each row's text to the rect
width, with a `<SCROLLBAR>` child for overflow. The row height is the font "W" glyph
height `[orig: font_cache_measure_text_default @ 0x653680]`, overridden by `this+201` (the `<MI>` /
`<MIN_ITEM_HEIGHT>` value; ctor default `-1` `[orig: CListWnd ctor @ 0x643bb0]`) only when
`>= 0`.

Reimpl (compiled path): `MenuFrameCompiler::emit_combo_popup` /
`combo_popup_rect` open the popup at the authored `list_box.position` rect
offset to the combo, and `row_height_` uses the authored `MIN_ITEM_HEIGHT`,
else the font "W" measure. (The pre-cutover Control-tree `MnuCombo` —
`set_popup_rect`/`open_popup`/`effective_item_height` — carried the same
rules; historical.) Top-level `ITEMS`
and `LIST_BOX/ITEMS` remain independent: an authored nested collection wins even when
empty, otherwise the popup uses authored top-level fallback rows. Popup text honors
the active ITEMS horizontal/vertical justification and the LIST_BOX STRING edge
inset; `SB_EDGE_PAD` shortens only that popup row/highlight/text content when
the scrollbar is shown—it does not reposition or resize the scrollbar child
`[orig: CListWnd_ParseXMLDefinition @ 0x645770; CListWnd_DrawItems
@ 0x643f30]`. The closed cell independently honors its outer STRING layout. Two earlier bugs are
fixed: the popup background (`%COLOR_BLACK%`/`%SEMIOPAQUE_BLACK%` color appearance) not
resolving (the `%VAR%` color change above), and the popup geometry being recomputed below
the combo instead of using the authored rect — see **D-MNU-7** and **D-MNU-8**.

### Dropdown input routing (2026-07-16 grill)

While a dropdown is open, the original gives it **exclusive ownership of the mouse**
through three scene-wide globals (renamed in the IDB this session):

- `g_ui_open_popup_wnd @ 0x31C16D8` — the shown popup widget. `CWnd_SetShown @ 0x6480e0`
  writes the widget's shown flag (`+224`) and, for popup-flagged widgets (`+660` — the
  combo's embedded `CListWnd` is one), registers/unregisters it here; `CUIElement_Draw
  @ 0x64a8a0` re-registers a drawn popup-flagged widget. While it is set:
  - the WM-message dispatch routes every mouse event ONLY to the open popup's
    dispatcher — the rest of the widget tree never sees the event
    `[orig: dispatch_mouse_event @ 0x63ab00, gate @ 0x63abb5; WM 0x200..0x20A map to
    event ids 0x1000001..0x100000B, screen coords scaled into design space]`;
  - the per-frame hover/press/click/sound pump runs ONLY on the popup
    `[orig: scene_end_frame @ 0x63e600, gate @ 0x63e691]`;
  - visible-in-hierarchy holds only for the popup and its descendants (a parent walk
    that reaches the root without passing the popup returns 0)
    `[orig: CWnd_IsVisibleInHierarchy @ 0x646290, gate @ 0x646299]`.
- `g_ui_active_combo_wnd @ 0x31C16D0` — the combo owning the open dropdown
  (`UI_SetActiveComboWnd @ 0x6463e0` / `UI_ClearActiveComboWnd @ 0x646400`). The
  dispatcher gives it a priority peek of every event `[orig:
  dispatch_mouse_event_to_children @ 0x647900, peek @ 0x647917]`, which drives the
  combo's outside-close check.
- `g_ui_mouse_capture_wnd @ 0x31C16CC` — transient press-capture: a button press sets
  it, release clears it, and while set the dispatch bypasses hit-testing entirely
  `[orig: CButtonWnd_HandleNamedEvent @ 0x658340 (set @ 0x65839c, clear @ 0x6583ed);
  bypass @ 0x647932]`. Capture set mid-iteration is also what gives the front-most hit
  widget priority among overlapping siblings in the frame pump.

The combo protocol `[orig: combobox_handle_event @ 0x65c190]` (vtable+32, fed by the
anonymous sink `CWnd_EmitEventToNamedHandlerAndCallbacks @ 0x646970` re-dispatching with
the widget's own name):

- **Toggle** (event `0x3000001`, produced by the closed-cell click): opening first sends
  the currently active combo its own toggle — **at most one dropdown open per scene**
  (`@ 0x65c210`) — then sets the active combo and shows the list; closing hides the list
  and clears the active combo (`@ 0x65c251`).
- **Outside press** (events `0x1000002`/`0x1000004` — L-down/dbl-click — via the priority
  peek): if the point is outside BOTH the closed cell and the list rect (children
  included), the combo sends itself the toggle — close — and since routing was exclusive,
  the press reaches nothing else: **the dismissing click is consumed** (`@ 0x65c261..0x65c2bc`).
  A press on the (input-dead) closed cell is inside-combo, so it neither closes nor
  re-toggles: clicking the open combo's own cell does nothing.
- **Row pick** (`"LISTBOX_WND"` event `0x5000001`): forward the selection, hide the list,
  clear the active combo (`@ 0x65c2fd..0x65c319`).
- **Screen switch** clears both the popup and capture globals — navigation kills an open
  dropdown `[orig: CUIScene_SelectNodeByName @ 0x63b6b0 (@ 0x63b6b8 / 0x63b7c4)]`;
  scripted window/url/form actions clear press-capture (`CUIWidget_HandleScriptedAction
  @ 0x6497f0, @ 0x6498ce/0x6499ea/0x649a51`) and track the popup global only when the
  action's target is itself popup-flagged (`@ 0x64993a / 0x64997b`).

Draw order in the WALKED code has **no overlay pass**: the open list renders at its
tree position (`CComboWnd_Render @ 0x65bfd0` renders the closed cell, then children in
array order — the embedded list is attached as a child by `CWnd_SetParentAndAttach
@ 0x6480a0` during parse — and `CListWnd_DrawItems @ 0x643f30` early-outs on hidden).
The earlier "unobservable in shipped menus" reading (from `player.mnu`, whose three
combos all author their LIST_BOX rects below every closed cell) is FALSIFIED by
`options.mnu`: the Advanced video panel authors WATERQUALITY before SHADOWQUALITY /
PARTICLES at a 30px row pitch under an 80px popup — pure tree-order painting would
overdraw the open popup, yet retail renders it on top. **The retail topmost mechanism
remains unwalked** (2026-08-11 sweep ruled out: a scene/frame overlay pass
(`Menu_RenderFrame @ 0x54b7c0` is gate → clear → walk → present), a draw-path
`IsVisibleInHierarchy` gate (input-only xrefs), tree reordering at toggle
(`combobox_handle_event @ 0x65c190`), and the `+0x290` dirty flag (no menu-range
reader)). The reimpl defers open popups to a post-walk overlay pass — the D-MNU-12
menu-top decision the Control-tree overlay carried.

**Menu-top overlay vs host mounts (regression fixed 2026-08-15, in the D-PLAYERINFO-1
train):** the compiled draw list is painted by one `MenuFrame` Control, and companions
mount their own Controls as frame CHILDREN (the PLAYER_INFO `AvatarPreview` over
`PLAYER_PREVIEW`, the loadout icon `TextureRect`s) — Godot draws later children over
the parent, so since the #476 cutover every popup whose LIST_BOX rect overlapped a mount
was painted UNDER it (all three `player.mnu` character dropdowns author `(0,65)-(214,306)`,
exactly the preview rect: they opened, took the click, and were invisible), and the
frame-drawn cursor vanished over the mounts. The compiler now marks where the overlay
tail begins (`MenuDrawList.overlay_op_start` = the ops after the widget walk: popups,
then the cursor — retail paints both after every screen widget,
`CUIScene_DrawScreensAndCursor @ 0x63bf60`) and `MenuFrame::_draw` paints that tail on
a child canvas item one z above the frame, so it draws over any companion mount. Pinned by
`menu_frame_compiler_test` (the overlay tail holds the popup quads; empty with no popup
and no cursor).

**Activation vs scripted ACTION order (fixed 2026-08-15, same train; cataloged as D-MNU-19 — kept divergence, reimpl-structural):** retail runs a
button's ACTION list first and its registered control callback last
(`CUIWidget_HandleScriptedAction @ 0x6497f0` walks the list, then calls
`widget[63]->vtable+32`), but its scene keeps every loaded screen alive, so PLAYER_INFO's
ACCEPT callback (`save_player_info_from_dialog @ 0x55ee10`) still reads its controls after
the OK button's cross-.mnu jump to `main.mnu`. The reimpl shell REPLACES the document on a
cross-file ACTION, and the companion guard (`get_menu_file() != wired file`) then dropped
the activation — ACCEPT never reached `commit`, so nothing was ever saved from the live
screen. `MenuDriver._activate_widget` now emits `widget_activated` before dispatching the
ACTION list: observers read the same still-live control values retail's callback reads,
and the jump follows.

Reimpl: the exclusivity is hosted as a full-menu transparent catcher overlay
(`ComboPopupOverlay`) added as the owning `MnuMenu`'s **last child** on open, with the
styled popup box inside it — last-in-tree wins Godot mouse picking and draw order, which
Godot's z_index does not affect (the pre-fix popup was a z-lifted child of the combo:
drawn on top but siblings stole its clicks, and nothing closed on outside press, so
`player.mnu`'s stacked-rect dropdowns could pile open on top of each other). The catcher
implements the witnessed outside-press close/consume + dead-cell rule; `MnuMenu`
tracks the single active combo (`register_open_combo`/`close_active_combo_popup`) and
closes it on every screen change; a combo leaving the tree or losing tree visibility
closes its own popup. Bare shell-built combos with no owning menu (a case the original
does not have) keep the legacy child-of-combo popup. See **D-MNU-11** (fixed) and
**D-MNU-12** (kept). Pinned by `mnu_combo_test.gd::test_combo_popup_overlay_hosts_exclusive_input`,
`::test_combo_single_open_per_menu`, `::test_combo_outside_press_closes_and_nothing_else_opens`,
`::test_combo_press_on_own_cell_keeps_popup_open`, and `::test_screen_change_closes_popup`.

## Marquee / credits `[orig: CMarqueeWnd @ 0x65c430; marquee_load_credits_from_ini @ 0x65c5a0]`

A `marquee_wnd`'s `<DATASOURCE>` (e.g. `nlist.kda`) is a CBIN-encrypted credits config,
NOT plain text: `ConfigFile_LoadGlobal` reads an `[ENV]` section (`SCROLL_RATE`,
`CENTER_X`, `VERTICAL_SPACE`) and a `[TEXT]` section whose lines are AES-decrypted and
carry formatting codes (`~C` colour, `~F` font, `~I`/`~F` image, `~J` justify, `<CR>`
newline) `[orig: marquee_load_credits_from_ini @ 0x65c5a0]`. Reimpl: a CBIN datasource is
routed to the existing `CreditsPlayer` (fed by a `CbinCreditsResource` decoded from
the file's bytes via `CbinCreditsResource::from_cbin_bytes`, so it works from a PFF); a
plain-text datasource keeps the simple `MnuMarquee`. The old code read the binary
`.kda` as a string, so the credits showed the literal `CBIN` magic and did not scroll.
`D-MNU-6`: the CBIN credits' custom fonts/textures are not yet resolved from the resource
root (text scrolls with the default font); a follow-up.

## Controls / key-binding table (CONTROL_MAPPING) `[orig: UI_PopulateControlMappingList @ 0x55c0c0]`

The Options screen's **Controls** tab hosts a `type="table"` named `CONTROL_MAPPING` (3 columns
Class / Action / Control) the shell fills with the player's key bindings, switched by an input
device (Keyboard / Mouse / Joystick). The `.mnu` declares only the table template; the rows are
shell-populated, like the mission/mod lists (see menu-wiring.md). The 2026-06-23b grill.

**Data model.** A static action catalog `aAbsoluteTurnLe @ 0x8159cb` (108-byte stride, ~112
entries) holds per action: a marker-prefixed English display NAME (offset 0; the leading `!`/`|`
no-localize marker is stripped on display), a config TOKEN (offset 40, e.g. `move_forward`), a
category/**Class id** (offset 85), and a default keyboard binding (primary + secondary Windows-VK
codes in the record's lead bytes — i.e. at the catalog record's `-15`/`-13` offset relative to the
next name, validated to the canonical JO scheme). `UI_BuildKeyBindingLoadoutTable @ 0x559e50`
builds a per-action display row from the player profile's binding array (`dword_25510fc+1808`,
72-byte entries), resolving the action name via `KeyHelp_GetStringWithFallback("Text", …)`
(fallback to the catalog NAME), formatting the Control column with
`KeyBinding_FormatBindingString @ 0x559a10`, and sorting via `UI_CompareSessionListEntries
@ 0x559d10` into 432-byte (0x1B0) display entries in `Base @ 0x25c7720`.

**Class names** come from `KeyBinding_BuildCategoryPages @ 0x4966c0`: the Class id resolves to a
name via `KeyHelp_GetStringWithFallback("Text", "<KEY>", "!<Name>")` — `0` Null, `1` Movement,
`2` Weapons, `3` Camera, `4` Map, `5` Communications, `6` Server, `7` NovaLogic, `9` Cheat,
`10` System, `11` Debug, `12` teammatemenu, `13` Spectator.

**Population.** `UI_PopulateControlMappingList @ 0x55c0c0` finds the widget by name
(`UI_FindScreenControl(…, "CONTROL_MAPPING") @ 0x63ae80`), clears it (`CTableWnd_RemoveRow(-1) @ 0x641a40`), and for the
active device `dword_25db7d8` (`0` keyboard / `1` mouse / `2` joystick) inserts a row per entry
(`table_insert_row @ 0x641c30`) and sets the cells (`@ 0x63edf0`). Each device has its own runtime
binding array (kb `dword_25c7724` / mouse `byte_25c784c` / joy `byte_25c7740`).
`refresh_control_mapping_list @ 0x55b320` recomputes per-row conflict state
(`check_weapon_slot_conflict @ 0x55ae60`, a kong-misnomer for *binding* conflict) and tints
conflicting rows yellow (`sub_640110(row, …, -256)`). The device radios call
`UI_SelectControlsInputDevice(mode) @ 0x55bcd0` (sets `dword_25db7d8`, swaps the REMAP_INSTRUCTIONS text id
`REMAP_Keyboard`/`REMAP_Mouse`/`REMAP_Joystick`, repopulates).

**Control column format** `[orig: KeyBinding_FormatBindingString @ 0x559a10]`: up to two key slots,
each prefixed `Ctrl-` / `Shift-` when its modifier word is `17` / `16`, joined by the localized
"OR" (` XXor ` fallback -> ` or `). Key names decode through `KeyBinding_GetKeyNameAndDisplayName
@ 0x494c60`, a Windows-VK switch returning a display name ("Mouse 1", "Up", "Space", "F1", "[", or
the printable char). Mouse buttons use special codes (`1` left, `2` right, `16` middle, `1024`
wheel up, `2048` wheel down); joystick uses `JOYBUTTON%d`.

Reimpl: **`engine/runtime/controls`** (Godot-agnostic) ports the catalog (`controls.cpp` `k_catalog` —
byte-exact names/tokens/Class id + the default VK binding from the catalog's binding slot,
validated Forward=W/Up, Reload=R, Jump=Space, …), the Class-name table (`action_class_name`), the
VK decoder (`key_name`), and the binding format (`format_binding`); `build_rows(device)` mirrors
`UI_PopulateControlMappingList`. The Godot wrapper **`ControlsModel`** hands rows to
`godot/game/menu_shell.gd` (`_seed_control_mapping` / `_fill_control_mapping`), which fills the
`CONTROL_MAPPING` `MnuTable` via `add_rows` and wires the Keyboard/Mouse/Joystick radios to
repopulate. The earlier reimpl left the table empty — `menu_shell` had no populate path for a
`type="table"`, so the Controls tab rendered floating headers over a blank grid.

The table render itself was also corrected this pass (see Table render below). This pass is
**read-only**: it reproduces what the Controls tab DISPLAYS. Live double-click rebinding
(`update_control_mapping_display @ 0x55b700`), DEFAULTS (`sub_55bd90`) / CLEAR_KEY (`loc_55bfd0`),
and profile persistence are deferred (D-CTRL-3), gated on a real game input-action layer.

### Table render `[orig: CTableWnd_ParseXMLContentDefinition @ 0x6427d0]`

The table render is carried by `MenuFrameCompiler::emit_table` since the
2026-08-10 cutover (the corrections below were made in the Control-tree
`build_table`/`MnuTable` and carry forward where the compiled path models
them — the image/SUBST/custom cells and per-row overrides are D-MNU-13
compiled-path follow-ups):

- **Header `type="id"` is resolved** through the RTXT table (`resolve_text`), closing the open
  inner divergence — the header branch `@ 0x64344a` looks the text up via
  `CUIStringTable_LookupString @ 0x6434df`; the old reimpl drew the raw id.
- **Body cells align per the `<BODY>` justify**, not the `<HEADER>` justify (the old code reused
  the header justify, so a LEFT-authored body rendered centred).
- **HEADER and BODY `vjustify` are independent.** Sortable headers keep their
  Button input surface but use a child text layer so TOP/BOTTOM alignment is not
  lost. Text and bitmap body cells use the BODY vertical alignment.
- **`SCALE_BITMAP` is live.** An unscaled bitmap keeps its native size and authored
  alignment; a scaled bitmap fills its cell. `BITMAP_FLAGS` remains
  preservation-only because decoded Godot textures have no proven one-to-one
  mapping for the legacy DirectDraw flag vocabulary.
- **`CUSTOM_DRAW` has an explicit shell seam.** At runtime a connected
  `custom_cell_requested(row, column, value, slot)` handler synchronously fills a
  table-owned cell slot. With no shell—or in Edit mode—the normal cell fallback
  remains visible; slots are recreated on every rebuild and must not be cached.
- **The compiled scrollbar honors a usable authored
  `<SCROLLBAR><POSITION>`** (table-relative); otherwise it uses retail's
  rightmost 22px/full-table-height child rect. The header keeps the FONT "W"
  height while top-level `MIN_ITEM_HEIGHT` controls body rows, so the visible
  page excludes the header even though the scrollbar itself spans it. The
  default-state parts use that body page, row count, and `scroll_row` for the
  proportional minimum-20px thumb and remain hidden when all rows fit `[orig:
  table SCROLLBAR parse delegate @ 0x643b22; CTableWnd_Init @ 0x640790;
  CUITable_Render @ 0x6411d0; CUIScrollbar_CalcThumbRect @ 0x64cba0;
  CScrollWnd_SetPageSize @ 0x64ce10; CScrollWnd_SetRangeAndClamp @ 0x64d490]`.
  Per-part states and direct manipulation remain D-MNU-13 residue. The ITEMS
  `%TRIM_COLOR%` outline now draws as a header rule + per-row grid line.

## Sound `[orig: widget_process_mouse_event @ 0x647a00]`

A `<SOUND>` element stores `{trigger, bank-id}` in a per-STATE slot, keyed by the
STATE attribute (`MOUSEIN=1`, `MOUSEOUT=2`, `SELECTED=3`), with `1<<state` OR'd into
a mask. The element TEXT names a `.lwf` bank; `TRIGGER` names a sound SET inside it.
Banks add-ref into a refcounted collection
(`sound_bank_collection_add_or_ref @ 0x652b40`, stricmp dedup, from
`CUIElement_ParseXMLDefinition @ 0x648ada`); a visual-state transition fires the slot
via the collection play path (`sound_collection_play_trigger @ 0x652de0` ->
`SoundBank_FindTriggerAndPlay @ 0x75d010` -> `SoundBank_PlayTriggerEntries @ 0x75ccd0`).
A set play services every layer (one member each via the global selection RNG; see
`docs/audio/lwf-dbf-sound-re.md`). UI channel volume is the `CGameMenu` master volume
(`+92`, ctor default `0xFF @ 0x63e060`); member volume only participates on layers
with distances (none in shipped menus).

Reimpl: `MnuWidgetSounds` (per-state slots) on each widget;
`MnuMenu::resolve_sound_bank` (per-file `.lwf` cache, the collection analogue) ->
`play_lwf_set` (reusing `opennova::audio::SoundSelector` from the sound slice);
`master_volume` property. The old model keyed sounds on trigger SUBSTRINGS
(`MOUSE`/`OVER` -> hover, `CLICK`/`SELECT` -> click), had no MOUSEOUT, and routed every
widget through a single shell `.lwf` profile - all corrected.

`g_MenuSoundBank @ 0x25DC3E0` (the hardcoded `menu.lwf` load at profile-selector init
`@ 0x5613bf`) is a RED HERRING: it is the player-info VOICE preview bank
(`VOICE_%d`, `PlayerInfo_PreviewVoice`), not the widget sound mechanism.

## Screen music `[orig: UI_DispatchScreenEvent @ 0x54e6a0]`

Every screen event stores the active screen's `MUSICVAR` field (screen +0x14) into
AudioVM Var2 at `0x54eff4`. The write is unconditional: an absent field contributes
its parsed default zero, and showing the same screen again repeats the store. The
runtime mirrors that rule through `MnuMenu::apply_music_for_screen`.

## XML entities `[orig: XML_ParseCharEntity @ 0x769cc0; table @ 0x85a628]`

Numeric entities are DECIMAL-only (`_wtol` base 10; `&#xNN;` -> 0) and truncate to the
low byte. The named table is Latin-1: 7 case-insensitive core entries
(`quot/amp/lt/gt/copy/reg/nbsp`, with `nbsp -> 0x20`) and the case-sensitive accented
set `0xC0-0xFF` + `laquo/raquo`. There is NO `&apos;`. An unknown entity (no match / no
`;`) decodes to a bare `&` with the name text left to re-parse verbatim. Reimpl:
`decode_entity` matches this exactly; `escape_xml` emits only the 4 decodable entities
(never `&apos;`).

## `%VAR%` expansion `[orig: NapiXML_ExpandVariablesInText @ 0x63a000]`

The engine expands `%name%` from a shell-supplied list at `ctx+84` over the whole
buffer before parse (`<RAW_TEXT>` is copied verbatim; an unresolved `%name%` is kept
literal). OpenNova keeps raw tokens in the document and expands per consumed field at
build time (colors/fonts/textures/text) - see ADR 0005. Documented gap: shell variables
in non-themed fields need a shell var map plumbed into the builder.

## MNS stylesheet format `[orig: Menu_InitShellResources @ 0x552500; parse_key_value_buffer @ 0x639870]`

The substitution table lives in `menu_style.mns` ("named menu_style.mns for the
game to find it"). Menu initialization calls
`NapiConfigMap_LoadIncludeFile @ 0x63b970`, which reads the file and passes its
buffer to `parse_key_value_buffer @ 0x639870`; menu XML expansion later uses
`NapiXML_ExpandVariablesInText @ 0x63a000`. This resolves the previously
unwitnessed loader chain.

The lossless document model remains (ADR 0014): typed node fields exactly
partition the file bytes, so an untouched parse -> serialize is byte-identical
and an edited value changes only its own line. Runtime flattening is a separate
retail evaluator because malformed source may still need inspection or repair
outside ONED.

Observed runtime rules:

- names are case-insensitive and a duplicate silently replaces the earlier
  value; duplicate diagnostics are a tooling enhancement;
- an unknown `%VAR%` remains literal;
- `#if` tests only the first non-whitespace value character: `0` is false and
  every other character is true, so `0foo` is false while `1foo` and `2` are
  true;
- directives encountered during a continuation remain directives; a
  continuation can resume into the next ordinary line;
- inactive scanning searches for the next `#` directive marker rather than
  requiring a line start;
- invalid names or a missing value delimiter fail evaluation;
- doubled backslashes stay doubled: the scan advances across both bytes at
  `0x639c0e..0x639c17`, then copies the original source span.

Disposition: **D-MNS-1** and **D-MNS-2** match retail (the lossless model keeps their
useful diagnostics); **D-MNS-3** and **D-MNS-4** are fixed by routing runtime
consumers through the retail evaluator. The lossless document intentionally
stays permissive so malformed files remain repairable.

## Verdict

**matching** (after this grill) on: type factory + unknown-token preservation, the
three-stage POSITION layout + texture-into-rect, the 8-piece frame + data-driven
stencil insets, per-state sound slots + per-element bank resolution + the set/layer
play and master volume, the XML entity policy, and the format round-trip (ADR 0002).

**matching** (2026-06-23 render grill): the 800x600 anamorphic coordinate system, the
draw-nothing frame fallback, MONOGRAM-parsed-but-not-drawn, spinlist/list/combo item
rendering (text / native image / full-rect color swatch), `%VAR%` color appearances,
spinlist SPINUP/SPINDOWN parent-relative geometry, the combo closed-cell + LIST_BOX
popup, the marquee_wnd CBIN-credits datasource, and the `DRAW_FRAME` frame-draw gate
(`elem+0x134` in `CStaticWnd_Render`; the old own-frame-drawn-unconditionally bug that put
full-window camo behind the in-game ESC menu and options screen is fixed).

**matching** (2026-06-23c combo-dropdown grill): the combobox dropdown is the embedded
`CListWnd` opened at the authored `<LIST_BOX>` POSITION rect (`CComboWnd` ctor `@ 0x65be40`,
`CComboWnd_ParseXMLDefinition @ 0x65c0d0`, list render `CListWnd_DrawItems @ 0x643f30`), with
the row height from `<MIN_ITEM_HEIGHT>`/font and below/beside/upward placement honored. The
old reimpl recomputed the popup below the combo, which dropped `player.mnu`'s semi-transparent
NATIONALITY list over the sibling DIVISION/COMBO_LIST combos (text bled through) — fixed
(D-MNU-7); the hardcoded 16px row-height default is replaced by the font/authored height
(D-MNU-8). Pinned by `mnu_combo_test.gd::test_combo_popup_uses_authored_listbox_rect` and
`::test_combo_popup_fallback_when_no_listbox_rect`.

**matching** (2026-07-16 dropdown-input grill): while a dropdown is open the original
routes mouse input exclusively to the open list (three gates: `dispatch_mouse_event
@ 0x63ab00`, `scene_end_frame @ 0x63e600`, `CWnd_IsVisibleInHierarchy @ 0x646290` over
`g_ui_open_popup_wnd @ 0x31C16D8`), keeps at most one dropdown open per scene
(`combobox_handle_event @ 0x65c190 @ 0x65c210` over `g_ui_active_combo_wnd @ 0x31C16D0`),
closes on an outside press with the press consumed (`@ 0x65c261`, closed cell dead while
open), and clears the dropdown on screen switches (`CUIScene_SelectNodeByName @ 0x63b6b0`).
Ported (post-cutover) as the MenuDriver popup gate: while a popup is open the driver
routes every mouse sample exclusively through the engine's popup geometry queries
(`combo_popup_row_at`/`combo_popup_contains`), keeps one open combo, consumes the
dismissing outside press, and closes on every screen switch (D-MNU-11; the pre-fix
Control-tree reimpl let overlapped siblings steal popup clicks and stack dropdowns
open). The compiled popup draws inside its owning combo's walk position, matching the
original's tree-positional order — the Control-tree era's menu-top overlay divergence
(D-MNU-12) dissolved with the cutover. Pinned by the popup cases in
`tests/game/menu_driver_test.gd` + the geometry checks in
`tests/menu/menu_frame_compiler_test`.

**matching** (2026-06-23b controls grill): the CONTROL_MAPPING population (the action catalog +
Class-id->name table + per-device row build), the byte-exact default keyboard bindings, the Control
column format (key-name decode + `Ctrl-`/`Shift-`/`OR`), and the three table-render fixes (header
`type="id"` lookup, body justify, authored scrollbar position) — the table now renders a populated,
scrollable, correctly-aligned grid instead of floating headers over a blank body.

**matching** (2026-08-09 draw-walk grill): the full render dispatch — the menu mode
callbacks (`Menu_RenderFrame @ 0x54b7c0` -> `CUIScene_DrawScreensAndCursor @ 0x63bf60`),
the forward screen/child draw order against the reverse input walk, the per-state
appearance records (COLOR/IMAGE/OUTLINE/custom passes) with the DEFAULT/DISABLED/
MOUSEOVER/SELECTED state vocabulary and its availability fallback, the per-frame state
pump, the draw-time font/color inheritance and state color pairs, glyph scaling by the
anamorphic pair, the edit focus-forces-state-2 + blinking stretched-underscore caret +
password mask + scroll window, the checkbox/radio checked-state forcing rules, the list
row style-index model, and the unscaled native-size cursor draw — ported as the
`engine/runtime/menu` `MenuFrameCompiler` (ADR 0033 R2) with the per-element int
truncation restored.

**matching** (2026-08-11 compiled-menu visual regression repair): authored
`MAP_STATE`/`HEIGHT` IMAGE bands survive compilation as cropped atlas UVs
`[orig: CUIElement_ParseXMLDefinition @ 0x648120;
CUIElement_DrawTextureNative @ 0x647e40 -> CTextureManager_DrawScaledRect
@ 0x654e60]`; frame BRUSH/STENCIL quads translate retail's neutral
`0x7F7F7F` modulate-2x input to effective white for Godot's ordinary multiply
`[orig: CUIElement_DrawFrame @ 0x64a210; init_border_materials @ 0x646f70]`;
and `MenuDrawList::draw_ops` preserves quad/line/font-run interleaving across
the forward painter walk `[orig: CUIScene_DrawScreensAndCursor @ 0x63bf60;
CUIElement_Draw @ 0x64a8a0; CStaticWnd_Render @ 0x657b10]`. Authored
at-rest scrollbar visuals now preserve the full-rect COLOR/OUTLINE versus
middle-track IMAGE split, the 20px default/authored WIDTH-or-HEIGHT part
extent, and shuttle/up/down child order `[orig: CScrollWnd_Construct
@ 0x64c450; CUIScrollWidget_ParseExtendedXMLDef @ 0x64c6d0;
CScrollWnd_Render @ 0x64c5c0; CUIScrollbar_CreateChildWindows @ 0x64d330]`.
Bound list/table/multiline/combo owners provide the proportional minimum-20px
thumb, hide-on-fit, and rightmost 22px/full-height fallback geometry `[orig:
CUIScrollbar_CalcThumbRect @ 0x64cba0; CScrollWnd_SetPageSize @ 0x64ce10;
CScrollWnd_SetRangeAndClamp @ 0x64d490; CListWnd_CreateScrollChild @ 0x6444c0;
CMEditWnd_CreateScrollChild @ 0x661260; CTableWnd_Init @ 0x640790]`.
Standalone min/max/page/value now crosses `MenuWidgetState`, `MenuFrame`, and
`MenuDriver`; `MenuShell` seeds GAMMA 5..20/page 2, the three volume controls
0..255/page 10, and MOUSE_SENSITIVITY 4..511/page 10 `[orig:
options_screen_init @ 0x554800; UI_PopulateRenderAndAudioSettings @ 0x55c830]`.
Persisted setting values remain unmodeled, so current=min is explicitly
temporary. Per-part state remains D-MNU-13 residue.

**matching** (2026-08-11 review round, same slice): the CScrollWnd
INTERACTION joined the compiler's mouse pump — arrows step -/+1, a track
press pages toward the click, a shuttle press captures and drags through the
ftol-truncated travel-ratio inverse, and the pressed part keeps the mouse
until release exactly like retail's child-window capture, so a scrollbar
press can never become another widget's click `[orig: CScrollWnd_HandleEvent
@ 0x64d050]`. The combo closed face now swaps unconditionally (an empty
selected row draws a blank face, never the widget's authored TEXT
`[orig: CComboWnd_Render @ 0x65c05b..0x65c083]`), and the draw/interaction
visible-row gates share one span solve. On the Controls side, the record's
"+12/+14 extended-flag words" reading is FALSIFIED: they are per-slot
MODIFIER VK words (17 Ctrl / 16 Shift) driving the display prefixes, the
capture writes 17 iff the queued event flag word is EXACTLY Ctrl-held-alone
(0x800), and the Ctrl key itself can never be captured — witnessed through
`Input_QueueKeyEvent @ 0x760c10` / `Input_DequeueKeyEvent @ 0x760d60` /
`KeyBinding_FormatBindingString @ 0x559a10` and ported end to end
(capture, display, blob, and the modifier-gated gameplay sampling).

**matching** (2026-08-10 multiline grill + shell cutover): the multiline edit render
(`CMEditWnd_Render @ 0x6608e0` — no focus forcing), the wrapped-text drawer
(`draw_text_wrapped @ 0x653710` via `draw_text_wrapped_clipped @ 0x653D60`: last-space
word wrap, LF-only explicit breaks, the 1.0-scale per-line justify/advance vs
widget-scale threshold, the first-visible-line window that advances nothing while
skipping, the `0x40000` bottom clip, the caret quirks) and the line-based scroll model
(`font_cache_count_wrapped_lines @ 0x653b90`, `CMEditWnd_UpdateScrollRange @ 0x661180`,
`widget[978]`) — ported as `emit_multiline_edit`/`emit_wrapped_text` +
`multiline_line_counts`, pinned by `test_multiline_wrap`. With this, the game shell
and the armory/deploy presenters cut over to the one
compiled path (`MenuFrame` + `MenuDriver`); the MnuMenu Control tree and its widget
classes are DELETED. RADIOEDIT (unauthored in shipped JO menus) and the compiled-path
follow-ups stay under D-MNU-13.

**matching** (2026-07-30 menu parity pass): the typed format retains source
encoding/BOM, optional-field presence, ordered HOTKEY/APPEARANCE/ITEM data, the
complete sixteen-type Action payload, and nested list/table/scroll structures
without raw-source replay. Runtime keyboard dispatch follows the witnessed
virtual-key versus character paths and activates the first visible document-order
match. WINDOW enable/disable/toggle, initial disabled state, AS_BUTTON layout,
edit constraints, per-screen RTXT, and the shared authored scrollbar range/art/
sound seam are live. Optional `has_*` and container `present` state is
authoritative at save and build time, so retained latent values never
re-materialize until presence is explicitly re-enabled. Browser, LAN, form,
application, and MNX Actions remain
authored Actions and cross the explicit host boundary with their full payload.

Accepted/divergent (each a documented decision, not a defect):

- **D-CTRL-1 (mouse/joystick defaults):** the keyboard defaults are byte-exact from the catalog;
  the per-device mouse/joystick binding arrays are profile-built at runtime, not static, and are
  not ported — mouse/joystick rows show the action list with a blank Control column.
- **D-CTRL-2 (visibility filter) — FIXED 2026-07-05:** the witnessed per-entry gate
  (`(*entry & 0x20)==0 && (*entry & 0x800)!=0` in `UI_PopulateControlMappingList @ 0x55c0c0`)
  is ported: every catalog row carries its witnessed flag word (the static catalog's flags
  dword at `0x8159AC + 108*id`, reaching the UI through the profile record at +1816 via
  `UI_BuildKeyBindingLoadoutTable @ 0x559e50`), and `is_player_visible` applies the exact
  bit test — replacing the class-category approximation. Observable corrections vs the
  approximation: the analog-only movement entries (`turn_left_abs`, `lookpitch`), `Last_move`,
  `showhud`, `ScopeZero±`, `escape`, and `tod`/`todrate` are hidden like retail; the
  spectator/commander entries show. Bit 0x4000000 (the `default.key` filtered-table
  membership, `KeyBinding_BuildFilteredTable @ 0x54c2b0`) rides the flag word for future use.
- **D-CTRL-3 (read-only):** live double-click rebinding, DEFAULTS/CLEAR_KEY mutation, and profile
  persistence are deferred (no game input-action layer consumes the bindings yet).

- **D-MNU-1 (`%VAR%` mechanism):** per-field build-time expansion vs whole-buffer
  pre-parse - the runtime result matches for stylesheet vars; shell-var-in-text is a
  plumbing follow-up (ADR 0005).
- **D-MNU-2 (sound jitter):** the shared `SoundSelector` reproduces member selection
  but not the interleaved per-play volume/pitch jitter draws (same accepted divergence
  as the mission sound owner).
- **D-MNU-3 (strictness / authoring superset):** the format layer preserves attributes
  the runtime ignores (ADR 0002) and a shell `sound_profile` fallback services file-less
  `<SOUND>` nodes the engine would fail to parse.
- **D-MNU-4 (per-quad int truncation):** the original truncates each scaled element rect
  to int per element (`@ 0x647d40`); the reimpl applies one float CanvasItem scale to the
  whole menu tree - a sub-pixel divergence only.
- **D-MNU-5 (item rendering scope):** shipped menus use image/color items only in
  spinlists; the combo closed cell + popup render text-only (their LIST_BOX items are all
  `type="id"`). The shared `resolve_item` model can back combo/list image/color if a
  future menu needs it.
- **D-MNU-6 (CBIN credits assets):** the marquee CBIN credits scroll with the default
  font; resolving the credits' custom `~F` fonts / `~I` images from the resource root
  (not a disk dir) is a follow-up.
- **D-MNU-7 (combo popup geometry) — FIXED 2026-06-23c:** the original dropdown is the
  embedded `CListWnd` (`this+1536`) whose window RECT (`this+13`) is the authored
  `<LIST_BOX>` `<POSITION>` (combo-relative, design space), drawn over whatever sits
  beneath `[orig: CComboWnd ctor @ 0x65be40; CComboWnd_ParseXMLDefinition @ 0x65c0d0;
  CListWnd_DrawItems @ 0x643f30]`. The reimpl ignored `list_box.position` and recomputed
  the popup at `(0, combo.height)` with `width = combo.width` and a height clamped against
  `get_viewport_rect()` (window pixels mixed with design space). For `player.mnu`
  NATIONALITY (LIST_BOX POSITION `0,65 -> 214,306`, `%SEMIOPAQUE_BLACK%` background) this
  placed the translucent list at `y=20` over the sibling DIVISION/COMBO_LIST combos, whose
  text bled through — the "overlapping dropdown" look. Fixed: `build_combo` passes
  `list_box.position` via `MnuCombo::set_popup_rect`; `open_popup` uses the authored
  rect when present (which also gives PLAYERVOICE its upward open), else the below-combo
  fallback for shell-built combos.
- **D-MNU-8 (combo row-height default) — FIXED 2026-06-23c:** the list row height is the
  font "W" glyph height `[orig: CListWnd_DrawItems @ 0x643f30 -> font_cache_measure_text_default @ 0x653680]`,
  overridden by `this+201` (the `<MI>`/`<MIN_ITEM_HEIGHT>` value; ctor default `-1`
  `[orig: CListWnd ctor @ 0x643bb0]`) only when `>= 0`. The reimpl defaulted to a hardcoded
  16px. Fixed: `MnuCombo::effective_item_height` returns the authored MIN_ITEM_HEIGHT,
  else the item font line height, else 16. The shipped `player.mnu` lists author
  MIN_ITEM_HEIGHT=20, so they were already correct; the default fallback is the latent
  divergence this closes.
- **D-MNU-11 (dropdown input exclusivity) — FIXED 2026-07-16:** while a dropdown is open
  the original routes mouse input exclusively to the open list and closes it on an
  outside press, consumed; only one dropdown opens per scene; screen switches clear it
  (full witness map in "Dropdown input routing" above: `@ 0x63ab00 / 0x63e600 / 0x646290 /
  0x65c190 / 0x63b6b0` over the three `0x31C16CC/D0/D8` globals). The reimpl had NONE of
  this: the popup was a z-lifted child of its combo — Godot picking ignores z_index, so a
  sibling combo built later swallowed hover and clicks anywhere it overlapped the open
  popup (clicking a row there opened the sibling's dropdown instead), there was no
  outside-press close, and popups stacked open — with `player.mnu`'s three combos
  authoring the SAME list rect, several translucent lists could pile onto one region (the
  user-visible "dropdowns overlap" bug). Fixed: full-menu catcher overlay as the menu's
  last child hosting the popup box (picking + draw priority by tree order), the witnessed
  outside-press close/consume + dead-cell rule on the catcher, the MnuMenu single-open
  registry, and close-on-screen-change/exit/hide. Since the 2026-08-10 shell
  cutover the compiled path carries all of this: `MenuFrameCompiler`'s popup
  rect/row hit tests (`engine/runtime/menu/menu_frame.cpp`) + `MenuDriver`'s
  popup lifecycle (`godot/game/menu_driver.gd` — single-open, outside-press
  close/consume, close-on-screen-change/exit); the Control-tree catcher
  overlay described above is historical.
- **D-MNU-12 (popup draw order — reimpl mapping):** the original has NO overlay draw pass —
  the open list draws at its tree position (`CComboWnd_Render @ 0x65bfd0` child walk ->
  `CListWnd_DrawItems @ 0x643f30`), so a later sibling would paint over an open list; the
  shipped menus author every dropdown rect into empty space below/beside the widgets
  (all three `player.mnu` lists share `(0,65)-(214,306)` parent-space), which makes draw
  order unobservable in retail content. The reimpl draws the popup in the menu-top
  overlay because that same node is the input-exclusivity owner — for retail menus the
  result is pixel-equivalent; only a hypothetical mod authoring a dropdown rect over a
  later sibling would see the list above it in the reimpl but below in retail. Kept: the
  overlay is the correct Godot home for the witnessed input model, which is the
  observable contract.
- **D-MNU-14 (SP mission-select population — CLOSED 2026-08-10, witnessed +
  ported):** the boot/reload scan builds a global mission table of
  0x11E8-byte entries (`MissionList_ScanAndBuildFromFiles @ 0x563170`: loose
  `FindFirstFile *.bms` walk, then `Mission_BuildMapListFromPFF @ 0x562910`
  over the archive volumes in localres/language PAIRS — the mission's
  sibling `.bin` is looked up in the PAIRED volume; final order = qsort with
  `Mission_CompareMapNames @ 0x5628e0`, stricmp on the filenames). Per
  entry: filename (+0), display title (+1044: the `.bin`'s case-insensitive
  `[Info] TITLE` via `TextResource_FindEntryBySectionAndKey @ 0x75D250`;
  when NO `.bin` exists the BMS header's embedded `mission_name` (header+4)
  stands in; either miss leaves it empty), the `[Info] BRIEFING` text
  pointer (+1300), the loose-scan flag (+4380: 1 loose / 0 archive), and
  the session code word (+4392: `AI_GetTaskTypeFromFlags(header+0x88)`
  through the code-word switch — our `bms::selected_game_mode` +
  `game_type::for_mission_mode` pair). The SP populate
  (`SinglePlayer_PopulateMissionList @ 0x561840`, a strip-enabling activate
  handler) filters `(gt & 0xFFFDFFFF) == 0x10020` (the waypoint/Co-op
  family), prefixes loose rows `*`, falls back to the FILENAME when the
  title is empty, stores the table index as the row param, CLEARS the
  BRIEFING widget, and disables ACCEPT; selection
  (`SinglePlayer_MissionListEventHandler @ 0x561ed0`, events
  0x5000001/0x5000002) sets the current-entry global, fills BRIEFING from
  the entry's briefing pointer, re-enables ACCEPT, and double-click starts
  the mission; screen activation re-syncs ACCEPT to whether the list has a
  selection (`SinglePlayer_RefreshAcceptOnActivate @ 0x561a20` over
  `UIList_CountSelectedItems @ 0x6445c0` — style-slot-3 rows; both defined
  + named in the IDB 2026-08-10). Reimpl:
  `engine/runtime/mission/mission_catalog.{h,cpp}` (the `.bin` resolves
  through the mount stack rather than the paired volume — identical on
  retail data; `.npj`/`.npz` legs not ported) + the `MissionCatalog`
  binding (the code-word stamp) + `menu_shell.gd`'s SP seeding
  (`_seed_mission_list` clears the reimpl's row-0 preselect to match the
  witnessed no-selection populate). Pinned by `mission_catalog` ctest +
  the shell GUT SP case over the retail `00tra.bin` fixture. The HOST
  screen's populate/filter chain is split off as D-MNU-17.
- **D-MNU-17 (host-screen mission population — FIXED 2026-08-10, core):** the
  full chain is witnessed and the populate/filter/selection core is ported.
  `init_host_settings_dialog @ 0x558960` (one-shot, guard `@ 0x25C76DC`):
  clears the per-mission selected flags (entry+4412), registers the 17
  control handlers, selects the GENERAL tab, populates settings/weapon/class
  lists, DISABLES `START_GAME`, clears both mission widgets, then fills
  `MISSION_LIST` — row text = title(+1044) else filename, row value = the
  mission index, EXCLUDING the stock (non-objective) Co-op family
  (`(code & 0xFFFDFFFF) == 0x10020` without `0x20000` — the pure-SP/training
  missions, `@ 0x558a70`) — and enables only the `GAME_TYPE` spin values at
  least one mission maps to (the shared 13-way code→category switch:
  0→11, TDM→1, objective-Co-op→2, TKOTH→3, KOTH→4, SD→5, AD→6, CTF→7, FB→8,
  8→12, AAS→9, CAC→10; 255 = ALL, always on, selected at init).
  `filter_mission_list_by_game_type @ 0x556fe0` (the spin event): hide all
  rows, re-show those whose mapped category matches (or 255) minus the
  already-selected set (+4412). `HostDialog_AddRemoveSelectedMissions
  @ 0x557c10` (ADD_MISSIONS +1 / REMOVE_MISSIONS −1; was an unowned shared
  tail chunk of `0x557f70`/`0x557fb0` — THE reason `sub_557FB0` never
  decompiled; boundaries repaired 2026-08-10): ADD walks the SELECTED list
  rows into `SELECTED_MISSIONS` — cells = title-else-filename /
  `GameText("GateTypeAbbrev", key)` (keys DM/TDM/KOTH/TKOTH/CTF/SD/AD/FB/FM/
  AAS/CAC + COOP for the waypoint family, `get_game_type_abbreviation
  @ 0x520fc0`) / the rotation "Switch" cell defaulting ON for team games
  without the objective bit (`(code & 0x10000) && !(code & 0x20000)`,
  entry+4416) — marks +4412 and hides the list row; REMOVE restores through
  the same category filter. `HostDialog_MissionListDoubleClick @ 0x557f70` =
  select + ADD; `HostDialog_SelectedMissionsTableEvent @ 0x557fb0` = the
  column-2 rotation toggle (eligible codes only) + double-click REMOVE.
  The common tail arms `START_GAME` only while the table has rows (plus the
  `0x25510A4..CC` config-word gate). `UI_HandleHostSessionStart @ 0x556d00`:
  queues every table row into the rotation (`sub_501960` +
  `Server_QueueEntityAction`), the FIRST row stamps `g_map_file_name` /
  `missionData` / its rotation flag / `g_GameType`(+4392), then the
  SERVERTYPE arm (1 = NovaWorld HTTP hosting, 2 = LAN session).
  **Port:** the witnessed rules live engine-side in `npwire/game_type.h`
  (`host_list_visible` / `host_filter_category` / `host_abbreviation_key` /
  `host_rotation_default`, pinned by `game_type_policy` ctest) through the
  `NetProtocol` binding; `mp_menu_companion.gd` seeds the host pool from
  `MissionCatalog.rows` (display = title-else-filename), filters on the
  GAME_TYPE spin value (ALL = 255 when unauthored), fills the rotation table
  columns (localized abbreviation with the key fallback), hides/restores
  list rows across ADD/REMOVE, and gates START_GAME on a non-empty rotation;
  the start config carries the rotation's FILE names with the head as the
  mission. Pinned by `mp_lan_menu_seam_test.gd`. Residues: the per-item
  GAME_TYPE spin enablement, the rotation-cell click toggle + double-click
  add/remove (the compiled table has no per-cell click surface yet), the
  weapon/class restriction lists, the `0x25510A4` config-word gate, and the
  rotation beyond its head (the runtime plays one mission; the rotation
  system is unported).
- **D-MNU-15 (combobox closed face — FIXED 2026-08-10):** the compiled combo's
  face gated on AUTHORED items only, so every runtime-seeded combo (the
  armory's ten companion-filled PRIMARY/SECONDARY/ACCESSORY/GRENADE ammo
  tuples, observed live 2026-08-10) fell through to the empty widget-text
  label while spinlist faces (the shared `emit_item_cell`, which handles
  runtime rows) drew fine. The face now takes the item cell whenever rows
  exist — authored OR runtime (`ws->has_items`) — matching retail's face,
  a `CButtonWnd` showing the selection (`CComboWnd ctor @ 0x65be40`, closed
  button at `this+764`). Pinned by the `menu_frame_compiler` runtime-rows
  face case.
- **D-MNU-16 (spinlist arrows unhittable — FIXED 2026-08-10):** the witnessed
  pump claims by widget rect (front-most LAST-hit of the forward draw walk),
  but retail's `<SPINUP>`/`<SPINDOWN>` are child windows carrying their own
  rects in that walk (`CSpinListWnd_CreateUpDownChildren @ 0x64b8b0`,
  parent-relative POSITION), and shipped menus author them OUTSIDE the parent
  rect (mp.mnu GAME_TYPE: left arrow −18..−2, right 217..233 against a
  0..215-wide widget) — so no arrows-outside spin combo could cycle by mouse
  (observed live 2026-08-10: GAME_TYPE presses at both arrow rects fell
  through to MAIN). The claim walk now tests the spin-button rects too (the
  shared `spin_arrow_hit_` behind `spin_arrow_at`), the compiled equivalent
  of the child-window claim retail gets for free; the driver's existing
  press routing then cycles. Pinned by the `menu_frame_compiler`
  outside-rect claim case.
- **D-MNU-19 (activation vs scripted ACTION order — KEPT, reimpl-structural):**
  retail runs a widget's ACTION list first and its registered callback last
  (`CUIWidget_HandleScriptedAction @ 0x6497f0` walks the list, then calls the
  callback `@ 0x649c7d`); the reimpl deliberately inverts — `widget_activated`
  emits first — because its shell REPLACES the document on a cross-`.mnu`
  jump, and it guards the dispatch against a document swap during the emit
  (`menu_driver.gd _emit_activated_then_dispatch`). The full story is the
  "Activation vs scripted ACTION order" paragraph in the popup section above.

**IDB changes (2026-08-10, the D-MNU-17 host-dialog walk; saved):** repaired
the function boundaries at `0x557c10..0x557f6a` (an unowned tail chunk shared
by BOTH `0x557f70` and `0x557fb0` — the reason `sub_557FB0` never decompiled);
renamed `sub_557C10 → HostDialog_AddRemoveSelectedMissions`, `sub_557F70 →
HostDialog_MissionListDoubleClick`, `sub_557FB0 →
HostDialog_SelectedMissionsTableEvent`, and the widget helpers `sub_63F3C0/
63F3D0/63F5C0/63F5F0 → CTableWnd_GetRowCount/GetRowValue/IsRowSelected/
SetRowSelected`, `sub_644580/6445F0/644A00/645150 → CListWnd_GetRowCount/
GetRowValue/SetRowEnabled/IsRowSelected`, `sub_642750 → CTableWnd_AddRow`,
and the misnomer `CPreprocessor_SetCellTexture @ 0x63edf0 →
CTableWnd_SetCellText`; entry comments on the four HostDialog handlers +
`init_host_settings_dialog` + `UI_HandleHostSessionStart`.

## Scroll interaction `[orig: CScrollWnd_HandleEvent @ 0x64d050]` (2026-08-11)

The standalone CScrollWnd input map: **arrows** (`SCROLLWND_UP`/`SCROLLWND_DOWN`
clicks) step `value -/+ step` (`+0xBFC`, ctor default 1 `@ 0x64c4cf`); a **track
press** pages toward the click (`value -/+ page`, `+0xC00` via `CScrollWnd_SetPageSize
@ 0x64ce10`, ctor default 10 `@ 0x64c4d9`; direction by the click's side of the
shuttle `@ 0x64d0ca..0x64d10e`); a **shuttle press** anchors `this[763] =
shuttle_origin - track_base - mouse` (`@ 0x64d1cb..0x64d217`) and, while the shuttle
child holds press-capture, every move maps `value = min + (mouse + anchor) / ratio`
clamped to `[min, max]` (`@ 0x64d231..0x64d2aa`; the ratio is the px-per-unit double
at `+0xC08`, whose forward map is `CScrollWnd_UpdateThumbPosition @ 0x64cd50`). Every
change re-lays the thumb and dispatches `0x4000001` with the new value (`@ 0x64d15c`).
Reimpl: the whole interaction lives in the compiler's mouse pump
(`MenuFrameCompiler::scroll_pump_mouse_` over `scroll_hit_at`/
`scroll_drag_anchor`/`scroll_drag_value` and the shared `solve_scroll_parts_`
geometry, `engine/runtime/menu/menu_frame_scrollbar.cpp`): the pump runs it
ahead of the claim walk, the pressed part keeps the mouse until release
(retail's child-window press-capture), and value changes surface through
`MenuFrame`'s `scroll_value_changed` signal, which `MenuDriver` mirrors and
relays. Embedded Table/List scrollbars ride the same parts with the rows
model (range `0..rows-visible`, page `visible-1`, value = first visible
row — the table SCROLLBAR delegate `@ 0x643b22`). The menu cursor is the OS
custom cursor carrying the claim's retail texture; the compiled software
cursor stays off in the game shell (drawing both showed a trailing second
cursor).

## Controls key-remap flow `[orig: UI_ControlsRemapArmHandler @ 0x55d560; the capture pump @ 0x55c67c]`

The OPTIONS scene registers per-widget callbacks (`@ 0x55d737..0x55d827`):
`CONTROL_MAPPING` -> the arm handler `UI_ControlsRemapArmHandler @ 0x55d560`, `DEFAULTS` ->
`@ 0x55bd90`, `CLEAR_KEY` -> `@ 0x55bfd0`. The arm fires on the table
activation event `0x5000002` OR a click on the already-stored row: it sets the
options pump to remap state, stores the row's value (the catalog index) in the
capture global, flushes the input queue, CLEARS the row's Control cell
(`CTableWnd_SetCellText @ 0x63edf0`), takes focus, and for the mouse device
registers a button callback (`sub_7613B0`). The pump (`@ 0x55c67c`, inside
`sub_55C450`) then consumes input by device: Esc restores the display and ends
the capture (`update_control_mapping_display @ 0x55b700` — formats the bound
control per device, resets both capture globals to -1); keyboard keys drain
into `KeyBinding_HandleKeyAssignment @ 0x55bb20`; the joystick page polls the
button bitfield and writes `button+1` into the record byte (`@ 0x55c712`).

The live records are 432-byte entries (base `0x25C772C`): +8 primary scan
(`g_keybind_slot1_scan`), +10 secondary scan, +12/+14 the per-slot MODIFIER
VK words (`g_keybind_slot1_modifier`/`slot2` — 2026-08-11: an earlier
"extended-flag" reading is FALSIFIED; the word is 17 = VK_CONTROL rendering
the "Ctrl-" display prefix, 16 = VK_SHIFT rendering "Shift-", 0 = none
`[orig: KeyBinding_FormatBindingString @ 0x559a10]`), +16 mouse mask,
+20/+21 joystick bytes. The capture's key events ride the 128-slot circular
keyboard queue (`Input_QueueKeyEvent @ 0x760c10` -> `Input_DequeueKeyEvent
@ 0x760d60`): byte 0 = VK, flag word = 0x800 Ctrl held + 0x200 Shift held +
0x100 extended (lParam bit 24) + 0x80 auto-repeat (lParam bit 30). Assignment
semantics (`@ 0x55bb20`): VK `0x11` with the Ctrl-held flag is dropped — the
`g_input_ctrl_down` state is set BEFORE its own event enqueues, so EVERY
Ctrl press arrives flagged and the Ctrl key itself can never be captured;
VK `0xDE` is dropped; the slot modifier becomes 17 iff the flag word is
EXACTLY 0x800 (Ctrl held alone — a shift/extended/repeat flag defeats it);
the keypad Enter captures as scan 269; re-assigning a held (scan, modifier)
collapses the record to that key as the sole primary; otherwise the key
fills the empty slot, or replaces the PRIMARY when both are full. The
mouse callback (`@ 0x55c780`) maps events to masks (LMB 1, RMB 2, MMB 0x10,
wheel up 0x400, wheel down 0x800), writes the record's mouse word, and
unhooks. `CLEAR_KEY` clears the SELECTED row's slots for the active device;
`DEFAULTS` qsorts by id, re-copies the 72-byte runtime default array at
`0x254CC2C`, reformats, and resets the profile mouse-sensitivity/invert
fields (`profile+0x590 = 0x80`, +0x594.. = 0). Bindings persist inside the
player profile: the 432-byte records copy to `profile+1808` (72-byte stride,
count at +1804, `@ 0x559d50`) and `PlayerProfile_SaveToFiles @ 0x54be00`
writes player.sav ("FPBC0211" 16-byte header + five 15488-byte records + an
8-byte trailer).

Reimpl: `engine/runtime/controls/binding_set.*` (records + assignment/clear/
defaults semantics, including the modifier word and the extended-VK
derivation), the `ControlsModel` binding (VK <-> Godot key seam, the
button->mask translation, and `is_token_pressed` — the one gameplay sampling
call: keyboard slots gated on their modifier plus the held-sampleable
L/R/M mouse-mask buttons; wheel masks are impulse-only and display/persist
without sampling), `controls_bindings.gd` (the shared live model +
persistence), `menu_shell.gd` (arm/capture/cancel + DEFAULTS/CLEAR_KEY;
a screen change cancels an armed capture like retail's screen-owned pump
state), and `player_input_router.gd` samples gameplay input through the live
records. Divergences: persistence rides `user://controls.cfg` until the
player.sav profile format slice exists, and the joystick capture page is not
wired (both under D-CTRL rows). The retail arm also fires on a single click
of the already-selected row; the reimpl arms on the driver's double-click
activation (the shipped REMAP_INSTRUCTION text documents double-click).

Deferred (unwitnessed or out of bar; backlog, not blocking):

- The binding DATA side of the D-CTRL family remains: D-CTRL-1 (the
  mouse/joystick default binding arrays are an RE hunt), the player.sav
  profile-record format (only its geometry is witnessed), and the
  refresh pass's yellow active-binding highlight
  (`refresh_control_mapping_list @ 0x55b320`, unwalked interior).
- Live data/behavior for `GLB_TABLE`, `LAN_LIST`, and `GOPHER` remains owned by
  the multiplayer/news hosts. Their authored menu structure and Action payloads
  are preserved; this menu-contained pass does not invent offline services.
- Real 3D globe and CBIN credits custom-resource font/image resolution remain
  separate render/data-supply work (D-MNU-6 covers the latter).

---

## Appendix: IDA correspondence (reverse citations)

Consolidated from `notes/proposed-ida-edits.md` on 2026-06-10; reimpl symbols
re-verified against the current sources and the Kong IDB on the same date.

This is the symbol-authority record for the menu system: every original handler in
`Jointops.exe` mapped to the reimplementation that answers for it. The forward leg
already lives as `// [orig: Name @ 0xADDR]` markers in the reimpl sources; the
matching `reimpl:` comments on the IDB entry addresses below are PROPOSED, not yet
applied (the IDB is shared state — apply manually via `set_comments`, reversible).

### Handler cross-links

| Original | OpenNova |
|---|---|
| `UIScene_LoadAndParseContent @ 0x63c830` | menu load path: `MnuDocument` + `godot/game/menu_shell.gd` |
| `Menu_RenderFrame @ 0x54b7c0` -> `CUIScene_DrawScreensAndCursor @ 0x63bf60` | the scene draw walk -> `MenuFrameCompiler::compile` + interleaved `MenuDrawList::draw_ops` — `engine/runtime/menu/menu_frame.cpp` |
| `CUIElement_Draw @ 0x64a8a0` / `CStaticWnd_Render @ 0x657b10` (the Draw vtable family) | the per-widget and cross-kind painter order — `MenuFrameCompiler::walk_widget` -> `draw_ops` -> `MenuFrame::_draw` |
| `CWnd_SetVisualState @ 0x646340` + `widget_process_mouse_event @ 0x647a00` (state write +236) | `MenuFrameCompiler::visual_state_for` + `MenuWidgetState` |
| `CWnd_GetFontAndColors @ 0x646a70` (vtable+64 draw-time font/color inheritance) | `MenuFrameCompiler` `WidgetNode::font/colors` |
| `CStaticWnd_DrawLabel @ 0x656fb0` + `draw_text_with_cursor @ 0x6533b0` + `font_cache_draw_text_scaled @ 0x653170` | `MenuFrameCompiler::emit_widget_text` / `emit_caret` over `opennova::hud::GameFont` |
| `CEditWnd_Render @ 0x6619e0` (focus state-2, blink, password, scroll window `update_edit_scroll_range @ 0x661790`) | the compiler's edit leg + `MenuWidgetState.focused/caret` |
| `CRadioWnd_Render @ 0x656e20` / `CCheckWnd_Render @ 0x64ae20` + `CCheckWnd_DrawLabel @ 0x64aa20` | checked-state forcing + label placement in the compiler |
| `Menu_InitShellResources @ 0x552500` → `NapiConfigMap_LoadIncludeFile @ 0x63b970` → `parse_key_value_buffer @ 0x639870` | `mns::Document::evaluate` — `engine/formats/mns/src/mns_document.cpp` (witnessed runtime evaluator) plus the separate lossless document model (ADR 0014) |
| `XML_ParseWithBOMDetection @ 0x76a690` | `mnu_xml::parse` + `skip_bom` — `engine/formats/mnu/src/mnu_xml.cpp` |
| `XML_ParseCharEntity @ 0x769cc0` | `mnu_xml::decode_entity` — `engine/formats/mnu/src/mnu_xml.cpp` (faithful to the engine's non-standard policy: no `&apos;`, decimal-only `&#`, Latin-1 named set) |
| `NapiXML_ExpandVariablesInText @ 0x63a000` | `MnsStyleSheet::substitute` — `godot/src/mnu/mns_stylesheet.cpp` (per-field post-parse, not whole-buffer; D-MNU-1 / ADR 0005) |
| `parse_scene_node_attributes @ 0x639630` | `mnu::parse_screen` — `engine/formats/mnu/src/mnu.cpp` |
| `CUIElement_ParseXMLDefinition @ 0x648120` | `mnu::parse_window` — `engine/formats/mnu/src/mnu.cpp`; layout in `MenuFrameCompiler::solve_rect` — `engine/runtime/menu/menu_frame.cpp` |
| `parse_edit_widget_xml_properties @ 0x661d10` | EDIT attrs (`NUMBER/MINVAL/MAXVAL/MAXCHAR/READONLY/PASSWORD`) in `mnu::parse_window` |
| `sub_64AD90 @ 0x64ad90` (CHECKBOX attr parse) | CHECKBOX attrs (`AS_BUTTON/CHECKED`) in `mnu::parse_window` |
| `CScrollWnd_Construct @ 0x64c450` + `CUIScrollWidget_ParseExtendedXMLDef @ 0x64c6d0` | SCROLL orientation and one along-axis part extent: constructor default 20, with authored `WIDTH`/`HEIGHT` override; texture size is not the extent source |
| `CUIScene_CreateWidgetByType @ 0x64f630` | `mnu::parse_type_string` / `window_type_name` — `engine/formats/mnu/src/mnu.cpp` |
| `CTableWnd_ParseXMLContentDefinition @ 0x6427d0` | `mnu::parse_table_*` — `engine/formats/mnu/src/mnu.cpp` |
| `CListWnd_ParseXMLDefinition @ 0x645770` | `mnu::parse_listbox` / direct list fields — `engine/formats/mnu/src/mnu.cpp`; LIST/MULTI/LAN_LIST take sibling top-level `MIN_ITEM_HEIGHT`, COMBO takes it from nested `LIST_BOX` |
| `CListWnd_Construct @ 0x643bb0` (embedded `CScrollWnd@+976`; row-height sentinel `this+201 = -1`) | popup row defaults — `MenuFrameCompiler::row_height_` (`engine/runtime/menu/menu_frame.cpp`, authored `MIN_ITEM_HEIGHT` sentinel) (D-MNU-8) |
| `CListWnd_DrawItems @ 0x643f30` (rows inside `this+13`; row height = font "W" or `this+201`; per-row text truncation) | `MenuFrameCompiler::emit_combo_popup` (`engine/runtime/menu/menu_frame_scrollbar.cpp`) + `row_height_` (`menu_frame.cpp`) (D-MNU-7/8) |
| `CScrollWnd_Render @ 0x64c5c0` (COLOR sink `@ 0x64ce70`, IMAGE sink `@ 0x64cf70`, outline via `CUIElement_DrawOutlineRect @ 0x647fc0`) + `CUIScrollbar_CreateChildWindows @ 0x64d330` | `MenuFrameCompiler::emit_scrollbar` (`engine/runtime/menu/menu_frame_scrollbar.cpp`) — COLOR/OUTLINE full rect, IMAGE middle inset, then SHUTTLE/SCROLLUP/SCROLLDOWN painter order |
| `CUIScrollbar_CalcThumbRect @ 0x64cba0` + `CScrollWnd_SetPageSize @ 0x64ce10` + `CScrollWnd_SetRangeAndClamp @ 0x64d490` + `CScrollWnd_SetScrollPos @ 0x64ce20` | proportional thumb with 20px minimum and clamped min/max/page/value; embedded owners hide-on-fit, while standalone state crosses `MenuWidgetState` -> `MenuFrame::set_widget_scroll_range` -> `MenuDriver.set_widget_scroll_range`; direct input/per-part state remains D-MNU-13 |
| `options_screen_init @ 0x554800` + `UI_PopulateRenderAndAudioSettings @ 0x55c830` | `MenuOptionScrollPolicy.apply` (`godot/game/menu_option_scroll_policy.gd`, invoked by `MenuShell`) — GAMMA 5..20/page 2; SOUNDFXVOLUME/DIALOGVOLUME/MUSICVOLUME 0..255/page 10; MOUSE_SENSITIVITY 4..511/page 10; current=min only until persisted setting values are modeled |
| `CListWnd_CreateScrollChild @ 0x6444c0` / `CMEditWnd_CreateScrollChild @ 0x661260` / `CTableWnd_Init @ 0x640790` | an absent/zero-width embedded scrollbar POSITION falls back to the rightmost 22px of the owner's full height |
| `CComboWnd_ParseXMLDefinition @ 0x65c0d0` (feeds `<LIST_BOX>` to embedded `CListWnd` `this+384`) | `mnu::parse_window`'s LIST_BOX + `MenuFrameCompiler::combo_popup_rect` (authored combo-relative POSITION; `SB_EDGE_PAD` narrows content only, not scrollbar geometry) (D-MNU-7) |
| `CUIElement_DrawFrame @ 0x64a210` + `init_border_materials @ 0x646f70` | `MenuFrameCompiler::emit_frame` — 8 border quads + tiled fill; retail-neutral 0x7F modulate-2x becomes effective white for Godot ordinary multiply; draws nothing when textures are absent; no monogram |
| `CStaticWnd_Render @ 0x657b10` | the base window render order (frame -> appearance -> text -> children); the frame pass is gated on the DRAW_FRAME flag (`elem+0x134`) -> the compiler's draw walk gates `emit_frame` on `w.draw_frame`; confirms the menu monogram is never drawn |
| `CUIScene_SetScreenScale @ 0x639480` (was `sub_639480`) | 800x600 anamorphic scale -> `_recompute_fit` in `menu_shell.gd` |
| `CWnd_SetScaleRecursive @ 0x646c60` | scale propagation (root CanvasItem `set_scale`) |
| `CUIElement_ParseXMLDefinition @ 0x648120` + `CUIElement_DrawTextureNative @ 0x647e40` -> `CTextureManager_DrawScaledRect @ 0x654e60` | `MenuFrameCompiler::emit_state_texture` — IMAGE stretched into the solved rect, with authored `MAP_STATE`/`HEIGHT` retained as the cropped atlas source band |
| `CUIElement_DrawStretchedTexture @ 0x647d40` | the compiler's stretched-quad emit (texture into the solved rect); per-element int truncation of scaled coordinates is compiled (closes D-MNU-4's divergence) |
| `CWnd_AccumulateAncestorOffset @ 0x6465e0` (was `sub_6465E0`) | Godot parent-child nesting (positions are parent-relative) |
| `CSpinListWnd_Render @ 0x64b220` + `CUISpinList_ParseXMLDefinition @ 0x64bd10` | `resolve_item` + `mnu_render_item_cell` (`mnu_item_cell.{h,cpp}`) + `build_spinlist` |
| `CSpinListWnd_CreateUpDownChildren @ 0x64b8b0` | `MenuFrameCompiler::emit_spin_arrows` + the shared `spin_arrow_hit_` claim (parent-relative SPINUP/SPINDOWN) — `engine/runtime/menu/menu_frame.cpp` (D-MNU-16) |
| `CComboWnd_Construct @ 0x65be40` + `CComboWnd_Render @ 0x65bfd0` | the compiled combo face + popup — `MenuFrameCompiler::emit_combo_popup`/`combo_popup_rect` (dropdown geometry from authored LIST_BOX POSITION, D-MNU-7; the closed face pins D-MNU-15) |
| `dispatch_mouse_event @ 0x63ab00` (WM `0x200..0x20A` -> ids `0x1000001..0x100000B`; exclusive route to `g_ui_open_popup_wnd` `@ 0x63abb5`) | an open dropdown owns the mouse exclusively — `MenuDriver.process_mouse`'s popup branch over `MenuFrameCompiler::combo_popup_row_at`/`combo_popup_contains` (`godot/game/menu_driver.gd`, D-MNU-11) |
| `scene_end_frame @ 0x63e600` (frame pump only the open popup `@ 0x63e691`; clears the per-frame mouse claim `scene+16` `@ 0x63e67e`) | `MenuFrameCompiler::pump_mouse` — the per-frame single-claim walk (`engine/runtime/menu/menu_frame.cpp`) |
| `CWnd_IsVisibleInHierarchy @ 0x646290` (popup-subtree-only while a popup is open `@ 0x646299`) | popup-open frames bypass the widget claim walk entirely (`MenuDriver.process_mouse` returns from the popup branch); hidden subtrees never hit in the claim walk |
| `combobox_handle_event @ 0x65c190` (toggle `0x3000001`; single-open `@ 0x65c210`; outside-press close `@ 0x65c261`; `LISTBOX_WND` `0x5000001` pick `@ 0x65c2fd`) | `MenuDriver` combo handling — toggle on activate, `_open_combo_popup`/`close_active_combo_popup` single-open, outside-press close, row pick -> `_combo_select` (D-MNU-11) |
| `CWnd_SetShown @ 0x6480e0` (shown flag `+224`; popup flag `+660` registers `g_ui_open_popup_wnd`; was `sub_6480E0`) | popup lifecycle = `set_widget_popup_open` on the frame state (`MenuDriver._open_combo_popup`/`close_active_combo_popup`) |
| `CUIScene_SelectNodeByName @ 0x63b6b0` (screen switch clears the popup + capture globals `@ 0x63b6b8/0x63b7c4`) | `MenuDriver` screen changes -> `close_active_combo_popup` |
| `dispatch_mouse_event_to_children @ 0x647900` (active-combo priority peek `@ 0x647917`; press-capture bypass `@ 0x647932`) + `widget_process_mouse_event @ 0x647a00` (reverse child walk; per-frame claim `scene+16`) | Godot viewport GUI picking (reverse tree order) — reimpl code / not grillable |
| `CButtonWnd_HandleNamedEvent @ 0x658340` (press sets `g_ui_mouse_capture_wnd` `@ 0x65839c`, release clears `@ 0x6583ed`; pressed-texture swap; was `sub_658340`) | Godot `BaseButton` press capture — reimpl code / not grillable |
| `CWnd_EmitEventToNamedHandlerAndCallbacks @ 0x646970` (+28 sink -> +32 with own name + callback chain by `1<<HIBYTE(event)`; was `sub_646970`) | Godot signals (`pressed`/`gui_input`) replace the named-event plumbing — reimpl code / not grillable |
| `CWnd_SetParentAndAttach @ 0x6480a0` (parent ptr `+252` + child-array attach; was `sub_6480A0`) | Godot `add_child` — reimpl code / not grillable |
| `CMarqueeWnd_Construct @ 0x65c430` + `CMarqueeWnd_ParseXMLDefinition @ 0x65ceb0` + `marquee_load_credits_from_ini @ 0x65c5a0` | `build_marquee` -> `CreditsPlayer` + `CbinCreditsResource::from_cbin_bytes` (CBIN datasource); `MnuMarquee` (plain text) |
| `CUIWidget_HandleScriptedAction @ 0x6497f0` | `MenuDriver._dispatch_widget_actions` + the shell action signals — `godot/game/menu_driver.gd` |
| `UI_PopulateControlMappingList @ 0x55c0c0` + `refresh_control_mapping_list @ 0x55b320` | `opennova::controls::build_rows` (`engine/runtime/controls/src/controls.cpp`) + `menu_shell.gd::_fill_control_mapping` |
| `UI_BuildKeyBindingLoadoutTable @ 0x559e50` (catalog `aAbsoluteTurnLe @ 0x8159cb`) | `engine/runtime/controls` `k_catalog` — `controls.cpp` |
| `KeyBinding_BuildCategoryPages @ 0x4966c0` (Class id -> name) | `controls::action_class_name` |
| `KeyBinding_GetKeyNameAndDisplayName @ 0x494c60` (VK -> display name) | `controls::key_name` |
| `KeyBinding_FormatBindingString @ 0x559a10` (`Ctrl-`/`Shift-`/`OR`) | `controls::format_binding` |
| `UI_SelectControlsInputDevice @ 0x55bcd0` (device-mode radio, sets `dword_25db7d8`) | Keyboard/Mouse/Joystick radio wiring — `menu_shell.gd::_seed_control_mapping` |
| `CTableWnd_ParseXMLContentDefinition @ 0x6427d0` (header `type="id"` `@ 0x64344a`, SCROLLBAR delegate `@ 0x643b22`) + `CUITable_Render @ 0x6411d0` | `mnu::parse_table_*` + `MenuFrameCompiler::emit_table` — FONT "W" header height, separate top-level-MIN_ITEM_HEIGHT body rows/page, full-height authored-or-22px scrollbar rect, default-state art/thumb geometry |
| `ControlsModel` (Godot wrapper) | `godot/src/mnu/controls_model.cpp` |
| `Input_HandleActionBinding_0 case 0xB1 @ 0x4e0b3f` (useitem armory leg) + `Input_HandleActionBinding case 218 @ 0x49b83d` | the shell armory key (SHIFT) + `_try_open_armory` — `main_game.gd` |
| `UI_InitWeaponClassSelection @ 0x567250` (CHARCLASS_* rows, values 5..9) | `ArmoryMenuCompanion._populate_classes` |
| `Armory_ResolveSelectedClass @ 0x5642f0` + `SpinList_SelectItemByValue @ 0x64ba50` | `ArmoryMenuCompanion._resolve_selected_class` + select-by-value |
| `populate_three_category_lists @ 0x566db0` (+ `ListWidget_SortRows @ 0x644990` / `cmp @ 0x6448a0`) | `ArmoryMenuCompanion._populate_slots/_fill_slot` (sorted rows, NONE at 0) |
| `update_weapon_weight_display @ 0x565640` + `calculate_equipped_weapons_weight @ 0x565490` | `ArmoryMenuCompanion._update_weight` (witnessed format + encumbrance bands; D-MNU-9 on the ammo model) |
| `WeaponLoadout_ApplyFromBuffer @ 0x565cd0` (ACCEPT/CANCEL, `skip_apply` arg) | `ArmoryMenuCompanion._on_accept/_on_cancel` -> the shell's SP apply |

IDB state note (2026-06-23): the 2026-06-23 render grill renamed `sub_639480 ->
CUIScene_SetScreenScale`, `sub_6465E0 -> CWnd_AccumulateAncestorOffset`, `CUIScene_GetActiveScreenName ->
CUIScene_GetActiveScreenName`, `sub_647E40 -> CUIElement_DrawTextureNative`, `CTextureManager_DrawScaledRect
-> CTextureManager_DrawScaledRect`, `sub_65BE40 -> CComboWnd_Construct`, and `CMarqueeWnd_ParseXMLDefinition
-> CMarqueeWnd_ParseXMLDefinition` (all anchored). `0x64ad90` is still unnamed
(`sub_64AD90`) and `0x6497f0` folds into the `0x648120` body (vtable-reached, no direct
xrefs); naming/splitting them remains a proposed edit.

IDB state note (2026-06-23c combo-dropdown grill): renamed `sub_65C0D0 ->
CComboWnd_ParseXMLDefinition` (`0x65c0d0`, anchored: combo vtable[15] parse slot, delegates
to `CUIButtonWidget_ParseXMLAttributes` and feeds the embedded `CListWnd` at `this+384`).
Comments added and `idb_save` done: `0x644070` (row height = `this+201` / font-W default),
`0x644060` (`row_rect = this+13` from `<LIST_BOX>` POSITION), `0x65c16e` (combo parse feeds
the embedded `CListWnd` `this+384`). `CListWnd_DrawItems @ 0x643f30` and `CListWnd_Construct
@ 0x643bb0` were already curated-named and left as-is.

IDB state note (2026-08-09 draw-walk grill): renamed, all anchored —
`sub_656FB0 -> CStaticWnd_DrawLabel`, `sub_646340 -> CWnd_SetVisualState`,
`sub_646A70 -> CWnd_GetFontAndColors`, `sub_6472A0 -> CWnd_ApplyClipViewport`,
`sub_647190 -> CWnd_FindInheritedFrameBlock`, `sub_647FC0 -> CUIElement_DrawOutlineRect`,
`sub_647F10 -> CUIElement_DispatchCustomDrawEvent`, `sub_656E20 -> CRadioWnd_Render`,
the misnamed `CWnd_SetEnabled -> CWnd_SetChecked` (`0x656e80`, writes +772),
`CEditWnd_Render_0 -> CEditWnd_Render` (`0x6619e0`, the single-line EDIT) and the old
`CEditWnd_Render -> CMEditWnd_Render` (`0x6608e0`, the MULTILINE_EDIT), the FLIRT-misnamed
`CHLSLParser_AllocToken -> EditWnd_CountCharsFitting` (`0x6616b0`),
`sub_63BF60 -> CUIScene_DrawScreensAndCursor`, `sub_54B7C0 -> Menu_RenderFrame`,
`0x5528a0` newly defined as `Menu_UpdateFrame`, `sub_676290 -> CGameFont_DrawText_Cdecl`,
`sub_6461F0 -> UI_EmitEventToFocusWnd`, `sub_646420 -> UI_SetFocusWnd`,
`sub_646430 -> UI_ClearFocusWnd`, `sub_653680 -> font_cache_measure_text_default`;
data `dword_31C16D4 -> g_ui_focus_wnd`, `dword_31C16DC -> g_ui_mouseover_wnd`,
`dword_31C16E0 -> g_ui_frame_cursor_texture`, `dword_2551100 -> g_GameMenu`,
`dword_2551114 -> g_menu_render_dirty`, `dword_31C3760 -> g_ui_half_bright_mode`.
Witness comments at `0x64a8a0`, `0x646340`, `0x647a00`, `0x6533b0`, `0x653170`,
`0x656fb0`, `0x6619e0`, `0x64ae20`, `0x64aa20`, `0x656e20`, `0x63bf60`, `0x54b7c0`,
`0x647e40`, `0x6483d4`, `0x646a70`, `0x643f30`; `idb_save` done.

IDB state note (2026-07-16 dropdown-input grill): renamed, all anchored —
`sub_6463C0 -> UI_SetMouseCaptureWnd`, `sub_6463D0 -> UI_ClearMouseCaptureWnd`,
`sub_6463E0 -> UI_SetActiveComboWnd`, `sub_646400 -> UI_ClearActiveComboWnd`,
`sub_6463F0 -> UI_SetOpenPopupWnd`, `sub_646410 -> UI_ClearOpenPopupWnd`,
`sub_6480E0 -> CWnd_SetShown`, the FLIRT-misnamed
`UMSSchedulerProxy::GetTransferListEvent -> CWnd_IsShown` (`0x646280`, returns `this[56]`),
`sub_6480A0 -> CWnd_SetParentAndAttach`, `sub_658340 -> CButtonWnd_HandleNamedEvent`,
`sub_64AB80 -> CCheckboxWnd_HandleNamedEvent`, `CWnd_EmitEventToNamedHandlerAndCallbacks ->
CWnd_EmitEventToNamedHandlerAndCallbacks`, `sub_6471C0 -> CWnd_MarkDirtyWithChildren`;
data `dword_31C16CC -> g_ui_mouse_capture_wnd`, `dword_31C16D0 -> g_ui_active_combo_wnd`,
`dword_31C16D8 -> g_ui_open_popup_wnd`, and (2026-08-09 draw-walk grill) `dword_31C16D4 ->
g_ui_focus_wnd` (keyboard focus — the caret gate), `dword_31C16DC -> g_ui_mouseover_wnd`
(the per-frame hovered widget; both cleared alongside capture by scripted actions
`@ 0x6498c8/0x6498d4`, witnessed via the debug formatter `@ 0x6394f0` and the keyboard
dispatch `@ 0x63ad10` — see "Widget render dispatch"). Witness comments at `0x65c190`,
`0x63ab00`, `0x63e691`, `0x646299`, `0x647917`, `0x647932`, `0x63b6b0`, `0x6480e0`;
`idb_save` done.

### Element struct fields (witnessed offsets)

| Offset | Field |
|---|---|
| `+0xD0..+0xDC` | POSITION rect (`left/top/right/bottom`) |
| `+0xF8` | `GLOBAL_VAR` flag `[orig: @ 0x648323]` |
| `+0x124` | `FORM` index (int) `[orig: @ 0x6482a6]` |
| `+0x134` | `DRAW_FRAME` flag (gates the frame draw) `[orig: CStaticWnd_Render @ 0x657b10]` |
| `+0x284` | stencil `SIZE` |
| `+0x288` | stencil `INSETX` (float) `[orig: @ 0x648717]` |
| `+0x28C` | stencil `INSETY` (float) `[orig: @ 0x648756]` |
| `+0x30C` (edit, this+780) | `PASSWORD` flag `[orig: @ 0x661d3b]` |

### Inner divergence (closed 2026-06-23b)

- Table HEADER `type="id"` `[orig: branch @ 0x64344a -> CUIStringTable_LookupString @ 0x6434df]`
  now resolves through the RTXT table in `build_table` (via `resolve_text`); the reimpl previously
  round-tripped the `type` attribute but drew the raw id. See "Controls / key-binding table" above.

Closed since the notes were taken (do not resurrect from `notes/`): the hardcoded
16/24 stencil insets `[orig: @ 0x648717 / 0x648756]` and the texture/rect inversion
`[orig: @ 0x647d40]` were fixed by the 2026-06-09 grill (see Layout and Frame above);
`FORM`, `GLOBAL_VAR`, `PASSWORD`, and scroll `HEIGHT/WIDTH` are now parsed and
round-tripped by `engine/formats/mnu`.

## In-game armory — the WEAPON screen (engine-research 2026-07-09; re-grilled 2026-07-11)

The in-match loadout UI is **weapon.mnu's WEAPON screen** (a boot resource of the
game.mnu family) — NOT the loadout.mnu/LOADOUT screen found in some extracts, which
retail `Jointops.exe` never references (no `loadout` string exists in the image).
Reimpl: `ArmoryMenuCompanion` (companion) + the shell armory key +
`Simulation.apply_local_player_loadout`; GUT `armory_menu_seam_test.gd`.

**Open paths (three, all -> `UI_OpenMenuScreen("weapon.mnu", "WEAPON", 0)
@ 0x54e520` + latch `g_WeaponScreenOpen @ 0x24C1884`; none stops the world —
the screen is a live overlay, and in an MP session the team scoreboard draws
over it `[orig: Render_ProcessMainSceneFrame @ 0x5cae1c]`):**

1. **The USE-ITEM key** (action 177, binding row 44 `useitem`; shipped default =
   **SHIFT**, per the retail KeyChart's "USE ITEM/ATTACH/ARMORY")
   `[orig: Input_HandleActionBinding_0 case 0xB1 @ 0x4e0b3f]`: gated on a local
   player, NOT seated (parentSlot == 0), entity Flags 0x400000 (inside a type-6
   armory collision volume — world-wac-ai-re.md §15.4), the repeat debounce
   `g_weaponScreenOpenDebounce @ 0x24C18E8`, and the host weapons rule
   `dword_A85B6C` (BSS ⇒ 0 at boot: SP allows; the WPN_ARMORY / WPN_NEVER /
   WPN_MISSION radios on MULTI_PLAYER_HOST are its setter `[orig: @ 0x5580f0]`).
   Flags 0x800 (type-11 vehicle-loadout volume) -> `vehicle.mnu` VEHICLE
   (occupancy check via groundEntity Team); out of both zones the key falls
   through to its normal use-item leg.
2. **Action 218** `[orig: Input_HandleActionBinding @ 0x49b83d]` — the same
   zone-gated open, but **no binding row ships for 218** (no config name, no
   default key: NOT user-remappable; programmatic/legacy only). Case 221 ->
   `cmap.mnu` CMAP (the deploy map, after `Game_InitRespawnState @ 0x499360`).
3. **Action 40** (`!ToSpecial`, row 117, not remappable) ->
   `UI_OpenWeaponScreenSinglePlayer @ 0x424390/0x4ddce0`: SP-only
   (`!is_in_session`), **no zone gate**.

**Screen lifecycle.** INIT (once) `[orig: UI_InitWeaponClassSelection
@ 0x567250]`: runs the control registrar, then fills PLAYER_CLASS with the
"Menu"-section `CHARCLASS_MEDIC/SNIPER/GUNNER/RIFLEMAN/ENGINEER` rows, values
5..9. ON SHOW `[orig: @ 0x567370]`: the selected class =
`Armory_ResolveSelectedClass @ 0x5642f0` — the player's current `playerClass`
when `g_hostClassAllowMask @ 0x24D59FC` allows it, else the next allowed class
scanning up through 9, else **7 (gunner)**; a class outside 5..9 (an unclassed
SP spawn) filters NOTHING (switch default mask = −1). Per-item enable rides the
mask bits 5..9; the spin selects **by value** (`SpinList_SelectItemByValue
@ 0x64ba50`, row 0 on no match); the spin + its label are enabled **only
in-session** (SP: disabled). ACCEPT gains hotkeys from binding row 177's
runtime keys (`g_useItemBindingKey0/1 @ 0x81A468/6A`, writer still unwalked):
the on-show clears then re-adds them on the ACCEPT control
(`CUIWidget_ResetScreenHotkeys @ 0x649ce0`, `CUIWidget_AddScreenHotkey
@ 0x649e20 -> the screen's key->widget table @ 0x63a8e0`), so the armory-opener
key doubles as ACCEPT while the screen is up. The opener press must RELEASE
once first: the open stamps `g_weaponScreenOpenDebounce` `[orig: @ 0x4e0b21]`
and only the row's KEYUP clears it (`Input_HandleMenuKeyRelease @ 0x4de2d0`).
PORTED 2026-07-11 (the weapon round): `ArmoryMenuCompanion.accept_hotkey_edge`
(armed-on-release debounce; `on_menu_built` = the on-show stamp) routed by
`ArmoryPresenter._unhandled_key_input` while the overlay is open.

**Control registration** `[orig: WeaponDef_RegisterUICallbacks @ 0x567020 —
(screen "WEAPON", control, kind, handler, arg) via the shared registrar
@ 0x63c060]`:

| Control | Handler | Notes |
|---|---|---|
| PRIMARY | `UI_OnPrimaryWeaponTypeChanged @ 0x5662d0` | slot combo (kind 0x220) |
| PRIMARY_AMMO1 / _AMMO2 | `sub_566620` (arg 0/1) | clip-count combos |
| PRIMARY_AMMO1_TYPE | `sub_566650` | round-type combo |
| SECONDARY (+ ammo/type) | `UI_OnSecondaryWeaponChanged @ 0x566670`, `sub_5669C0/…F0` | |
| ACCESSORY (+ ammo) | `ui_on_weapon_ammo_slot_changed @ 0x566a10`, `sub_566D40` | |
| GRENADE_AMMO1..3 | `WeaponDef_UISlotSelectCallback` (arg 0/1/2) | |
| PLAYER_CLASS | `handle_team_class_selection @ 0x566f60` (kind 0x40 spinlist) | MP-only flip; host fills the authored-empty items |
| ACCEPT / CANCEL | `WeaponLoadout_ApplyFromBuffer @ 0x565cd0` (arg 0/1) | kind 8 buttons |

**Population** `[orig: populate_three_category_lists @ 0x566db0]` — the WEAPON
screen's own populate (the similar `populate_weapon_slot_lists @ 0x560430`
serves player.mnu's PLAYER_INFO): filter = def valid && class mask && team mask
&& `g_armoryWeaponAvailability @ 0x24D5600` (per-adm-index byte table; writers:
`Mission_LoadBMSFile @ 0x40f834` — SP missions author the armory list —
`NapiNPClientMsg_HandleWeaponRestrictions @ 0x42d4cc`,
`apply_session_settings_to_globals`, `Game_StartMission`,
`CAdminServer_HandleWeaponCommand`); category dword +17 routes 1->PRIMARY,
2->SECONDARY, 0->ACCESSORY; rows **sorted case-insensitively ascending**
(`ListWidget_SortRows @ 0x644990` -> `cmp @ 0x6448a0`, params (string, asc));
`Menu/NONE` prepended at row 0. The tail parses the canonical, unexpanded
`{name, ammoPri, ammoSec, flags}` tuples from the **per-class** loadout buffer
(`populate_ammo_type_combo_boxes @ 0x564930`), resolves each parent name to its
catalog index `@ 0x564A00..0x564A06`, routes it by the parent's category
(`ACCESSORY` case 0 `@ 0x564B47`), selects that visible row by adm index via
`UIList_SelectByValue @ 0x645240 (the calls @ 0x564B26..0x564B33)`, and fills the ammo/type combos from it. It
does not reconstruct the selection from the expanded runtime weapon-slot table,
then `update_weapon_weight_display @ 0x565640` renders STATIC_TOTAL_WEIGHT as
`sprintf "%s %.1f %s (%s)"` = TOTAL_WEIGHT / `calculate_equipped_weapons_weight
@ 0x565490` / LBS / encumbrance (`< 33.3 LIGHT_ENCUMBRANCE`, `< 66.6 NORMAL_`,
else `HEAVY_`), and swaps PRIMARY/SECONDARY/ACCESSORY_ICON from the 192-byte
icon table `@ 0x2540D70`. The weight sum: selected weapon `adm[85]/65536` per
slot + `(ammoRow+1) × selectedAmmoDef[84]/65536` per ammo combo — the ammo
TYPE's own def carries the clip weight. A class flip
`[orig: handle_team_class_selection @ 0x566f60]` (MP-only) first serializes the
outgoing class's selections into its buffer, then swaps + repopulates.

**ACCEPT** `[orig: WeaponLoadout_ApplyFromBuffer @ 0x565cd0]` (CANCEL = arg 1 =
the `skip_apply` param; both legs clear the latch + `Server_ResetBalanceCounters
@ 0x54b940`; guard = `!g_weaponScreenOpenDebounce && g_WeaponScreenOpen`):
serialize the UI into the **per-class** (5..9) 2048-byte loadout string buffer
`{name\0 ammoPri\0 ammoSec\0 flags\0}*`
`[orig: WeaponLoadout_SerializeSelectionsToBuffer @ 0x5658b0 ->
g_armoryLoadoutBufferByClass @ 0x25DD740 + 2048*g_armorySelectedClass
@ 0x25DCF34]`; in an MP session the CLIENT resets its slots and sends the buffer
to the host (the C2S 0x2F seam `[orig: @ 0x42cdc0]`, already byte-golden in
npruntime — net-re §5.56/5.57); offline/SP it parses the tuples back
(`AvatarDef_FindByName`), expands each def's **sub-weapons** (`def[235]` count),
and applies through the SAME chain as the S2C 0x5A client apply (clips clamped
to adm[83]; −1 -> adm[23]. The main fallback is raw; a sub-weapon total is
multiplied by adm[22] only when nonnegative, while a negative no-clip fallback
stays raw `[orig: @ 0x566166; @ 0x5661E8..0x566215]`;
`WeaponSlot_SetAmmoCount @ 0x540b50`, `WeaponSlotPool_ResetAllEntries
@ 0x53f240` -> `WeaponSlotTable_LoadAllFromDefs @ 0x5414e0` ->
`WeaponSlots_RecalculateAmmoFromCapacity @ 0x542280`; carry-flag bits
`entity+44 |= 8/0x10` from adm[2]&0x1000 / adm[3]&2), then re-selects the
equipped slot `[orig: Player_SelectWeaponSlot @ 0x4dd680 /
Player_MountWeaponSlot @ 0x4dfa40 — camera/scope/switch-queue state only: the
mount never consults the FP render model]`. Our SP apply (2026-07-18, the
loadout grill) parses the full multi-slot kit into the sim's slot pool
(`engine/runtime/world/weapon_inventory` — sub-weapon expansion, requested-ammo pool
fills, the witnessed re-select), stamps `player_class`, and the commit event
re-mounts the FP viewmodel/action FSM; the accepted kit becomes the respawn
spawn kit (net-re §5.63).

**Reimpl status (2026-07-11 re-grill; refreshed 2026-07-21).**
Ported and matching: the zone-gated open on the use-item key (SHIFT), including
its not-seated gate, the live-overlay (no world-stop) state, the CHARCLASS_*
class rows + resolve rule in offline play, sorted rows under NONE, the canonical
parent-tuple reselect on ALL THREE slot combos (backed by the authoritative
unexpanded spawn kit rather than the expanded runtime slot pool), retail
one-through-max parent-ammo rows with quantity/round labels and canonical count
preselect, the `GRENADE_AMMO1..3` count selectors backed by class/team/selectable
grenade defs in table order (an unavailable definition keeps its control as
zero-only; canonical count preselect + ACCEPT serialization are live), the
`g_armoryWeaponAvailability` filter term (mission-authored via the .bms
item_availability promote; values in net-re §5.63), the witnessed weight
format, and the offline ACCEPT collect/apply seam — now the full multi-slot
kit into the sim's slot pool with requested-ammo pool fills (net-re §5.63).
The standalone game's `GameWorld` exposes its lazily loaded weapon catalog to
the armory companion, so this canonical reselect also runs on the first production
visit (fixed 2026-07-21 after the unit proxy had masked the missing shell seam).
Kept divergence: **D-MNU-9**. Deferred (unported sub-elements, tracked here +
in `ArmoryMenuCompanion`'s header): the per-class loadout buffer MEMORY
(save-on-flip + remembered ammo counts), live MP C2S 0x2F / S2C 0x5A
submission (the UI stays gated in a network session until that authoritative
path is exposed; the S2C 0x66/admin availability writers ride it), MP class
selection, the `*_AMMO1_TYPE` round-type cascade + per-ammo-def weight,
`*_AMMO2`, the icon swaps, the MP scoreboard overlay, and
the use-item key's non-armory leg (the ACCEPT hotkeys landed with the weapon
round — see the on-show section above). Open questions: the runtime site that stamps the shipped default keys
into the binding rows (default.key ships in no JO PFF; the KeyChart is the
defaults witness); the entity Team {1,3}->mask 2 else 1 convention vs our
avatar team ids.

**IDB changes (2026-07-11):** renamed `sub_5642F0 ->
Armory_ResolveSelectedClass`, `sub_424390 -> UI_OpenWeaponScreenSinglePlayer`,
`sub_64BA50 -> SpinList_SelectItemByValue`, `sub_644990 -> ListWidget_SortRows`,
`WeaponLoadout_SerializeToBufferTeamBased ->
WeaponLoadout_SerializeSelectionsToBuffer` (the buffer is per-CLASS, not
per-team), `unused6 -> g_armoryWeaponAvailability`, `team2 ->
g_armorySelectedClass`, `unk_25DD740 -> g_armoryLoadoutBufferByClass`,
`dword_24C18E8 -> g_weaponScreenOpenDebounce`, `dword_24D59FC ->
g_hostClassAllowMask`; `WeaponLoadout_ApplyFromBuffer` param 3 ->
`skip_apply`. (2026-07-09: `UI_OpenMenuScreen @ 0x54e520` renamed, ex
"renderer init" misnomer.)

- **D-MNU-9 (armory per-class loadout memory):** the original's clip-count
  combos are driven by the per-class loadout buffer, including save-on-class-flip
  remembered counts. The reimpl now matches the visible row model and weight
  path: parent row zero means one clip, rows show
  `clips * clipsize - round_type`, and ACCEPT serializes `row+1`
  `[orig: @0x564c7d..0x564ce4; @0x565490]`; grenade rows retain zero and
  serialize the selected row. First open restores counts from the authoritative
  canonical kit, but separate remembered buffers per class remain deferred.

- **D-MNU-10 (offline class selection, deliberate — user decision 2026-07-11):**
  the retail WEAPON screen enables the PLAYER_CLASS spin only **in a network
  session** `[orig: UI_InitTeamClassSelection @ 0x567370 is_in_session branch;
  the SP-only open UI_OpenWeaponScreenSinglePlayer @ 0x424390 exists because SP
  runs sessionless, and retail SP pins the class]`. The reimpl enables it in
  offline play too: our runtime hosts a listen session even for SP (ADR 0009),
  and the offline loadout flow wants the choice. The in-session filter masks,
  sorted rows, and ACCEPT class apply are unchanged.
