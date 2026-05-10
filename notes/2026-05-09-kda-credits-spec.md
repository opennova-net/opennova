# KDA Credits Data Format and Editor Spec

This document is the consolidated source of truth for the KDA/CBIN credits
work. It supersedes the earlier design, implementation-plan, navigation,
polish, and verification notes from this branch.

## Goals

- Support NovaLogic `.kda` credits files as first-class data in OpenNova.
- Decode and encode the underlying CBIN binary format.
- Expose decoded credits as a Godot `CbinCreditsResource`.
- Provide a runtime `NovaCreditsPlayer` control for scrolling credits.
- Provide an ONED Credits workspace with visual editing, source editing,
  live preview, file inspection, and `.kda` save/load.
- Preserve practical round-trip behavior for known credits fixtures while
  making missing assets and malformed source edits visible to the user.

Out of scope:

- A main-menu integration that launches the runtime credits scene.
- Re-creating every unknown original-game rendering quirk.
- Preserving unknown/non-credits CBIN labels through the Godot resource layer.

## CBIN Binary Data Format

KDA credits files are CBIN files. All integer fields are little-endian.

### Header

The file begins with a 20-byte header:

| Offset | Field | Type | Meaning |
| --- | --- | --- | --- |
| `0x00` | `magic` | `uint32` | `0x4E494243`, the bytes `CBIN` |
| `0x04` | `string_offset` | `uint32` | Absolute file offset of the encoded string blob |
| `0x08` | `blob_length` | `uint32` | Encoded string blob length in bytes |
| `0x0C` | `string_count` | `uint32` | Number of null-terminated strings in the string table |
| `0x10` | `xor_key` | `uint32` | Initial key for payload obfuscation |

The encoded payload starts immediately after the header at `0x14`.
The decoded payload length is:

```text
encoded_length = string_offset + blob_length - 0x14
```

`string_offset + blob_length` must be within the file size.

### Obfuscation

The payload is decoded in place using the same operation as encoding:

```text
for each byte:
    key = rol32(key, 7)
    byte = byte XOR (key & 0xFF)
```

XOR is symmetric, so the same loop encodes and decodes.

### Decoded Payload Layout

The decoded payload contains an entry table followed by a string table.
The string table starts at:

```text
string_table_offset = string_offset - 0x14
```

Payload layout:

```text
uint32 label_count
LabelHeader[label_count]
NameEntry entries for each label, including one terminator per label
ValueEntry entries until string_table_offset
char[] null-terminated string table
```

`LabelHeader`:

| Field | Type | Meaning |
| --- | --- | --- |
| `str_idx` | `uint32` | 1-based string-table index for the label name |
| `element_count` | `uint32` | Number of named elements before this label's terminator |

`NameEntry`:

| Field | Type | Meaning |
| --- | --- | --- |
| `str_idx` | `uint32` | 1-based string-table index for the element name |
| `type` | `uint32` | `0` terminator, `1` one value, `2` two consecutive values |

`ValueEntry`:

| Field | Type | Meaning |
| --- | --- | --- |
| `value_raw` | `uint32` | Raw integer, raw float bits, or 1-based string index |
| `flags` | `uint32` | `4` string index, `2` float, otherwise integer |

String table rules:

- String indices are 1-based.
- Index `0` means no string.
- Strings are null-terminated byte strings.
- The portable encoder reuses `original_strings` when available so unchanged
  files can preserve string ordering.
- The portable encoder reuses the decoded `xor_key` when available; otherwise
  it generates a nonzero random key.

### Labels

The credits implementation understands two semantic labels:

- `env`: global render/editor settings.
- `text`: ordered credits content stream.

The portable decoder skips unknown labels by advancing over their declared
value counts. The portable encoder emits only `env` and `text`.

## Credits Semantic Model

The portable C++ model is `cbin::Credits`.

### ENV

Recognized environment keys:

| Key | Type | Default | Meaning |
| --- | --- | --- | --- |
| `scroll_rate` | float | `0.5` | Base scroll speed used by `NovaCreditsPlayer` |
| `vertical_space` | int | `14` | Pixel height for `<CR>` and scrolling image advances |
| `center_x` | int | `400` | Authored credits center value; stored and editable |

The portable model stores unrecognized ENV values in `env_extra`. The current
Godot `CbinCreditsResource` exposes only the three recognized keys.

