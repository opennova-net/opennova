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
| `CUIElement_DrawFrame @ 0x64a210` | `add_frame` — `godot/engine/mnu/nova_mnu_builder.cpp` (8 border pieces + tiled fill) |
| `CUIWidget_HandleScriptedAction @ 0x649790` | `NovaMnuMenu::dispatch_action` — `godot/engine/mnu/nova_mnu_menu.cpp` |

IDB state note (2026-06-10): `0x64ad90` is still unnamed (`sub_64AD90`) and `0x649790`
currently folds into the `0x648120` function body (vtable-reached, no direct xrefs);
naming/splitting them is part of the proposed edits.

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
