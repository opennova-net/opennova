# ADR 0008 - PFF writer policy

Consolidated from `notes/pff_writer_re.md` (Phase 0 PFF writer/timestamp/checksum RE) on 2026-06-10.

Status: accepted. Gates the `.pff` save path (libs/pff writer + the ONED Archive Tool).
Addresses are Jointops.exe retail (imagebase 0x400000).

## Context

Before authoring `.pff` archives we needed to know what the retail loader actually validates,
and whether the game binary contains a writer whose field semantics (in particular the unknown
entry `timestamp` and `checksum` fields) we would have to reproduce.

### What the read path consumes

The 36-byte directory entry has confirmed field usage: the engine consumes **only**
`flags(+0)`, `offset(+4)`, `size(+8)`, and `name(+16, 16B)`. `timestamp(+12)` and
`checksum(+32)` are **never read, never validated** anywhere in the open/find/sort/load path.
There is no CRC check of any kind.

| Function | Reads from entry | Reads from header |
|---|---|---|
| [orig: PFF_Open @ 0x7682e0] | (table only) | `header_size(+0)`, then re-reads header into pff+132: `num_entries(+140)`, `entry_size(+144)`, `file_table_offset(+148)`. **Magic is NOT validated.** |
| [orig: PFF_SortEntries @ 0x768280] | name(+16) | num_entries(+140) |
| [orig: PFF_CompareEntryNames @ 0x768200] / [orig: PFF_CompareSearchNameToEntry @ 0x768240] | name(+16) (`strcmp` against entry+16) | — |
| [orig: PFF_FindEntry @ 0x7685d0] | name(+16) via bsearch (uppercased, trailing-`0x20` trimmed) | num_entries(+140), table ptr(+152) |
| [orig: PFF_OpenFile @ 0x7688a0] | offset(+4) | — |
| [orig: PFF_LoadFileToMemory @ 0x768920] | flags(+0) bit0 = ENCRYPTED, offset(+4), size(+8) | — |

Container decrypt is confirmed identical to our port: when `flags & 1`, each byte is XORed
with a running key evolved by `ROL4(key, 7)`, seed `0x0312A4CE` (decrypt loop inside
[orig: PFF_LoadFileToMemory @ 0x768920], at 0x768a03).

### No writer exists in the binary

Searching the code segment for the PFF magics as immediates found zero matches for all three:
`0x33464650` (PFF3), `0x34464650` (PFF4), `0x34460001` (BHD). No function constructs a PFF
header, so **no PFF writer/packer is compiled into Jointops.exe** (consistent with
[orig: PFF_Open @ 0x7682e0] never comparing the magic). The shipping archives were built by an
external NovaLogic dev tool we do not have — there is no in-binary checksum algorithm to
reproduce even if we wanted to.

## Decision

Our writer (`libs/pff/src/pff_writer.cpp`, driven by the Archive Tool in
`godot/engine/pff/nova_pff_archive.cpp`) emits:

- **New entries**: `timestamp = 0`, `checksum = 0`. No checksum is computed — there is nothing
  to reproduce, and the loader ignores both fields.
- **Retained entries**: original `timestamp`/`checksum` values copied **verbatim** from the
  source archive, regardless of content.
- **Magic**: preserved from the source archive on resave; `PFF3` default for newly created
  archives. The loader never compares it, so this is purely for tidiness and third-party-tool
  compatibility.
- Payload bytes are never transformed: the stored bytes are exactly the supplied bytes
  (container-XOR-encrypted iff `flags & PFF_FLAG_ENCRYPTED`), and the directory is emitted
  sorted by normalized name, matching the load-time sort order.

## Consequences

- **Retail-safe by construction**: the shipping loader reads only flags/offset/size/name, so
  archives with zeroed timestamp/checksum load identically to retail-built ones.
- **Resaves are directory-faithful for untouched entries**: verbatim copy means a retained
  entry's record differs only where offsets legitimately move.
- **Third-party caveat**: community PFF tools (not the game) *may* read timestamp/checksum.
  Verbatim-for-retained + zero-for-new is the safest stance without a sample of such a tool.
  Revisit only if a concrete tool rejects our output.
- Because the loader validates neither magic nor any checksum, format identification of our
  output rests on the preserved/default magic alone — another reason to keep emitting it
  faithfully even though the engine would accept anything.