### TEXT Stream

The `text` label is an ordered stream. Control entries update state or create
non-text entries.

| Token | Meaning |
| --- | --- |
| plain text | Create a text entry |
| plain text + font value | Create a text entry with a font |
| `<CR>` | Create a newline/spacer entry |
| `~Crrggbb` | Set current RGB text color for later text |
| `~JL` | Set current text justification to left |
| `~JC` | Set current text justification to center |
| `~JR` | Set current text justification to right |
| `~Ipath` | Create a scrolling image entry |
| `~Fx|y|path` | Create a fixed overlay image entry |

Parsing is case-insensitive for tilde control-code letters where implemented.

Control-state behavior:

- Color and justification are stateful.
- They do not become separate Godot resource entries.
- On load, their current values are applied to each following
  `CbinTextEntry`.
- On save, the saver emits `~C` and `~J` control entries whenever a text
  entry's color or justification differs from the current emitted state.

Text spacing and image spacing:

- `CbinNewlineEntry` advances stream Y by `vertical_space`.
- `~I` scrolling image entries also advance stream Y by `vertical_space`.
- `~F` fixed images do not advance stream Y, but they still have a logical
  stream Y used for fade timing and editor synchronization.

Text normalization:

- When loading `.kda`, underscores in decoded text are shown as spaces.
- When saving `.kda`, spaces in text are written as underscores.
- The editor source view presents readable text and writes through the same
  resource model.

Image forms:

- `~Ipath`: scrolls with the content, is centered in the text area, and does
  not fade independently.
- `~Fx|y|path`: renders as a fixed viewport overlay at `display_x`,
  `display_y`; if `display_y` is `0`, the player vertically centers it.
  Fixed overlays fade in/out based on their logical stream trigger Y.

## Godot Resource Model

### Resource Classes

`CbinCreditsResource` is a Godot `Resource` with:

- `scroll_rate: float`
- `vertical_space: int`
- `center_x: int`
- `entries: Array[CbinEntry]`

Entry subclasses:

| Class | Fields | Serialized form |
| --- | --- | --- |
| `CbinTextEntry` | `text`, `font`, `color`, `justify` | text plus optional emitted `~C`, `~J`, font |
| `CbinNewlineEntry` | none | `<CR>` |
| `CbinImageEntry` | `texture`, `texture_name`, `display_x`, `display_y`, `advances_y` | `~Ipath` if `advances_y`, otherwise `~Fx|y|path` |

Enums:

```text
CbinEntryType:
    CBIN_ENTRY_TEXT = 0
    CBIN_ENTRY_NEWLINE = 1
    CBIN_ENTRY_IMAGE = 2

CbinJustify:
    CBIN_JUSTIFY_LEFT = -1
    CBIN_JUSTIFY_CENTER = 0
    CBIN_JUSTIFY_RIGHT = 1
```

Mutation/signaling contract:

- Entry property setters emit the entry's `changed` signal.
- `CbinCreditsResource` connects to child entry `changed` signals and re-emits
  its own `changed` signal.
- Structural operations (`set_entries`, `add_entry`, `insert_entry`,
  `remove_entry`, `clear_entries`, successful `from_text`) emit both
  `changed` and `entries_structure_changed`.
- Non-structural child edits emit only `changed` through the parent.
- This split lets the live preview rebuild on content edits without forcing
  the visual block list to recreate cards and steal focus.

### KDA Load/Save

Load path:

```text
.kda file
-> KdaResourceFormatLoader
-> cbin::decode_credits
-> CbinCreditsResource
-> Credits editor and/or NovaCreditsPlayer
```

Save path:

```text
CbinCreditsResource
-> KdaResourceFormatSaver
-> cbin::Credits
-> cbin::encode
-> .kda file
```

Loader behavior:

- Rejects files without CBIN magic.
- Pushes a Godot error and returns null on open/decode failure.
- Collapses portable `Color` and `Justify` entries into text-entry state.
- Creates `CbinNewlineEntry` for `<CR>`.
- Creates `CbinImageEntry` for both `~I` and `~F`.
- Records the original image filename in `texture_name` even when the texture
  cannot be resolved.

Saver behavior:

- Converts spaces in text back to underscores.
- Emits color and justify control entries only when state changes.
- Writes scrolling images as `~Ipath`.
- Writes fixed overlays as `~Fx|y|path`.
- Skips unknown entry subclasses.

