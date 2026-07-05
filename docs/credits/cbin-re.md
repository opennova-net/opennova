# CBIN credits codec — reverse-engineering record (PARTIAL)

Structure-mapping record for the original engine's **CBIN** obfuscated-text
container (the format behind the credits `.kda`) — its 20-byte header, counted
string table, and ROL32/XOR cipher. The reimplementation surface is `libs/cbin`
(`Header` / `encode_buffer` / `decode_buffer`) and the Godot credits player
(`godot/engine/cbin`). Binary: retail **Jointops.exe** (IDB
`Jointops.exe.kong.i64`). This file is the committed home for the `D-CBIN`
catalog. Produced 2026-07-05 (PAR-R5), **read-only** — witnessed from raw
disassembly without defining functions in the shared IDB.

**Status: PARTIAL.** The CODEC is now witnessed against retail and MATCHES
`libs/cbin` in **both directions**: magic + 20-byte header + the ROL32/XOR cipher,
and — resolving D-CBIN-2 — the READ path (retail has 8 cipher sites incl. decode
loops, so it deciphers CBIN, not just writes it; our symmetric cipher matches).
The one remaining piece is the credits `~C`/`~F`/`~J` markup interpreter (D-CBIN-1).
Converts credits from `UNAUDITED` to *tracked (partial)*.

## How it was witnessed

The CBIN magic `0x4E494243` ("CBIN") is a binary constant, not a string — a
string search misses it, but `find_bytes 43 42 49 4E` locates the writer at
`~0x75e250` (an IDA-undefined region; `add_func` to decompile writes the shared
read-only IDB and is correctly denied, so this is disassembly-only).

## Verdict table

| Component | Verdict | Evidence |
| --- | --- | --- |
| Magic + 20-byte header | **MATCHING** | writer `@ 0x75e311`: `mov dword [esp+68h], 4E494243h` … `mov dword [esp+6Ch], 0` → `fwrite(hdr, 20)`; `libs/cbin` `Header` is 20 B (magic/string_offset/blob_length/string_count/xor_key) |
| ROL32/XOR cipher | **MATCHING** | cipher loop `@ 0x75e348` (below) is byte-for-byte `libs/cbin` `encode_buffer` |
| Counted string table | **MATCHING (structure)** | the writer scans/counts NUL-terminated strings (`@ 0x75e250`: strlen-style scan + `++count`) into the blob, as `libs/cbin` models |
| Credits `~C`/`~F`/`~J` markup | **NEEDS-RE** | D-CBIN-1 |
| CBIN read path (does retail read it?) | **RESOLVED 2026-07-05** | D-CBIN-2 — retail decodes CBIN (8 cipher sites); our symmetric cipher matches |

## The cipher (`@ 0x75e348`, per 4-byte group)

```
loc_75E348:
  mov  ebx, [esi]        ; ebx = key   (key lives at [esi])
  rol  ebx, 7            ; key = ROL32(key, 7)
  mov  [esi], ebx        ; store rotated key back
  mov  al,  [esi]        ; al = low byte of the ROTATED key  (little-endian)
  xor  [esp+ecx+0Ch], al ; blob[ecx] ^= (key & 0xFF)
  inc  ecx
  cmp  ecx, 4
  jb   loc_75E348        ; 4-byte group; an outer loop repeats over the blob
```

This is exactly `libs/cbin`:

```cpp
void encode_buffer(std::vector<uint8_t>& data, uint32_t key) {
    for (size_t i = 0; i < data.size(); ++i) {
        key = rol32(key, 7);          // rol ebx, 7
        data[i] ^= (key & 0xFF);      // xor [buf], low-byte-of-key
    }
}
```

Rotate-left-7 then XOR-low-byte, key carried across bytes; XOR is its own inverse,
so `decode_buffer` == `encode_buffer` (as `libs/cbin` has it). MATCHING.

## D-CBIN divergence catalog

| ID | Class | Disposition | One-liner |
|---|---|---|---|
| D-CBIN-1 | B | NEEDS-RE | Credits markup consumers: `libs/cbin` models `~Crrggbb` (color), `~F x|y|path` / `~Ipath` (image), `~JL/~JC/~JR` (justify), `<CR>` (newline). The retail markup interpreter (the credits scroller that consumes the decoded string table) is not yet witnessed — pin it to confirm the tag set + the D-MNU-6 custom-font/image resolution. |
| D-CBIN-2 | B | **RESOLVED (MATCHING) 2026-07-05** | Read path CONFIRMED present: `find_bytes C1 C3 07` (`rol ebx,7`) finds **8** cipher sites in the CBIN codec region `0x75e158–0x75e914`, not just the one writer — the DECODE loops (e.g. `@ 0x75e473`: `rol ebx,7` then read+`xor` source bytes) prove retail READS/deciphers CBIN, disproving the write-only hypothesis. The magic isn't re-checked as an immediate in the reader (it trusts the header). Our `decode_buffer` == `encode_buffer` (XOR is symmetric, rol-7) so the read direction MATCHES. Which specific caller invokes the read for the credits scroller rides D-CBIN-1. |

The related `D-MNU-6` (credits custom `~F` fonts / `~I` images not resolved from
the resource root) is the one previously-tracked credits divergence; it rides
D-CBIN-1's markup grill.

## Cross-references

- Reimpl: `libs/cbin` (`cbin.h`/`cbin.cpp`), `libs/refs/src/refs_cbin.cpp`,
  `godot/engine/cbin` (credits resource + player), the ONED credits editor.
- The one tracked credits divergence: `D-MNU-6` ([mnu/menu-re.md](../mnu/menu-re.md)).
