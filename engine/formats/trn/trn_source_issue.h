#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova {

// What the terrain's readers do with a line of a .trn otherwise than the engine's record (TrnConfig, as load_trn
// reads it and save_trn writes it) holds it, one rule each; the twin of env::env_source_issues for the .trn
// [orig: Terrain_ParseConfigCallback @ 0x60F330 and Environment_LoadTimeOfDayConfig @ 0x57DB30's .trn pass over
// File_ParseASCIIFile @ 0x53D810's lines]. The rules are the readers'; a caller words each.
enum class TrnSourceRule : uint8_t {
	// The last line no CR LF ends loses its final byte to the walk (`token` the byte lost) [orig:
	// File_ParseASCIIFile @ 0x53D8C7..0x53D8F5].
	CutLastLine,
	// The same cut on a foliage block's "end", read "en": the block stays open, and is still a definition, since
	// the runtime takes every slot whose graphic is named [orig: Terrain_Init @ 0x60FD11..0x60FD16;
	// Foliage_RemapPixelToDefMask @ 0x5FF4E0]. A save writes its end.
	CutBlockEnd,
	// An environment keyword the record does not hold (it holds water_height, water_rgb and water_murk): the
	// environment's reader reads every line of the terrain, inside a foliage block too, and takes it for the
	// mission's environment before the .env [orig: Environment_LoadTimeOfDayConfig @ 0x57DB30, the .trn pass
	// @ 0x57DBCC..0x57DBDE]. Blocks; `token` the keyword as written.
	EnvironmentKey,
	// A water_rgb line short of its three values (`read` the values it has): the missing ones are where earlier,
	// longer lines left the tokenizer's slots; a save writes the three read.
	ShortColour,
	// A water_rgb value past a byte (`read` as read, `kept` the byte the parser keeps) [orig:
	// TimeOfDay_ParseProperty @ 0x57CAF6; Color_ScaleRGBAndPack @ 0x57F890].
	ColourByte,
	// A water_murk past 0.99, held at 0.99 [orig: TimeOfDay_ParseProperty @ 0x57CB7C..0x57CBA9].
	MurkClamp,
	// A match line of more than FOLIAGE_MATCH_CODES codes: up to seven are kept, the first four compared [orig:
	// Terrain_ParseConfigCallback @ 0x60F330, `idx < 8`; Foliage_RemapPixelToDefMask @ 0x5FF4E0, +0x108..+0x10B].
	MatchPastFour,
	// A match code past a byte (`read` as read, `kept` the byte stored) [orig: Terrain_ParseConfigCallback
	// @ 0x60F330, the byte store].
	MatchByte,
	// An attrib word neither forceon nor shadow (`token`): no arm reads it [orig: Terrain_ParseConfigCallback
	// @ 0x60F58B].
	AttribWord,
	// A color_lower or color_upper past 0..2 (`read` as read, `kept` the record's, foliage_normalize_color_mode).
	ColourMode,
	// Inside a foliage block, a line no block arm reads (`token` its keyword): only graphic, match, color_lower,
	// color_upper, attrib and end are read there [orig: Terrain_ParseConfigCallback @ 0x60F36C..0x60F5F0].
	BlockKeySkipped,
	// A fifth foliage block: the reader opens it and reads nothing more of the file [orig:
	// Terrain_ParseConfigCallback @ 0x60F330, `dword_31BC900 < 4` around every block key].
	FifthBlock,
	// A grid row before the grid's width (polytrn_sectorcount): none of its sectors is read.
	RowBeforeWidth,
	// A grid row short of the width (`read` its sectors, `kept` the width): the rest are where earlier, longer
	// lines left the tokenizer's slots [orig: Terrain_ParseConfigCallback @ 0x60F330, the polytrn_sectors arm].
	RowShort,
	// A grid row past the width (`read` its sectors, `kept` the width): the rest are not read.
	RowWide,
	// More than 16 grid rows (`read` the rows; its line the 17th row's): the gate refuses the terrain, and a save
	// writes the first 16 [orig: Terrain_LoadEnvironmentConfig @ 0x610940, the gate's `> 16`].
	TooManyRows,
	// polytrn_scale: read for the multiplayer file check alone, which the record does not hold [orig:
	// Terrain_LoadEnvironmentConfig @ 0x61096D, its default; @ 0x60C5FD feeds the CRC]. Blocks.
	ScaleKey,
	// polytrn_depthmap: an arm reads it [orig: Terrain_ParseConfigCallback @ 0x60F81D]; the record does not hold
	// it. Blocks.
	DepthmapKey,
	// A keyword no reader reads (`token` as written): skipped, and a save leaves it out.
	Skipped,
	// A keyword read again (its line the first, `again` the later one): the last line wins.
	ReadAgain,
};

// One line a reader reads otherwise than the record holds: its rule, whether it `blocks` (the record cannot hold
// what the game reads, so a save would change it; else input the game ignores or reads otherwise, which a save
// writes as the game read it), its line (1-based, every CR LF line counted), the keyword it is about (lower case;
// "end" for a cut block end), and the rule's values.
struct TrnSourceIssue {
	TrnSourceRule rule = TrnSourceRule::Skipped;
	bool blocks = false;
	size_t line = 0;
	std::string field;
	std::string token;
	int read = 0;
	int kept = 0;
	size_t again = 0;
};

// The lines of a .trn's text the terrain's readers read otherwise than the record holds, in line order.
std::vector<TrnSourceIssue> trn_source_issues(const std::string &text);

} // namespace opennova
