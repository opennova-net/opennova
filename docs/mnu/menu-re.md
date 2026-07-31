# MNU/MNS menu UI: engine correspondence and equivalence

How Joint Operations parses, lays out, sounds, and draws its `.mnu` menus, as
witnessed in the original engine, and how `libs/mnu` (incl. the mnu_xml reader), `libs/mns`,
and `godot/engine/mnu` correspond to it.

Reverse-engineered from `Jointops.exe` (Joint Operations: Combined Arms, imagebase
`0x400000`, IDB `Jointops.exe.kong.i64`). All addresses are absolute in that image.
This is the menu-slice grill (2026-06-09), closing the render/sound divergences the
2026-06-01 format pass deferred. The 2026-06-23 render grill added the coordinate system,
per-item image/color rendering, spinlist arrows, the combo dropdown, the marquee CBIN
credits, the monogram (parsed-but-not-drawn), and the `DRAW_FRAME` frame-draw gate — see
the sections below.

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

A frame draws ONLY when the window's `DRAW_FRAME` flag is set: the render gate is
`if (elem+0x134) DrawFrame(...)` in `CStaticWnd_Render @ 0x657b10`, evaluated BEFORE the
ungated appearance/texture passes — so a window's own `<APPEARANCE>` image still draws when
DRAW_FRAME is clear. A window may carry a `<FRAME>` purely to hand its stencil/brush down to
framed descendants without drawing one itself: the shipped `jo_game.mnu` / `jo_options.mnu`
root `MAIN` defines the camo `BOXTILE` brush + `BORDER2` stencil but has NO DRAW_FRAME, so it
draws no frame, while its `MAIN_WRAPPER` / `OPTIONS_WRAPPER` children carry DRAW_FRAME and
draw the inherited frame. Reimpl: `build_container` gates ALL frame drawing (own and
inherited) on `w.draw_frame`. The old reimpl drew a window's own `<FRAME>` unconditionally,
which tiled the camo brush across the whole 800x600 root — the full-window camo behind the
in-game ESC menu (and under `letterbox.tga` on the options screen); fixed.

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
space (`nova_menu_shell.gd::_recompute_fit`, `mnu_canvas.gd::_recompute_fit`). The old reimpl
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
fonts copy all eight values `[orig: sub_646A70 @ 0x646a70]`.
`draw_text_with_cursor @ 0x6533b0` selects the pair for the active state, but
the common `font_cache_draw_text_scaled @ 0x653170` path consumes only the
foreground member and never reads the paired background. Shipped JO menus
normally author black BG values. The reimplementation therefore preserves and
editor-exposes every BG field (including a color picker) but intentionally does
not paint a text background.

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
embedded `CListWnd` popup (`this+1536`) `[orig: CComboWnd ctor @ 0x65be40]`. The parse
`[orig: CComboWnd_ParseXMLDefinition @ 0x65c0d0]` delegates the `<LIST_BOX>` content to
that embedded list, whose own window RECT (`this+13`) is set from the authored
`<LIST_BOX>` `<POSITION>` — combo-relative, in 800x600 design space. So the dropdown is a
**fixed authored rect**, not a runtime-computed box: it can sit below, beside, or above
the combo (`player.mnu` PLAYERVOICE authors a negative `TOP` to open upward).

The list render `[orig: CListWnd_DrawItems @ 0x643f30]` lays rows out inside `row_rect =
this+13`, advancing by `row_height` per row and truncating each row's text to the rect
width, with a `<SCROLLBAR>` child for overflow. The row height is the font "W" glyph
height `[orig: sub_653680 @ 0x653680]`, overridden by `this+201` (the `<MI>` /
`<MIN_ITEM_HEIGHT>` value; ctor default `-1` `[orig: CListWnd ctor @ 0x643bb0]`) only when
`>= 0`.

Reimpl `NovaMnuCombo`: a TextureButton + an in-tree layered popup. `build_combo` now
passes the authored `list_box.position` via `set_popup_rect`, and `open_popup` opens at
that rect (falling back to a below-combo clamped/scrollable box only for shell-built combos
with no authored LIST_BOX, e.g. server browsers); `effective_item_height` uses the
authored `MIN_ITEM_HEIGHT`, else the item font line height, else 16. Top-level `ITEMS`
and `LIST_BOX/ITEMS` remain independent: an authored nested collection wins even when
empty, otherwise the popup uses authored top-level fallback rows. Popup text honors
the active ITEMS horizontal/vertical justification and the LIST_BOX STRING edge
inset; the closed cell independently honors its outer STRING layout. Two earlier bugs are
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