### Text Source Format

`CbinCreditsResource.to_text()` emits an editable text form:

```text
# Credits Text Format
# Lines starting with # are comments
# Control codes: ~Crrggbb (color), ~JL/~JC/~JR (justify)
# Newline: <CR>
# Images: ~Ipath (scrolling), ~Fx|y|path (fixed overlay)
# Text with font: text [FONTNAME]

[ENV]
scroll_rate=0.50
vertical_space=14
center_x=400

[TEXT]
~CFF0000
~JR
Lead Programming
Mark Davis
<CR>
~F0|0|cr2.png
```

`from_text()` rules:

- Ignores blank lines and lines beginning with `#`.
- Reads `[ENV]` key/value lines until `[TEXT]`.
- Recognizes `scroll_rate`, `vertical_space`, and `center_x`.
- Parses `[TEXT]` using the same controls listed above.
- Parses `text [FONTNAME]` as a text entry with font lookup.
- Fails atomically on malformed `~F` lines or any input producing zero
  entries.
- On failure, the existing resource state is preserved.
- On success, old entries are disconnected, new entries are connected, and the
  resource emits `changed` plus `entries_structure_changed`.

## Asset Resolution

Fonts:

- Base path: `res://assets/fonts/`.
- Font names resolve as `{name}.fnt`.
- Exact case is tried first.
- A case-insensitive directory scan is used as fallback.

Textures:

- Base path: `res://assets/textures/`.
- Exact path is tried first.
- A case-insensitive filename match is tried next.
- If not found, common alternate extensions are tried for the same basename:
  `.png`, `.pcx`, `.tga`, `.jpg`, `.bmp`.
- Missing textures are allowed. The resource keeps `texture_name`, the editor
  shows a missing-image warning, and the player renders a visible placeholder.

The editor image card uses the same practical lookup policy and stores only
the filename portion of typed or picked paths.

## Runtime Player Spec

`NovaCreditsPlayer` is a Godot `Control` that renders a
`CbinCreditsResource`.

Public properties and methods:

| API | Meaning |
| --- | --- |
| `credits_resource` | Resource to render |
| `font_base_path` | Font lookup base path |
| `texture_base_path` | Texture lookup base path |
| `speed_scale` | Multiplier on resource scroll speed |
| `autoplay` | Starts playback when ready |
| `play()` | Start from scroll offset 0 |
| `pause()` | Pause without resetting |
| `resume()` | Continue after pause |
| `stop()` | Stop and reset internal playback state |
| `is_playing()` | True only when playing and not paused |
| `set_scroll_offset(offset)` | Set authored scroll position directly |
| `get_scroll_offset()` | Current scroll offset |
| `content_y_for_entry(index)` | Logical stream Y for editor sync |
| `entry_index_at_scroll_center()` | Entry nearest preview center |
| `rebuild()` | Rebuild generated visual nodes |
| `highlight_entry(index)` / `clear_highlight()` | Debug/editor highlighting |

Signals:

- `started`
- `finished`
- `scroll_offset_changed(offset)`

Layout and playback:

- The player clips its own contents.
- A generated `Content` control holds scrolling labels, spacers, and `~I`
  images.
- Fixed `~F` images are siblings behind the content so they remain viewport
  overlays while text can render over them.
- Generated labels, image nodes, and placeholders ignore mouse input so the
  editor/player can handle preview wheel events.
- Text is positioned inside a text area from the right edge of fixed images to
  the viewport right edge.
- Left, center, and right justification are applied within that text area.
- Scroll speed is `scroll_rate * speed_scale * 60` pixels per second.
- Content Y is `viewport_height - scroll_offset`.
- Playback finishes when `scroll_offset > content_height + viewport_height`.
- Resource `changed` signals schedule a deferred rebuild to coalesce rapid
  edits.

Fixed overlay fade:

- Each fixed image stores a logical `trigger_y` in the scroll stream.
- During scroll, `trigger_y + content_y` is compared against the viewport top
  and bottom.
- Alpha ramps from `0` at/outside the viewport edge to `1` over a 50 px fade
  zone.

Editor sync helpers:

- Every entry has a logical stream Y in `entry_stream_y_`.
- `content_y_for_entry()` returns the logical stream Y, not the rendered node
  Y. This is critical for `~F` images because their rendered node is a fixed
  overlay.
