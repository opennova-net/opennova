# MNU/MNS menu UI: engine correspondence and equivalence

How Joint Operations parses, lays out, sounds, and draws its `.mnu` menus, as
witnessed in the original engine, and how `libs/mnu`, `libs/mnu_xml`, `libs/mns`,
and `godot/engine/mnu` correspond to it.

Reverse-engineered from `Jointops.exe` (Joint Operations: Combined Arms, imagebase
`0x400000`, IDB `Jointops.exe.kong.i64`). All addresses are absolute in that image.
This is the menu-slice grill (2026-06-09), closing the render/sound divergences the
2026-06-01 format pass deferred.

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

## Verdict

**matching** (after this grill) on: type factory + unknown-token preservation, the
three-stage POSITION layout + texture-into-rect, the 8-piece frame + data-driven
stencil insets, per-state sound slots + per-element bank resolution + the set/layer
play and master volume, the XML entity policy, and the format round-trip (ADR 0002).

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

Deferred (unwitnessed or out of bar; backlog, not blocking):

- MONOGRAM frame-overlay placement (subtype 0x600, drawn separately from the frame
  pass) - currently a centered heuristic.
- The hotkey consume-on-effect vtable path (`CUIWidget_HandleScriptedAction @ 0x649790`,
  no direct xrefs) and MUSICVAR host dedup policy.
- `GLB_TABLE/RADIOEDIT/LAN_LIST/GOPHER` runtime behavior (multiplayer-browser widgets;
  build as containers, behavior rides with the net workspace).
- Real 3D globe; the table SCROLLBAR delegate `(*(tableWnd[244]+60)) @ 0x643b22`.
