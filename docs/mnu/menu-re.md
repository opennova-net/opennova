# MNU/MNS menu UI: engine correspondence and equivalence

How Joint Operations parses, lays out, sounds, and draws its `.mnu` menus, as
witnessed in the original engine, and how `libs/mnu`, `libs/mnu_xml`, `libs/mns`,
and `godot/engine/mnu` correspond to it.

Reverse-engineered from `Jointops.exe` (Joint Operations: Combined Arms, imagebase
`0x400000`, IDB `Jointops.exe.kong.i64`). All addresses are absolute in that image.
This is the menu-slice grill (2026-06-09), closing the render/sound divergences the
2026-06-01 format pass deferred. The 2026-06-23 render grill added the coordinate system,
per-item image/color rendering, spinlist arrows, the combo dropdown, the marquee CBIN
credits, and the monogram (parsed-but-not-drawn) — see the sections below.

---

## Parse pipeline

`UIScene_LoadAndParseContent @ 0x63c830` loads the `.mnu` bytes, strips a UTF-8 BOM,
expands `%VAR%` over the WHOLE buffer (`NapiXML_ExpandVariablesInText @ 0x63a000`),
then SAX-parses the UTF-16 tag stream (`XML_ParseWithBOMDetection @ 0x76a690`). Tag
literals are wide; the element handler is `CUIElement_ParseXMLDefinition @ 0x648120`,
and the type factory is `CUIScene_CreateWidgetByType @ 0x64f630`.

| Concern | Original | OpenNova |
|---|---|---|
| Element parse | `CUIElement_ParseXMLDefinition @ 0x648120` | `libs/mnu` parse + `nova_mnu_builder.cpp` |
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

The element texture is stretched INTO the resulting rect, UV 0..1
(`CUIElement_DrawStretchedTexture @ 0x647d40`). Reimpl: `apply_position` in
`nova_mnu_builder.cpp`; checkboxes stretch like every other element (the old
keep-aspect was a reference-repo choice). With no resolvable font the builder
measures a nominal 8x16 glyph box so headless layout stays deterministic.

## Frame `[orig: CUIElement_DrawFrame @ 0x64a210; init_border_materials @ 0x646f70]`

The stencil is a grid of `SIZE x SIZE` tiles (canonically 4*SIZE square):
row 0 = top corners + top edge (+ fill tile at col 3), row 1 = left/right edges,
row 2 = bottom row. The drawer paints a center fill quad (the BRUSH, tiled) plus 8
INDEPENDENT border pieces: corners at `SIZE`, edges stretched between them, the whole
border hanging OUTSIDE the window rect by `SIZE` and pulled back by the authored
`STENCIL INSETX/INSETY` (floats at elem+0x288/+0x28C, default 0). Every quad is
modulated by `0x7F7F7F` (neutral in the modulate-2x fixed-function path -> no tint).
Reimpl: `add_frame` in `nova_mnu_builder.cpp` (8 `TextureRect` pieces + a tiled fill).
The old 4x4 mirrored-corner NinePatch bake with hardcoded 16/24 insets is retired.

When NEITHER the stencil nor the brush texture resolves, the original draws nothing:
every draw in `CUIElement_DrawFrame` is guarded by a successful texture load
(`sub_654370 >= 0`). The reimpl matches this at runtime (no panel); the editor keeps a
faint placeholder so an author can still see the framed region. The old opaque dark
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
space (`menu_shell.gd::_recompute_fit`, `mnu_canvas.gd::_recompute_fit`). The old reimpl
used a hardcoded 640x480 board with uniform letterbox + centering, which overhung and
mis-centered the 800x600 `jo_game.mnu` (the badly-placed ESC menu) and letterboxed every
menu. `D-MNU-4`: the original truncates each scaled quad to int per element; the reimpl
applies one float CanvasItem scale, a sub-pixel divergence (accepted).

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
combo/list items (a host can populate text-only at runtime). `D-MNU-5`: shipped menus use
image/color items only in spinlists; the combo closed cell still renders text-only (its
LIST_BOX items are all `type="id"`).

