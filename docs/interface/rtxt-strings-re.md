# RTXT localized string tables

Reverse-engineering record for NovaLogic's RTXT string-table format and the
TextResource runtime that consumes it, grilled against retail
`Jointops.exe` (Joint Operations: Combined Arms, kong IDB) on 2026-06-09.
Reimplementation: `libs/rtxt` (format), `godot/engine/rtxt` (RtxtStringFile
resource), `godot/engine/strings/nova_strings.gd` (runtime model),
`godot/modtools/strings` (ONED workspace).

Ground truth: all 98 RTXT-magic `.bin` files in the retail install
(language.pff) sweep clean through the invariants and byte-roundtrip below
(`tests/rtxt/jo_install_sweep_test.cpp`, gated on `OPENNOVA_JO_DIR`). Seven of
them are committed under `fixtures/rtxt/` and pinned by
`tests/rtxt/real_parity_test.cpp`.

## On-disk format

Defined by the engine's offset→pointer fixup, `TextResource_FixupPointers
@ 0x75D050`. Little-endian throughout.

| Offset | Field | Engine behaviour |
|---|---|---|
| +0 | magic `"RTXT"` (0x54585452) | **never validated** — the bytes `52 54 58 54` appear nowhere in Jointops.exe code; every retail file carries it |
| +4 | section-meta offset (= text data end) | rebased to a pointer at load; 4-byte aligned in 98/98 retail files, text blob zero-padded up to it |
| +8 | section-meta size **excluding** the section_count dword | never read at runtime by any family member; authoring-tool artifact (98/98 retail files obey the −4 rule) |
| +12 | entry_count | bounds for every walk (`@ 0x75D1E0` validates indices against it) |
| +16 | entry table, 16 B each | `{u32 text_offset, u32 packed_xy, u32 section_index, u32 pad=0}` |

After the entry table: the text blob (null-terminated cp1252 strings written
sequentially in entry order — no deduplication in any retail file; text_offset
is relative to the blob start, rebased against it by the fixup). At the
section-meta offset:

```
u32 section_count
section_count * { u32 first_key_offset, u32 string_count }
section names   (null-terminated, walked sequentially — no offsets)
entry keys      (null-terminated, one per entry, contiguous across sections)
```

`first_key_offset` is relative to `(section_meta_offset + 4)` and the fixup
rebases it into a pointer to the section's **first key string** — not a name
offset. `TextResource_FindKeyInSection @ 0x75D1E0` walks keys from that
pointer, bounded by `string_count`.

**Grouping invariant.** `TextResource_FindEntryBySectionAndKey @ 0x75D250`
computes an entry's index by accumulating preceding sections' `string_count`
and never reads the entry's own `section_index` field. Entries must therefore
sit in contiguous per-section runs, in section order. 98/98 retail files obey;
`section_index` is descriptive redundancy as far as the runtime is concerned.

`packed_xy` = `(x & 0xFFFF) | (y << 16)`, two int16s. Exactly four entries in
the whole retail game carry a nonzero value (`Hud_Bay`, `Hud_Malfunction`,
`Hud_Gear`, `Hud_Target` in gametext.bin); **no runtime reader was witnessed**
in Jointops.exe — the field is preserved opaquely and treated as a legacy
layout hint.

## Runtime model

Five table globals plus an override:

| Table | Global | Loaded by | Notes |
|---|---|---|---|
| gameerr.bin | `g_TextGameErr @ 0xB4C2A8` | `Game_InitSubsystems @ 0x4A6CD0` | fatal if missing; miss → `"??%s:%s??"` into `g_GameErrMissBuf` (`GameErr_GetString @ 0x4C2C60`) |
| gametext.bin | `g_TextGameText @ 0xB4C2AC` | same | item names: section `Item Names`, keys `STR_ITM%04i` (`Item_LoadLocalizedNames @ 0x49E1B0`) |
| vmacros.bin | `g_TextVMacros @ 0xB4C2B8` | same | section `macrotext` (`VMacros_GetMacroText @ 0x5B7170`); radio/emote menus walk it directly |
| keyhelp.bin | `g_TextKeyHelp @ 0xB4C2B0` | same | `KeyHelp_GetStringWithFallback @ 0x51ED40` gates on the *gametext* global (original quirk) |
| `<mission>.bin` | `g_TextMission @ 0xB4C2B4` | `TextResource_LoadMissionTextBin @ 0x51ED90` per mission start | falls back to `medmssn.bin`; WAC triggered text: section `Triggered Text`, keys `ID%03i` (`HUD_DisplayTriggeredText @ 0x51F190`) |
| override | `g_TextOverrideTable @ 0x33429B0` | `TextResource_LoadOverrideTable @ 0x75D5C0` from `Expansion_LoadAssets @ 0x4A4730` | the active expansion's `expansion\<exp>\<exp>.bin`; **consulted first by every lookup**, cleared with NULL |

Lookup family (all `stricmp`, first match wins):

| Address | Name (ours) | Behaviour |
|---|---|---|
| 0x75D050 | TextResource_FixupPointers | offsets → pointers (format definition) |
| 0x75D0B0 / 0x75D0F0 | TextResource_LoadFromArchive / LoadFile | load + align + tag `"TEXTMGR Resource"` + fixup |
| 0x75D150 / 0x75D560 | TextResource_GetKeyByIndex / 2 | key of entry[i]; identical twins |
| 0x75D1B0 | TextResource_GetEntryCount | header +12 (override-first) |
| 0x75D1E0 | TextResource_FindKeyInSection | bounded key walk within a section |
| 0x75D250 | TextResource_FindEntryBySectionAndKey | section walk + accumulated start index; recurses into the override table first |
| 0x75D300 | TextResource_GetSectionStringCount | row.string_count by section name |
| 0x75D380 | TextResource_FindIndexKeyInSection | re-resolve entry[i]'s key inside a named section; **no callers** in Jointops.exe |
| 0x75D450 | TextResource_FindEntryByKey | **flat** key walk across all entries |
| 0x75D4E0 | TextResource_FindKeyIndex | key → global index; quirk: override hit at index 0 is treated as a miss |
| 0x75D630 / 0x75D6F0 | FindEntryByKeyAtSectionIndex / BySectionAndIndex | (section, n-th) → key → flat find |
| 0x75D680 / 0x75D790 | FindKeyIndexByTextInSection / BySectionAndText | reverse lookup: display text → key index |

Wrappers bind tables to miss policies: `GameText_GetString @ 0x51EBD0` (empty
sentinel; 485 call sites), `GameText_GetStringWithFallback @ 0x51EB90` (caller
fallback), `MissionText_GetString @ 0x51EB50` / `GetString2 @ 0x51EC50`
(empty), `MissionText_GetStringByKey @ 0x51EC90` (flat),
`MissionText_GetStringByKeyOrGameText @ 0x51ECD0` (flat with gametext
fallback). Debug flag `0x10000` in `dword_24C1930` replaces every result with
`"&"` (unlocalized-string spotting). The menu path is separate:
`CUIStringTable_LookupString @ 0x6527C0` keeps a per-filename cache of loaded
tables and always looks up section `"menu"`; `.mnu` XML (`ty=ID`) and button
labels resolve through it (`UIStringTable_LookupAndDup @ 0x63B290` duplicates
the result, with the key itself as the miss fallback).

## {hot} accelerator marker

`CButtonWnd_SetLabel @ 0x6572F0` and `CUIButtonWidget_ParseXMLAttributes
@ 0x657C30`: `strstr` finds the **first** `{hot}` anywhere in the label, the
tail is shifted left 5 bytes in place, the marker's byte offset is recorded,
and the character now **at** that offset (the one that followed the marker)
registers as the accelerator. Later markers stay literal. `rtxt::strip_hotkey`
matches this exactly.

## Text encoding

Retail tables are cp1252 (67/98 files contain bytes ≥ 0x80 — curly quotes,
accents). `libs/rtxt` treats text as raw bytes (no transcoding);
`RtxtStringFile` decodes UTF-8 when the bytes are valid UTF-8, cp1252
otherwise, and re-encodes edited strings to cp1252 whenever every character
fits — pinned losslessly on retail data by
`godot/tests/strings_editor_test.gd::test_retail_fixture_cp1252_survives_string_roundtrip`.

## Divergences (D-RTXT-N)

| # | Finding | Status |
|---|---|---|
| 1 | Our writer emitted section row[+0] as a *name offset relative to the names blob*; engine needs *first-key offset relative to meta+4* | **fixed** (`rtxt.cpp write()`) |
| 2 | Editor allowed entries interleaved across sections; engine requires contiguous per-section runs | **fixed**: model invariant (`add_entry` / `set_entry_section_index` insert into the section's run); ungrouped *files* load faithfully, validate as errors, and are repairable via `normalize_grouping()` |
| 3 | Runtime lookup was flat-key only with empty-string misses; original is section-scoped with an override table and `??section:key??` miss marker | **fixed** as supersets: `File::find_in_section`, `RtxtStringFile.get_string_in_section`, `NovaStrings.register_table/set_override_table/lookup`; legacy flat APIs kept (they match `0x75D450`) |
| 4 | We require the RTXT magic on parse; the engine never checks it | **intentional**: 98/98 retail files carry it and resource-kind sniffing depends on it |
| 5 | Editor flagged duplicate keys globally; same key in different sections is legitimate retail data (8 shipped files), and even in-section duplicates ship (5 sections; engine first-match-wins) | **fixed**: per-section scoping, later in-section duplicate = warning |
| 6 | Header +8 was written as the full meta size; retail stores meta size − 4 (excl. count dword) | **fixed** |
| 7 | Text blob was written unaligned; retail zero-pads to a 4-aligned meta offset | **fixed** |
| 8 | `String::utf8()` mangled cp1252 text on display/edit | **fixed**: cp1252 fallback both directions in the Godot wrapper |
| 9 | Flat lookup map was last-wins on duplicate keys; engine's forward walk is first-wins | **fixed** (`build_lookup` emplace) |

## Verdict

**Matching** at byte level: parse→write reproduces all 98 retail RTXT bins
byte-for-byte, and the lookup/marker semantics mirror the witnessed functions.
The remaining deliberate differences are D-RTXT-4 (strictness) and the
unconsumed `packed_xy` field (no witnessed reader; preserved opaquely).

Leads deferred to the menu/mission workspaces: the `CUIStringTable` per-file
cache (menu workspace), per-mission `<mission>.bin` + `medmssn.bin` loading and
WAC `Triggered Text` wiring (mission runtime), expansion override loading from
`Expansion_LoadAssets`.

## Test inventory

- `tests/rtxt/{roundtrip,byte_equal,lookup,strip_hotkey,empty,section_lookup}_test.cpp` — unit behaviour
- `tests/rtxt/real_parity_test.cpp` — committed retail fixtures: raw-byte format invariants + byte roundtrip
- `tests/rtxt/jo_install_sweep_test.cpp` — full-install sweep (`OPENNOVA_JO_DIR`)
- `godot/tests/strings_editor_test.gd`, `strings_workspace_test.gd`, `nova_strings_test.gd` — editor model, shell seams, runtime registry