- `entry_index_at_scroll_center()` chooses the entry whose logical stream Y is
  closest to `scroll_offset - viewport_height * 0.5`.

## ONED Credits Workspace Spec

The workspace adapter is `CreditsEditorWorkspace`.

Workspace contract:

| Method | Value |
| --- | --- |
| `get_workspace_id()` | `credits` |
| `get_workspace_label()` | `Credits` |
| open filter | `*.kda,*.KDA ; Credits` |
| new label | `New Credits` |
| open label | `Open Credits...` |
| save label | `Save Credits` |
| save-as label | `Save Credits As...` |

Document behavior:

- `CreditsEditorDocument` extends the shared editor document base.
- Owns the current `CbinCreditsResource`.
- Starts with a fresh empty resource.
- `open_kda(path)` loads through `ResourceLoader`.
- `save_current()` writes current path.
- `save_as(dir_path)` writes `{current basename or credits}.kda`.
- Resource `changed` marks the document dirty and emits state changes.

Inspector behavior:

- Shows current filename plus dirty marker.
- Shows total/text/newline/image counts.
- Shows missing-image count when any image has a stored path/name but no
  resolved texture.
- Shows ENV summary: scroll rate, spacing, center.

Editor scene:

```text
CreditsEditor
  HSplit
    LeftPane
      ModeBar: Visual / Source
      EnvBar: scroll_rate, vertical_space, center_x
      WarningBar
      ContentStack
        BlockListHost
          BlockScroll
            BlockList
          sticky AddRow: + Text, + Image, + Newline
        SourceViewHost
          Apply button
          CodeEdit
          StatusBar
    RightPane
      PreviewHost
        Toolbar: Play, Pause, Stop, Speed
        NovaCreditsPlayer
```

Visual/source mode:

- Visual and Source buttons are exclusive toggles.
- Visual mode shows the block list.
- Source mode shows the text editor.
- Source `Apply` parses through `from_text()`.
- Parse failure keeps the user's source text visible and reports failure in
  the status bar.

ENV bar:

- `scroll_rate`: min `0.05`, max `5.0`, step `0.05`.
- `vertical_space`: min `0`, max `200`, step `1`.
- `center_x`: min `0`, max `1280`, step `1`.
- Changes write directly to `CbinCreditsResource`.

Block cards:

- All cards have a drag handle, type chip, delete button, selection affordance,
  and stable sizing.
- Text cards expose inline text, font picker, color picker, and L/C/R
  justification buttons.
- Image cards expose thumbnail, filename, picker, Scroll/Fixed toggle, and
  X/Y offsets for Fixed mode.
- Newline cards expose only type and delete.
- Alignment and image-mode buttons use exclusive button groups.
- Image path edits commit on Enter and focus loss.
- Deleting a selected entry selects the next entry, or the previous entry when
  deleting the last entry.

Block list reconcile:

- `CreditsEditorBlockList` keeps an entry-to-card dictionary.
- Structural resource changes reconcile existing cards instead of rebuilding
  the whole list.
- Non-structural entry edits refresh the bound card in place.
- Insert actions add after the selected entry; with no selection they append.
- Successive inserts chain naturally because the inserted entry becomes
  selected.
- Drag reorder changes resource order and preserves selected entry identity.

Preview toolbar:

- `Play` starts from the beginning unless paused, where it resumes.
- `Pause` is enabled only while playing and changes `Play` text to `Resume`.
- `Stop` stops playback and sets the scroll offset so the first entries are
  visible.
- Speed writes to `NovaCreditsPlayer.speed_scale`.

Preview and list sync:

- The center of the editor list viewport and the center of the preview are the
  canonical sync anchors.
- Scrolling the editor list finds the visible card nearest the list viewport
  center and seeks the preview so that entry is centered.
- Selecting a card seeks the preview to that entry's logical stream Y.
- Playback and manual preview scrubbing update editor selection to the entry
  nearest the preview center.
- Mouse wheel over the preview scrubs `scroll_offset` by 48 px per wheel step.
- Preview scrubbing works while stopped, paused, or playing.
- The player, right pane, preview host, and left pane clip their contents so
  fixed images cannot overlap the editor panel.

## Runtime Credits Scene

`godot/game/credits.tscn` contains:

- Full-rect root `Control`.
- Black background.
- `NovaCreditsPlayer` bound to `res://assets/credits/nlist.kda`.

