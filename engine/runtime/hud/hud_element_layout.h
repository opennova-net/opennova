#pragma once

// Which hudpos.def values place each element of the HUD's walk (hud_elements.h), as the game's readers
// take them: each key's values that put the element where it draws [orig: HUD_ParseHudposToken @ 0x59F370,
// each key's arm], the HUDDECLUT row whose levels gate its draw (the gate's witness beside it), the keys of
// the colours it draws in and whether it writes in the HUD's own font (hudpos.def's FONTHUD1). The values
// are in the 1024 x 768 design space the HUD scales to the screen (hud_math.h kDesignWidth / kDesignHeight)
// [orig: Viewport_ScaleToVirtualCoords @ 0x5D2B20]. The frame compiler reads the same keys through the
// layout fill (hud_layout_from_hudpos.h) and its own gates (hud_frame.cpp); this table names them per
// element, so a tool that moves or resizes an element writes the lines the game reads for it (the
// OpenNova Editor's HUD layout editing, ADR 0046 DI-37). The values are written as the engine's own
// writer writes them (formats/def/def_hudpos_write.h).

#include <cstdint>
#include <string>
#include <vector>

#include <formats/def/def.h>
#include <runtime/hud/hud_elements.h>

namespace opennova::hud {

// What a value of an element's key is: on the x or the y axis, a point that moves with the element, the
// near edge of a size (its left or top), the far edge (its right or bottom), or an extent (a width or a
// height the near edge measures from).
enum class HudAxis : uint8_t { X, Y };
enum class HudEdge : uint8_t { Point, Near, Far, Extent };

// One value of a key that places an element: the key (as the parser's _stricmp names it), the first value
// that tells its line from the key's others (a HUDLS_SLOT's slot; "" for none), the value's index on the
// line, its axis and what it is; `primary`: the key always places the element (another key places it only
// where the file authors that key: an icon whose texture a file would have to name).
struct HudCoordinate {
	const char *key = "";
	const char *first = "";
	uint8_t index = 0;
	HudAxis axis = HudAxis::X;
	HudEdge edge = HudEdge::Point;
	bool primary = true;
};

// An element's row: its coordinates (in the order a move reads them), the HUDDECLUT row whose levels show
// it ("" for none: no gate reads one for it) and that gate's witness, the keys of the colours it draws in,
// and whether it writes in the HUD's own font (hudpos.def's FONTHUD1_HI / FONTHUD1_LO).
struct HudElementLayout {
	HudElement element = HudElement::kCount;
	std::vector<HudCoordinate> coordinates;
	const char *detail = "";
	const char *detail_cite = "";
	std::vector<const char *> colours;
	bool fonts = false;
};

// The element's row; null for an element no row names (one no hudpos.def line places and no HUDDECLUT row
// gates).
const HudElementLayout *hud_element_layout(HudElement element);
// Its coordinates (none for an element no line places), whether the game reads a size for it on both axes
// (a rect's corners, a bar's width and height, a panel's pads), and its HUDDECLUT row ("" for none).
const std::vector<HudCoordinate> &hud_element_coordinates(HudElement element);
bool hud_element_resizable(HudElement element);
const char *hud_element_detail_row(HudElement element);

// A positioned text's key: the index of its value that hides what it places (-1: none) and of its
// alignment, and the hidden value's witness (its drawer's gate).
struct HudTextKey {
	const char *key;
	int hidden;
	int align;
	const char *hidden_cite;
};
// The key's row (compared without case), null for a key that places no positioned text.
const HudTextKey *hud_text_key(const std::string &key);

// A value as the writer writes it (def::hudpos_key_values) read as a whole number: false for a text that
// is not one (a word, a fraction, a number with anything after it).
bool hud_whole_value(const std::string &text, int &out);

// Whether the layout says anything of the coordinate's line: its values are not those a file without it
// reads (def::hudpos_unauthored).
bool hud_coordinate_authored(const def::DefHudPosDef &hud, const HudCoordinate &coordinate);

// A coordinate's value as the game reads it now: what the layout holds for it, the game's own where the
// file leaves it out (NETWORKINDICATOR's reset corners, kNetIndicatorResetPos; PAUSEDPOS's
// paused_text_pos). False where the key has no such line or value.
bool hud_coordinate_value(const def::DefHudPosDef &hud, const HudCoordinate &coordinate, int &out);

} // namespace opennova::hud