Draw order has **no overlay pass**: the open list renders at its tree position
(`CComboWnd_Render @ 0x65bfd0` renders the closed cell, then children in array order —
the embedded list is attached as a child by `CWnd_SetParentAndAttach @ 0x6480a0` during
parse — and `CListWnd_DrawItems @ 0x643f30` early-outs on hidden). The shipped menus are
authored so this is unobservable: all three `player.mnu` combos (NATIONALITY / DIVISION /
COMBO_LIST) author their LIST_BOX rects to the SAME parent-space region `(0,65)-(214,306)`,
below every closed cell — dropdowns cover background art, never interactive siblings.

Reimpl: the exclusivity is hosted as a full-menu transparent catcher overlay
(`ComboPopupOverlay`) added as the owning `NovaMnuMenu`'s **last child** on open, with the
styled popup box inside it — last-in-tree wins Godot mouse picking and draw order, which
Godot's z_index does not affect (the pre-fix popup was a z-lifted child of the combo:
drawn on top but siblings stole its clicks, and nothing closed on outside press, so
`player.mnu`'s stacked-rect dropdowns could pile open on top of each other). The catcher
implements the witnessed outside-press close/consume + dead-cell rule; `NovaMnuMenu`
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
routed to the existing `NovaCreditsPlayer` (fed by a `CbinCreditsResource` decoded from
the file's bytes via `CbinCreditsResource::from_cbin_bytes`, so it works from a PFF); a
plain-text datasource keeps the simple `NovaMnuMarquee`. The old code read the binary
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
(`sub_63ae80(…, "CONTROL_MAPPING")`), clears it (`CTableWnd_RemoveRow(-1) @ 0x641a40`), and for the
active device `dword_25db7d8` (`0` keyboard / `1` mouse / `2` joystick) inserts a row per entry
(`table_insert_row @ 0x641c30`) and sets the cells (`@ 0x63edf0`). Each device has its own runtime
binding array (kb `dword_25c7724` / mouse `byte_25c784c` / joy `byte_25c7740`).
`refresh_control_mapping_list @ 0x55b320` recomputes per-row conflict state
(`check_weapon_slot_conflict @ 0x55ae60`, a kong-misnomer for *binding* conflict) and tints
conflicting rows yellow (`sub_640110(row, …, -256)`). The device radios call
`sub_55bcd0(mode) @ 0x55bcd0` (sets `dword_25db7d8`, swaps the REMAP_INSTRUCTIONS text id
`REMAP_Keyboard`/`REMAP_Mouse`/`REMAP_Joystick`, repopulates).

**Control column format** `[orig: KeyBinding_FormatBindingString @ 0x559a10]`: up to two key slots,
each prefixed `Ctrl-` / `Shift-` when its modifier word is `17` / `16`, joined by the localized
"OR" (` XXor ` fallback -> ` or `). Key names decode through `KeyBinding_GetKeyNameAndDisplayName
@ 0x494c60`, a Windows-VK switch returning a display name ("Mouse 1", "Up", "Space", "F1", "[", or
the printable char). Mouse buttons use special codes (`1` left, `2` right, `16` middle, `1024`
wheel up, `2048` wheel down); joystick uses `JOYBUTTON%d`.

Reimpl: **`libs/controls`** (Godot-agnostic) ports the catalog (`controls.cpp` `k_catalog` —
byte-exact names/tokens/Class id + the default VK binding from the catalog's binding slot,
validated Forward=W/Up, Reload=R, Jump=Space, …), the Class-name table (`action_class_name`), the
VK decoder (`key_name`), and the binding format (`format_binding`); `build_rows(device)` mirrors
`UI_PopulateControlMappingList`. The Godot wrapper **`NovaControlsModel`** hands rows to
`godot/game/nova_menu_shell.gd` (`_seed_control_mapping` / `_fill_control_mapping`), which fills the
`CONTROL_MAPPING` `NovaMnuTable` via `add_rows` and wires the Keyboard/Mouse/Joystick radios to
repopulate. The earlier reimpl left the table empty — `nova_menu_shell` had no populate path for a
`type="table"`, so the Controls tab rendered floating headers over a blank grid.

The table render itself was also corrected this pass (see Table render below). This pass is
**read-only**: it reproduces what the Controls tab DISPLAYS. Live double-click rebinding
(`update_control_mapping_display @ 0x55b700`), DEFAULTS (`sub_55bd90`) / CLEAR_KEY (`loc_55bfd0`),
and profile persistence are deferred (D-CTRL-3), gated on a real game input-action layer.

### Table render `[orig: CTableWnd_ParseXMLContentDefinition @ 0x6427d0]`

The reimplementation's table template is carried by `build_table` and
`NovaMnuTable`:

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
- **The scrollbar honors the authored `<SCROLLBAR><POSITION>`** (table-relative) and art width
  instead of a hardcoded 16px right strip; the track texture is applied like `build_scroll`. The
  ITEMS `%TRIM_COLOR%` outline now draws as a header rule + per-row grid line.

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
widget through a single shell `.lwf` profile - all corrected.

`g_MenuSoundBank @ 0x25DC3E0` (the hardcoded `menu.lwf` load at profile-selector init
`@ 0x5613bf`) is a RED HERRING: it is the player-info VOICE preview bank
(`VOICE_%d`, `PlayerInfo_PreviewVoice`), not the widget sound mechanism.

## Screen music `[orig: UI_DispatchScreenEvent @ 0x54e6a0]`

Every screen event stores the active screen's `MUSICVAR` field (screen +0x14) into
AudioVM Var2 at `0x54eff4`. The write is unconditional: an absent field contributes
its parsed default zero, and showing the same screen again repeats the store. The
runtime mirrors that rule through `NovaMnuMenu::apply_music_for_screen`.

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

The editor model remains lossless (ADR 0014): typed node fields exactly
partition the file bytes, so an untouched parse -> serialize is byte-identical
and an edited value changes only its own line. Runtime flattening is a separate
retail evaluator because the original can reject syntax that the editor must
still open for repair.

Observed runtime rules:

- names are case-insensitive and a duplicate silently replaces the earlier
  value; duplicate diagnostics are an editor enhancement;
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

Disposition: **D-MNS-1** and **D-MNS-2** match retail (the editor keeps their
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
Ported as the menu-top catcher overlay + NovaMnuMenu single-open registry (D-MNU-11; the
pre-fix reimpl let overlapped siblings steal popup clicks and stack dropdowns open). Draw
order stays tree-positional in the original with no overlay pass; the reimpl's menu-top
draw is a recorded reimpl divergence, unobservable in shipped menus (D-MNU-12). Pinned by
the five input-routing tests in `mnu_combo_test.gd` (see the section above).

**matching** (2026-06-23b controls grill): the CONTROL_MAPPING population (the action catalog +
Class-id->name table + per-device row build), the byte-exact default keyboard bindings, the Control
column format (key-name decode + `Ctrl-`/`Shift-`/`OR`), and the three table-render fixes (header
`type="id"` lookup, body justify, authored scrollbar position) — the table now renders a populated,
scrollable, correctly-aligned grid instead of floating headers over a blank body.

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
  `list_box.position` via `NovaMnuCombo::set_popup_rect`; `open_popup` uses the authored
  rect when present (which also gives PLAYERVOICE its upward open), else the below-combo
  fallback for shell-built combos.
- **D-MNU-8 (combo row-height default) — FIXED 2026-06-23c:** the list row height is the
  font "W" glyph height `[orig: CListWnd_DrawItems @ 0x643f30 -> sub_653680 @ 0x653680]`,
  overridden by `this+201` (the `<MI>`/`<MIN_ITEM_HEIGHT>` value; ctor default `-1`
  `[orig: CListWnd ctor @ 0x643bb0]`) only when `>= 0`. The reimpl defaulted to a hardcoded
  16px. Fixed: `NovaMnuCombo::effective_item_height` returns the authored MIN_ITEM_HEIGHT,
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
  outside-press close/consume + dead-cell rule on the catcher, the NovaMnuMenu single-open
  registry, and close-on-screen-change/exit/hide. `godot/engine/mnu/nova_mnu_combo.cpp`,
  `nova_mnu_menu.cpp`.
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

Deferred (unwitnessed or out of bar; backlog, not blocking):

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
| `UIScene_LoadAndParseContent @ 0x63c830` | menu load path: `NovaMnuDocument` + `godot/game/nova_menu_shell.gd` |
| `Menu_InitShellResources @ 0x552500` → `NapiConfigMap_LoadIncludeFile @ 0x63b970` → `parse_key_value_buffer @ 0x639870` | `mns::Document::evaluate` — `libs/mns/src/mns_document.cpp` (witnessed runtime evaluator) plus the separate lossless editor model (ADR 0014) |
| `XML_ParseWithBOMDetection @ 0x76a690` | `mnu_xml::parse` + `skip_bom` — `libs/mnu/src/mnu_xml.cpp` |
| `XML_ParseCharEntity @ 0x769cc0` | `mnu_xml::decode_entity` — `libs/mnu/src/mnu_xml.cpp` (faithful to the engine's non-standard policy: no `&apos;`, decimal-only `&#`, Latin-1 named set) |
| `NapiXML_ExpandVariablesInText @ 0x63a000` | `MnsStyleSheet::substitute` — `godot/engine/mnu/mns_stylesheet.cpp` (per-field post-parse, not whole-buffer; D-MNU-1 / ADR 0005) |
| `parse_scene_node_attributes @ 0x639630` | `mnu::parse_screen` — `libs/mnu/src/mnu.cpp` |
| `CUIElement_ParseXMLDefinition @ 0x648120` | `mnu::parse_window` — `libs/mnu/src/mnu.cpp`; layout in `apply_position` — `godot/engine/mnu/nova_mnu_builder.cpp` |
| `parse_edit_widget_xml_properties @ 0x661d10` | EDIT attrs (`NUMBER/MINVAL/MAXVAL/MAXCHAR/READONLY/PASSWORD`) in `mnu::parse_window` |
| `sub_64AD90 @ 0x64ad90` (CHECKBOX attr parse) | CHECKBOX attrs (`AS_BUTTON/CHECKED`) in `mnu::parse_window` |
| `CUIScrollWidget_ParseExtendedXMLDef @ 0x64c6d0` | SCROLL `ORIENTATION` + `HEIGHT/WIDTH` thickness in `mnu::parse_window` |
| `CUIScene_CreateWidgetByType @ 0x64f630` | `mnu::parse_type_string` / `window_type_name` — `libs/mnu/src/mnu.cpp` |
| `CTableWnd_ParseXMLContentDefinition @ 0x6427d0` | `mnu::parse_table_*` — `libs/mnu/src/mnu.cpp` |
| `CListWnd_ParseXMLDefinition @ 0x645770` | `mnu::parse_listbox` — `libs/mnu/src/mnu.cpp` (`<MI>`/`<MIN_ITEM_HEIGHT>` -> `this+201`; justify/vjustify/items/appearances) |
| `CListWnd_Construct @ 0x643bb0` (embedded `CScrollWnd@+976`; row-height sentinel `this+201 = -1`) | `NovaMnuCombo` popup defaults — `godot/engine/mnu/nova_mnu_combo.cpp` (D-MNU-8) |
| `CListWnd_DrawItems @ 0x643f30` (rows inside `this+13`; row height = font "W" or `this+201`; per-row text truncation) | `NovaMnuCombo::open_popup` + `effective_item_height` — `nova_mnu_combo.cpp` (D-MNU-7/8) |
| `CComboWnd_ParseXMLDefinition @ 0x65c0d0` (feeds `<LIST_BOX>` to embedded `CListWnd` `this+384`) | `build_combo` `set_popup_rect(list_box.position)` — `godot/engine/mnu/nova_mnu_builder.cpp` (D-MNU-7) |
| `CUIElement_DrawFrame @ 0x64a210` | `add_frame` — `godot/engine/mnu/nova_mnu_builder.cpp` (8 border pieces + tiled fill; draws nothing when textures absent; no monogram) |
| `CStaticWnd_Render @ 0x657b10` | the base window render order (frame -> appearance -> text -> children); the frame pass is gated on the DRAW_FRAME flag (`elem+0x134`) -> `build_container` gates `add_frame` on `w.draw_frame`; confirms the menu monogram is never drawn |
| `CUIScene_SetScreenScale @ 0x639480` (was `sub_639480`) | 800x600 anamorphic scale -> `_recompute_fit` in `nova_menu_shell.gd` / `mnu_canvas.gd` |
| `CWnd_SetScaleRecursive @ 0x646c60` | scale propagation (root CanvasItem `set_scale`) |
| `CUIElement_DrawStretchedTexture @ 0x647d40` | `apply_position` texture-into-rect; the scaled-rect int truncation is D-MNU-4 |
| `CWnd_AccumulateAncestorOffset @ 0x6465e0` (was `sub_6465E0`) | Godot parent-child nesting (positions are parent-relative) |
| `CSpinListWnd_Render @ 0x64b220` + `CUISpinList_ParseXMLDefinition @ 0x64bd10` | `resolve_item` + `mnu_render_item_cell` (`mnu_item_cell.{h,cpp}`) + `build_spinlist` |
| `CSpinListWnd_CreateUpDownChildren @ 0x64b8b0` | `add_spin_button` (parent-relative SPINUP/SPINDOWN) — `nova_mnu_builder.cpp` |
| `CComboWnd_Construct @ 0x65be40` + `CComboWnd_Render @ 0x65bfd0` | `NovaMnuCombo` — `godot/engine/mnu/nova_mnu_combo.cpp` (dropdown geometry from authored LIST_BOX POSITION, D-MNU-7) |
| `dispatch_mouse_event @ 0x63ab00` (WM `0x200..0x20A` -> ids `0x1000001..0x100000B`; exclusive route to `g_ui_open_popup_wnd` `@ 0x63abb5`) | the catcher overlay owning all input while a dropdown is open — `NovaMnuCombo::open_popup` (D-MNU-11) |
| `scene_end_frame @ 0x63e600` (frame pump only the open popup `@ 0x63e691`; clears the per-frame mouse claim `scene+16` `@ 0x63e67e`) | overlay `MOUSE_FILTER_STOP` coverage (the Godot reimpl has no per-frame pump) |
| `CWnd_IsVisibleInHierarchy @ 0x646290` (popup-subtree-only while a popup is open `@ 0x646299`) | the overlay makes non-popup widgets unpickable; `NovaMnuCombo` closes on lost tree visibility |
| `combobox_handle_event @ 0x65c190` (toggle `0x3000001`; single-open `@ 0x65c210`; outside-press close `@ 0x65c261`; `LISTBOX_WND` `0x5000001` pick `@ 0x65c2fd`) | `NovaMnuCombo::on_overlay_gui_input` + `on_row_pressed` + `NovaMnuMenu::register_open_combo` (D-MNU-11) |
| `CWnd_SetShown @ 0x6480e0` (shown flag `+224`; popup flag `+660` registers `g_ui_open_popup_wnd`; was `sub_6480E0`) | popup lifecycle = overlay spawn/free in `open_popup`/`close_popup` |
| `CUIScene_SelectNodeByName @ 0x63b6b0` (screen switch clears the popup + capture globals `@ 0x63b6b8/0x63b7c4`) | `NovaMnuMenu::show_screen`/`set_current_screen`/`clear` -> `close_active_combo_popup` |
| `dispatch_mouse_event_to_children @ 0x647900` (active-combo priority peek `@ 0x647917`; press-capture bypass `@ 0x647932`) + `widget_process_mouse_event @ 0x647a00` (reverse child walk; per-frame claim `scene+16`) | Godot viewport GUI picking (reverse tree order) — reimpl code / not grillable |
| `CButtonWnd_HandleNamedEvent @ 0x658340` (press sets `g_ui_mouse_capture_wnd` `@ 0x65839c`, release clears `@ 0x6583ed`; pressed-texture swap; was `sub_658340`) | Godot `BaseButton` press capture — reimpl code / not grillable |
| `CWnd_EmitEventToNamedHandlerAndCallbacks @ 0x646970` (+28 sink -> +32 with own name + callback chain by `1<<HIBYTE(event)`; was `sub_646970`) | Godot signals (`pressed`/`gui_input`) replace the named-event plumbing — reimpl code / not grillable |
| `CWnd_SetParentAndAttach @ 0x6480a0` (parent ptr `+252` + child-array attach; was `sub_6480A0`) | Godot `add_child` — reimpl code / not grillable |
| `CMarqueeWnd_Construct @ 0x65c430` + `CMarqueeWnd_ParseXMLDefinition @ 0x65ceb0` + `marquee_load_credits_from_ini @ 0x65c5a0` | `build_marquee` -> `NovaCreditsPlayer` + `CbinCreditsResource::from_cbin_bytes` (CBIN datasource); `NovaMnuMarquee` (plain text) |
| `CUIWidget_HandleScriptedAction @ 0x649790` | `NovaMnuMenu::dispatch_action` — `godot/engine/mnu/nova_mnu_menu.cpp` |
| `UI_PopulateControlMappingList @ 0x55c0c0` + `refresh_control_mapping_list @ 0x55b320` | `opennova::controls::build_rows` (`libs/controls/src/controls.cpp`) + `nova_menu_shell.gd::_fill_control_mapping` |
| `UI_BuildKeyBindingLoadoutTable @ 0x559e50` (catalog `aAbsoluteTurnLe @ 0x8159cb`) | `libs/controls` `k_catalog` — `controls.cpp` |
| `KeyBinding_BuildCategoryPages @ 0x4966c0` (Class id -> name) | `controls::action_class_name` |
| `KeyBinding_GetKeyNameAndDisplayName @ 0x494c60` (VK -> display name) | `controls::key_name` |
| `KeyBinding_FormatBindingString @ 0x559a10` (`Ctrl-`/`Shift-`/`OR`) | `controls::format_binding` |
| `sub_55bcd0 @ 0x55bcd0` (device-mode radio, sets `dword_25db7d8`) | Keyboard/Mouse/Joystick radio wiring — `nova_menu_shell.gd::_seed_control_mapping` |
| `CTableWnd_ParseXMLContentDefinition @ 0x6427d0` (header `type="id"` `@ 0x64344a`, SCROLLBAR delegate `@ 0x643b22`) | `build_table` — `nova_mnu_builder.cpp` (id lookup + body justify + authored scrollbar) + `NovaMnuTable` |
| `NovaControlsModel` (Godot wrapper) | `godot/engine/mnu/nova_controls_model.cpp` |
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
(`sub_64AD90`) and `0x649790` folds into the `0x648120` body (vtable-reached, no direct
xrefs); naming/splitting them remains a proposed edit.

IDB state note (2026-06-23c combo-dropdown grill): renamed `sub_65C0D0 ->
CComboWnd_ParseXMLDefinition` (`0x65c0d0`, anchored: combo vtable[15] parse slot, delegates
to `CUIButtonWidget_ParseXMLAttributes` and feeds the embedded `CListWnd` at `this+384`).
Comments added and `idb_save` done: `0x644070` (row height = `this+201` / font-W default),
`0x644060` (`row_rect = this+13` from `<LIST_BOX>` POSITION), `0x65c16e` (combo parse feeds
the embedded `CListWnd` `this+384`). `CListWnd_DrawItems @ 0x643f30` and `CListWnd_Construct
@ 0x643bb0` were already curated-named and left as-is.

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
`dword_31C16D8 -> g_ui_open_popup_wnd`. Witness comments at `0x65c190`, `0x63ab00`,
`0x63e691`, `0x646299`, `0x647917`, `0x647932`, `0x63b6b0`, `0x6480e0`; `idb_save` done.
Open: `dword_31C16D4` / `dword_31C16DC` (cleared alongside capture by scripted actions
`@ 0x6498c8/0x6498d4`) remain unnamed — likely the focus/edit pair, unwitnessed.

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
round-tripped by `libs/mnu`.

## In-game armory — the WEAPON screen (engine-research 2026-07-09; re-grilled 2026-07-11)

The in-match loadout UI is **weapon.mnu's WEAPON screen** (a boot resource of the
game.mnu family) — NOT the loadout.mnu/LOADOUT screen found in some extracts, which
retail `Jointops.exe` never references (no `loadout` string exists in the image).
Reimpl: `ArmoryMenuCompanion` (companion) + the shell armory key +
`NovaSimulation.apply_local_player_loadout`; GUT `armory_menu_seam_test.gd`.

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
`NovaArmoryPresenter._unhandled_key_input` while the overlay is open.

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
| GRENADE_AMMO1..3 | `sub_566D70` (arg 0/1/2) | |
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
`sub_645240 @ 0x564B26..0x564B33`, and fills the ammo/type combos from it. It
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
(`libs/world/weapon_inventory` — sub-weapon expansion, requested-ammo pool
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
round — see the on-show section above). Open questions: ~~the SP-time
value/writer of `g_hostClassAllowMask`~~ (CLOSED 2026-07-18: MP = the per-class
host settings vs the mission entry's class word, SP = the mission entry word
directly — net-re §5.63); the runtime site that stamps the shipped default keys
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