`godot/game/credits.gd`:

- Starts the player.
- Emits `credits_done` when playback finishes or when the user skips.
- Treats ESC, Enter, and mouse click as skip inputs.

## Error Handling

Load errors:

- Non-CBIN files are rejected.
- Decode failures push a Godot error and return null.
- Missing font or texture assets do not fail loading.

Editor source errors:

- Empty source, ENV-only source, and malformed fixed-image lines fail.
- Failed source parse preserves the existing resource.
- Successful parse replaces entries atomically.

Missing images:

- Missing image filenames are preserved in `texture_name`.
- The editor warning bar reports the number of missing images.
- The workspace inspector reports missing images.
- The player shows a colored placeholder with the missing filename.

## Verification Status

Latest verification for this branch:

- `cmake --build build-godot --config Debug`: passed.
- `ctest --test-dir build -C Debug -R cbin --output-on-failure`: passed.
- Focused Credits editor GUT tests: 15/15 passed.
- Broader Credits/KDA GUT set: 56/56 passed.
- Full `bash scripts/test_godot.sh`: 60/62 passed.

Known full-suite failures at the time of this spec:

- `terrain_editor_import_export_test.gd::test_dvxi5_import_then_export_matches_fixture_bytes`
  fails because `NovaTerrainBuilder` cannot create one exported terrain mesh
  file under the user data temp export directory; exported fixture files are
  then missing.
- `terrain_editor_workstation_test.gd::test_workstation_starts_with_two_domain_workspaces`
  still expects two workspaces, while this branch registers Credits as a third
  workspace.

These failures are outside the Credits/KDA implementation path.

## File Map

Portable CBIN:

- `libs/cbin/include/cbin/cbin.h`
- `libs/cbin/src/cbin.cpp`
- `tests/cbin/cbin_unit_test.cpp`
- `tests/cbin/cbin_roundtrip_test.cpp`
- `fixtures/cbin/nlist.reference.kda`

Godot engine integration:

- `godot/engine/cbin/cbin_asset_lookup.h`
- `godot/engine/cbin/cbin_credits_resource.{h,cpp}`
- `godot/engine/cbin/kda_resource_format.{h,cpp}`
- `godot/engine/cbin/nova_credits_player.{h,cpp}`
- `godot/engine/register_types.cpp`

Editor/runtime:

- `godot/modtools/credits/credits_editor.{tscn,gd}`
- `godot/modtools/credits/credits_editor_block_card.{tscn,gd}`
- `godot/modtools/credits/credits_editor_block_list.gd`
- `godot/modtools/credits/credits_editor_document.gd`
- `godot/modtools/credits/credits_editor_preview.gd`
- `godot/modtools/credits/credits_editor_source_view.gd`
- `godot/modtools/editor/credits_workspace.gd`
- `godot/game/credits.{tscn,gd}`
- `godot/assets/credits/nlist.kda`
- `godot/assets/textures/cr*.png`, `bink.tga`, and Godot import metadata

Godot tests:

- `godot/tests/test_kda_load.gd`
- `godot/tests/test_kda_from_text.gd`
- `godot/tests/test_cbin_signal_propagation.gd`
- `godot/tests/test_credits_editor_document.gd`
- `godot/tests/test_credits_workspace.gd`
- `godot/tests/test_credits_source_view.gd`
- `godot/tests/test_credits_editor_scene.gd`
- `godot/tests/test_credits_block_list_focus.gd`
- `godot/tests/test_credits_block_list_reconcile.gd`
- `godot/tests/test_credits_insert_after_selected.gd`
- `godot/tests/test_credits_inspector.gd`
- `godot/tests/test_credits_scroll_sync.gd`

## Acceptance Criteria

The branch is considered ready for merge when:

- `.kda` files load as `CbinCreditsResource`.
- Known fixture `.kda` files round-trip through `libs/cbin` tests.
- The runtime credits scene plays the vendored fixture.
- The ONED Credits workspace can create, open, edit, save, and save-as `.kda`
  credits documents.
- Visual card edits immediately refresh the live preview without stealing text
  focus.
- Source edits parse atomically and preserve state on failure.
- Image filenames are preserved and missing images are surfaced visibly.
- The visual list, source view, and preview remain synchronized by logical
  stream position.
- The focused Credits/KDA automated test set passes.