### `%VAR%` colors in APPEARANCE

A `<APPEARANCE type="color">` value is frequently a stylesheet variable (e.g.
`%COLOR_BLACK%` on a LIST_BOX background, `%TRIM_COLOR%` on an outline). The reimpl now
resolves the var through the stylesheet before parsing the hex
(`get_appearance_color` -> `resolve_color`, `[orig: NapiXML_ExpandVariablesInText
@ 0x63a000]`). Previously every `%VAR%` color appearance silently failed, leaving combo
dropdown popups, container backgrounds, and outlines transparent (the "stacked-inline /
overlapping" video-options dropdowns).

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

## Combo dropdown `[orig: CComboWnd @ 0x65be40; CComboWnd_Render @ 0x65bfd0]`

A combobox is a closed `CButtonWnd` (showing the selected item) plus an embedded
`CListWnd` popup (`this+1536`), shown/hidden with its own `<LIST_BOX>` POSITION + clip and
appearance background. Reimpl `NovaMnuCombo`: a TextureButton + a clamped, scrollable
in-tree popup styled from the LIST_BOX. The visible bug was the popup background (a
`%COLOR_BLACK%` color appearance) not resolving — fixed by the `%VAR%` color change above.

## Marquee / credits `[orig: CMarqueeWnd @ 0x65c430; marquee_load_credits_from_ini @ 0x65c5a0]`

A `marquee_wnd`'s `<DATASOURCE>` (e.g. `nlist.kda`) is a CBIN-encrypted credits config,
NOT plain text: `ConfigFile_LoadGlobal` reads an `[ENV]` section (`SCROLL_RATE`,
`CENTER_X`, `VERTICAL_SPACE`) and a `[TEXT]` section whose lines are AES-decrypted and
carry formatting codes (`~C` colour, `~F` font, `~I`/`~F` image, `~J` justify, `<CR>`
newline) `[orig: marquee_load_credits_from_ini @ 0x65c5a0]`. Reimpl: a CBIN datasource is
routed to the existing `NovaCreditsPlayer` (fed by a `CbinCreditsResource` decoded from
the file's bytes via `CbinCreditsResource::from_cbin_bytes`, so it works from a PFF); a
plain-text datasource keeps the simple `NovaMnuMarquee`. The old code read the binary
`.kda` as a string, so the credits showed the literal `CBIN` magic and did not scroll.
`D-MNU-6`: the CBIN credits' custom fonts/textures are not yet resolved from the resource
root (text scrolls with the default font); a follow-up.

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
`NovaMnuMenu::resolve_sound_bank` (per-file `.lwf` cache, the collection analogue) ->
`play_lwf_set` (reusing `opennova::audio::SoundSelector` from the sound slice);
`master_volume` property. The old model keyed sounds on trigger SUBSTRINGS
(`MOUSE`/`OVER` -> hover, `CLICK`/`SELECT` -> click), had no MOUSEOUT, and routed every
widget through a single host `.lwf` profile - all corrected.

`g_MenuSoundBank @ 0x25DC3E0` (the hardcoded `menu.lwf` load at profile-selector init
`@ 0x5613bf`) is a RED HERRING: it is the player-info VOICE preview bank
(`VOICE_%d`, `sub_55FF70`), not the widget sound mechanism.

## XML entities `[orig: XML_ParseCharEntity @ 0x769cc0; table @ 0x85a628]`

Numeric entities are DECIMAL-only (`_wtol` base 10; `&#xNN;` -> 0) and truncate to the
low byte. The named table is Latin-1: 7 case-insensitive core entries
(`quot/amp/lt/gt/copy/reg/nbsp`, with `nbsp -> 0x20`) and the case-sensitive accented
set `0xC0-0xFF` + `laquo/raquo`. There is NO `&apos;`. An unknown entity (no match / no
`;`) decodes to a bare `&` with the name text left to re-parse verbatim. Reimpl:
`decode_entity` matches this exactly; `escape_xml` emits only the 4 decodable entities
(never `&apos;`).

## `%VAR%` expansion `[orig: NapiXML_ExpandVariablesInText @ 0x63a000]`

The engine expands `%name%` from a host-supplied list at `ctx+84` over the whole
buffer before parse (`<RAW_TEXT>` is copied verbatim; an unresolved `%name%` is kept
literal). OpenNova keeps raw tokens in the document and expands per consumed field at
build time (colors/fonts/textures/text) - see ADR 0005. Documented gap: host variables
in non-themed fields need a host var map plumbed into the builder.

## MNS stylesheet format `[orig: sub_552500, ref'd from UIScene_LoadAndParseContent @ 0x63c830]`

The substitution table lives in `menu_style.mns` ("named menu_style.mns for the game
to find it"), loaded by canonical name when menus initialize. The format's
authoritative specification is NovaLogic's own 38-line comment header in the shipped
file (vendored byte-exact at `fixtures/mns/menu_style.mns`); the loader itself is
unwitnessed in IDA so far - the reimplementation (`mns::Document::parse`,
`libs/mns/src/mns_document.cpp`) is built from that in-file spec plus
current-behavior compatibility, with a grill session as the open follow-up.

Spec rules implemented (2026-06-12 pass; the old parser violated the first two):
`NAME value` pairs, value from the first non-whitespace after the name to the last
non-whitespace on the line; `//` comments anywhere, including after a value;
`\` continuations (whitespace before the backslash is part of the value; a comment
may follow the backslash; a name alone followed by `\` starts its value on the next
line); `\\` escapes a literal backslash in values; names exclude whitespace and the
six `% < > # \ /`; nestable `#if 0|1` / `#else` / `#endif`.

The model is lossless (ADR 0009): typed node fields exactly partition the file's
bytes, so an untouched parse -> serialize is byte-identical and an edited value
changes only its own line. `Document::flatten()` is the runtime view the existing
`mns::StyleSheet` API serves (last duplicate wins, evaluated conditionals).

Divergences (each lenient-with-diagnostic where the spec says "error"; the parse
never fails on shipped data):

- **D-MNS-1 (duplicate names):** the spec calls duplicates an error (debug-build
  reporting only); the reimpl keeps last-wins flatten behavior and emits a
  `duplicate-name` error diagnostic the editor surfaces.
- **D-MNS-2 (unknown `%VAR%`):** the spec calls an unmatched tagged macro a failure;
  the substitution layer keeps it literal (cross-ref D-MNU-1 / ADR 0005 - the host
  var list means stylesheet-side strictness would misfire), and the Menus workspace
  counts unresolved tokens in its status bar instead.
- **D-MNS-3 (`#if` argument):** only `0`/`1` are valid per spec; any other token is
  truthy in the reimpl (legacy-parser behavior, pinned) plus a `bad-if-arg`
  diagnostic.
- **D-MNS-4 (inactive-region scanning):** inside an evaluated-false region the
  scanner is line-based and continuations are not honored, while an ACTIVE define's
  continuation consumes even a `#endif`-looking next line (legacy `read_value`
  precedence, pinned by `tests/mns/mns_document_test.cpp`). Unwitnessed in the
  binary; flagged for the grill.

## Verdict

**matching** (after this grill) on: type factory + unknown-token preservation, the
three-stage POSITION layout + texture-into-rect, the 8-piece frame + data-driven
stencil insets, per-state sound slots + per-element bank resolution + the set/layer
play and master volume, the XML entity policy, and the format round-trip (ADR 0002).

**matching** (2026-06-23 render grill): the 800x600 anamorphic coordinate system, the
draw-nothing frame fallback, MONOGRAM-parsed-but-not-drawn, spinlist/list/combo item
rendering (text / native image / full-rect color swatch), `%VAR%` color appearances,
spinlist SPINUP/SPINDOWN parent-relative geometry, the combo closed-cell + LIST_BOX
popup, and the marquee_wnd CBIN-credits datasource.

Accepted/divergent (each a documented decision, not a defect):

- **D-MNU-1 (`%VAR%` mechanism):** per-field build-time expansion vs whole-buffer
  pre-parse - the runtime result matches for stylesheet vars; host-var-in-text is a
  plumbing follow-up (ADR 0005).
- **D-MNU-2 (sound jitter):** the shared `SoundSelector` reproduces member selection
  but not the interleaved per-play volume/pitch jitter draws (same accepted divergence
  as the mission sound host).
- **D-MNU-3 (strictness / authoring superset):** the format layer preserves attributes
  the runtime ignores (ADR 0002) and a host `sound_profile` fallback services file-less
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

Deferred (unwitnessed or out of bar; backlog, not blocking):

- The hotkey consume-on-effect vtable path (`CUIWidget_HandleScriptedAction @ 0x649790`,
  no direct xrefs) and MUSICVAR host dedup policy.
- `GLB_TABLE/RADIOEDIT/LAN_LIST/GOPHER` runtime behavior (multiplayer-browser widgets;
  build as containers, behavior rides with the net workspace).
- Real 3D globe; the table SCROLLBAR delegate `(*(tableWnd[244]+60)) @ 0x643b22`.

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
| `UIScene_LoadAndParseContent @ 0x63c830` | menu load path: `NovaMnuDocument` + `godot/game/menu_shell.gd` |
| `sub_552500` (.mns stylesheet load, ref'd from `0x63c830`) | `mns::Document::parse` — `libs/mns/src/mns_document.cpp` (lossless model, ADR 0009; loader unwitnessed, built from the in-file spec — D-MNS-1..4) |
| `XML_ParseWithBOMDetection @ 0x76a690` | `mnu_xml::parse` + `skip_bom` — `libs/mnu_xml/src/mnu_xml.cpp` |
| `XML_ParseCharEntity @ 0x769cc0` | `mnu_xml::decode_entity` — `libs/mnu_xml/src/mnu_xml.cpp` (faithful to the engine's non-standard policy: no `&apos;`, decimal-only `&#`, Latin-1 named set) |
| `NapiXML_ExpandVariablesInText @ 0x63a000` | `MnsStyleSheet::substitute` — `godot/engine/mnu/mns_stylesheet.cpp` (per-field post-parse, not whole-buffer; D-MNU-1 / ADR 0005) |
| `parse_scene_node_attributes @ 0x639630` | `mnu::parse_screen` — `libs/mnu/src/mnu.cpp` |
| `CUIElement_ParseXMLDefinition @ 0x648120` | `mnu::parse_window` — `libs/mnu/src/mnu.cpp`; layout in `apply_position` — `godot/engine/mnu/nova_mnu_builder.cpp` |
| `parse_edit_widget_xml_properties @ 0x661d10` | EDIT attrs (`NUMBER/MINVAL/MAXVAL/MAXCHAR/READONLY/PASSWORD`) in `mnu::parse_window` |
| `sub_64AD90 @ 0x64ad90` (CHECKBOX attr parse) | CHECKBOX attrs (`AS_BUTTON/CHECKED`) in `mnu::parse_window` |
| `CUIScrollWidget_ParseExtendedXMLDef @ 0x64c6d0` | SCROLL `ORIENTATION` + `HEIGHT/WIDTH` thickness in `mnu::parse_window` |
| `CUIScene_CreateWidgetByType @ 0x64f630` | `mnu::parse_type_string` / `window_type_name` — `libs/mnu/src/mnu.cpp` |
| `CTableWnd_ParseXMLContentDefinition @ 0x6427d0` | `mnu::parse_table_*` — `libs/mnu/src/mnu.cpp` |
| `CListWnd_ParseXMLDefinition @ 0x645770` | `mnu::parse_listbox` — `libs/mnu/src/mnu.cpp` |
| `CUIElement_DrawFrame @ 0x64a210` | `add_frame` — `godot/engine/mnu/nova_mnu_builder.cpp` (8 border pieces + tiled fill; draws nothing when textures absent; no monogram) |
| `CStaticWnd_Render @ 0x657b10` | the base window render order (frame -> appearance -> text -> children); confirms the menu monogram is never drawn |
| `CUIScene_SetScreenScale @ 0x639480` (was `sub_639480`) | 800x600 anamorphic scale -> `_recompute_fit` in `menu_shell.gd` / `mnu_canvas.gd` |
| `CWnd_SetScaleRecursive @ 0x646c60` | scale propagation (root CanvasItem `set_scale`) |
| `CUIElement_DrawStretchedTexture @ 0x647d40` | `apply_position` texture-into-rect; the scaled-rect int truncation is D-MNU-4 |
| `CWnd_AccumulateAncestorOffset @ 0x6465e0` (was `sub_6465E0`) | Godot parent-child nesting (positions are parent-relative) |
| `CSpinListWnd_Render @ 0x64b220` + `CUISpinList_ParseXMLDefinition @ 0x64bd10` | `resolve_item` + `mnu_render_item_cell` (`mnu_item_cell.{h,cpp}`) + `build_spinlist` |
| `CSpinListWnd_CreateUpDownChildren @ 0x64b8b0` | `add_spin_button` (parent-relative SPINUP/SPINDOWN) — `nova_mnu_builder.cpp` |
| `CComboWnd_Construct @ 0x65be40` + `CComboWnd_Render @ 0x65bfd0` | `NovaMnuCombo` — `godot/engine/mnu/nova_mnu_combo.cpp` |
| `CMarqueeWnd_Construct @ 0x65c430` + `CMarqueeWnd_ParseXMLDefinition @ 0x65ceb0` + `marquee_load_credits_from_ini @ 0x65c5a0` | `build_marquee` -> `NovaCreditsPlayer` + `CbinCreditsResource::from_cbin_bytes` (CBIN datasource); `NovaMnuMarquee` (plain text) |
| `CUIWidget_HandleScriptedAction @ 0x649790` | `NovaMnuMenu::dispatch_action` — `godot/engine/mnu/nova_mnu_menu.cpp` |

IDB state note (2026-06-23): the 2026-06-23 render grill renamed `sub_639480 ->
CUIScene_SetScreenScale`, `sub_6465E0 -> CWnd_AccumulateAncestorOffset`, `sub_6394E0 ->
CUIScene_GetActiveScreenName`, `sub_647E40 -> CUIElement_DrawTextureNative`, `sub_654E60
-> CTextureManager_DrawScaledRect`, `sub_65BE40 -> CComboWnd_Construct`, and `sub_65CEB0
-> CMarqueeWnd_ParseXMLDefinition` (all anchored). `0x64ad90` is still unnamed
(`sub_64AD90`) and `0x649790` folds into the `0x648120` body (vtable-reached, no direct
xrefs); naming/splitting them remains a proposed edit.

### Element struct fields (witnessed offsets)

| Offset | Field |
|---|---|
| `+0xD0..+0xDC` | POSITION rect (`left/top/right/bottom`) |
| `+0xF8` | `GLOBAL_VAR` flag `[orig: @ 0x648323]` |
| `+0x124` | `FORM` index (int) `[orig: @ 0x6482a6]` |
| `+0x284` | stencil `SIZE` |
| `+0x288` | stencil `INSETX` (float) `[orig: @ 0x648717]` |
| `+0x28C` | stencil `INSETY` (float) `[orig: @ 0x648756]` |
| `+0x30C` (edit, this+780) | `PASSWORD` flag `[orig: @ 0x661d3b]` |

### Inner divergence still open

- Table HEADER `type="id"` `[orig: branch @ 0x64344a]` resolves the header text
  through `CUIStringTable_LookupString @ 0x6434df`; the reimpl round-trips the `type`
  attribute but performs no string-table lookup.

Closed since the notes were taken (do not resurrect from `notes/`): the hardcoded
16/24 stencil insets `[orig: @ 0x648717 / 0x648756]` and the texture/rect inversion
`[orig: @ 0x647d40]` were fixed by the 2026-06-09 grill (see Layout and Frame above);
`FORM`, `GLOBAL_VAR`, `PASSWORD`, and scroll `HEIGHT/WIDTH` are now parsed and
round-tripped by `libs/mnu`.
