#include <formats/til/til_tsd.h>
#include <base/io/ascii_config.h>
#include <base/io/strutil.h>

#include <cstdlib>

namespace opennova {

// [orig: the 64-B-stride name table @ 0x8493f0, "TSD_NULL".."TSD_FLESH"]
const char *const til_tsd_surface_names[TIL_TSD_SURFACE_NAME_COUNT] = {
	"TSD_NULL",      "TSD_DIRT",       "TSD_GRASS",   "TSD_SNOW",
	"TSD_CEMENT",    "TSD_SAND",       "TSD_PACKEDDIRT", "TSD_UNDERWATER",
	"TSD_RAILROAD",  "TSD_MUD",        "TSD_ICE",     "TSD_QUICKSAND",
	"TSD_STONE",     "TSD_WOOD",       "TSD_METAL",   "TSD_GLASS",
	"TSD_CLOTH",     "TSD_FOLIAGE",    "TSD_HMETAL",  "TSD_FLESH",
};

namespace {

// One tokenized line -> the Terrain_ParseTsdRow row apply. [orig: Terrain_ParseTsdRow @ 0x604c00]
void apply_row(const char *key, const char *value, TilSurfaceTable &out) {
	// token 2 vs the name table, case-insensitive; no match -> 0 (retail's
	// unbounded walk is bounded at the 20 real entries here) [orig: @ 0x604c23].
	uint8_t ordinal = 0;
	for (int i = 0; i < TIL_TSD_SURFACE_NAME_COUNT; ++i) {
		if (strutil::iequals(value, til_tsd_surface_names[i])) {
			ordinal = static_cast<uint8_t>(i);
			break;
		}
	}
	// "INDEX_" prefix, case-insensitive; the slot is atol of the suffix
	// [orig: @ 0x604c46/@ 0x604c62]. Out-of-range slots are skipped where
	// retail writes out of bounds (reimpl guard).
	if (!strutil::starts_with_icase(key, "INDEX_")) return;
	const long slot = std::strtol(key + 6, nullptr, 10);
	if (slot < 0 || slot > 255) return;
	out.map[slot] = ordinal;
}

} // namespace

void til_tsd_parse(const char *text, size_t len, TilSurfaceTable &out) {
	// The shared retail line walk and tokenizer (io/ascii_config.h)
	// [orig: File_ParseASCIIFile @ 0x53d8de-0x53d8ec; Terrain_TokenizeConfigLine
	//  @ 0x53cb60; the '/'-led first token skip @ 0x53d91e].
	io::for_each_config_line(text, len, [&](const io::ConfigTokens &tokens) {
		apply_row(tokens.token(0), tokens.token(1), out);
	});
}

} // namespace opennova
