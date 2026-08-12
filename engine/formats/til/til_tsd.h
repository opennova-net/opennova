#pragma once

#include <cstddef>
#include <cstdint>

namespace opennova {

// The tileset surface-definition (.TSD) sidecar: the placed-tile
// surface-override table Terrain_GetSurfaceTypeAtPosition reads under placed
// tiles (docs/audio/lwf-dbf-sound-re.md D-SND-15).
//
// Retail probes `<tilestrip base>.TSD` at terrain texture init: the 256-entry
// tile-index -> surface-class table is memset to 0 first and filled only when
// the file exists. No shipped JO install carries one, so every retail placed
// tile reads class 0 = TSD_NULL — NOT the underlying charmap class.
// [orig: memset @ 0x60c5c9; exists probe @ 0x60c5d3; File_ParseASCIIFile
// @ 0x60c5ef with the per-line callback sub_604C00 @ 0x604c00, all in
// PolyTrn_InitTextures @ 0x60aaa0]

// The 20-entry TSD surface-class name table [orig: the 64-B-stride name table
// @ 0x8493f0]. The index IS the surface-type ordinal the runtime sampler
// returns — the charmap byte shares the domain (3 = snow footsteps, 7 = the
// off-island ocean default; the ammo impact table adds +4).
inline constexpr int TIL_TSD_SURFACE_NAME_COUNT = 20;
extern const char *const til_tsd_surface_names[TIL_TSD_SURFACE_NAME_COUNT];

// tile palette index (the overlay entry's tile_index byte) -> TSD ordinal.
// Zero-initialized = retail's memset table (every tile TSD_NULL).
struct TilSurfaceTable {
	uint8_t map[256] = {};
};

// Parse a .TSD text buffer over `out` (call on a fresh zeroed table).
// Structural translation of the retail chain: lines split STRICTLY on CRLF
// and a tail line without one loses its final byte to the terminator
// [orig: File_ParseASCIIFile @ 0x53d8de-0x53d8ec]; each line tokenizes on
// space/comma/tab with '"' quoting, cut by unquoted "//" or ';', clamped to
// 1000 chars and 30 tokens, missing tokens reading as empty strings
// [orig: Terrain_TokenizeConfigLine @ 0x53cb60]; a line whose first token
// starts with '/' is skipped [orig: @ 0x53d91e]. Per line, token 2 is matched
// case-insensitively against the TSD name table (no match -> 0) and, when
// token 1 carries the case-insensitive "INDEX_" prefix, the ordinal lands at
// atol(token1 + 6) [orig: sub_604C00 @ 0x604c00, store @ 0x604c62].
// Reimpl guards (both memory-safety class, noted in the record): an
// out-of-range index is skipped where retail writes out of bounds, and the
// name walk is bounded at the 20 real entries where retail's unbounded scan
// runs into adjacent data.
void til_tsd_parse(const char *text, size_t len, TilSurfaceTable &out);

} // namespace opennova
