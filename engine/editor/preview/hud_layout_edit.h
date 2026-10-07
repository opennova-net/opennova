#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <editor/model/edit.h>
#include <formats/def/def.h>
#include <runtime/hud/hud_elements.h>

namespace opennova::editor {

class TextDocument;

// Editing a HUD element through hudpos.def's lines (the editor deep-integration plan's DI-37): what moving
// an element, dragging a corner of one and setting one of its values write, each a value of a key's line
// the game takes [orig: HUD_ParseHudposToken @ 0x59F370], in the 1024 x 768 design space the HUD scales
// to the screen [orig: Viewport_ScaleToVirtualCoords @ 0x5D2B20]. An editor authoring aid, not a port:
// which keys place an element is the game's (each key's consumer, cited where the table names it); the
// values are written as the engine's own writer writes them (formats/def/def_hudpos_write.h).

// What a value of an element's key is to a drag: on the x or the y axis, a point that moves with the
// element, the near edge of a size (its left or top: a move takes it, a corner beside it too), the far edge
// (its right or bottom), or an extent (a width or a height the near edge measures from).
enum class HudAxis : uint8_t { X, Y };
enum class HudEdge : uint8_t { Point, Near, Far, Extent };

// One value of a key that places an element: the key (as the parser's _stricmp names it), the first value
// that tells its line from the key's others (a HUDLS_SLOT's slot; "" for none), the value's index on the
// line, its axis and what it is to a drag; `primary`: the line is written where the file has none (else a
// key the file lacks stays out: an icon whose texture it would have to name).
struct HudCoordinate {
	const char *key = "";
	const char *first = "";
	uint8_t index = 0;
	HudAxis axis = HudAxis::X;
	HudEdge edge = HudEdge::Point;
	bool primary = true;
};

// What a field of an element is: a number (a place, a size, a colour's channel, a level's flag), an
// alignment's word, or a name (a font).
enum class HudFieldKind : uint8_t { Number, Align, Name };

// One value of an element the Inspector shows and `set` writes: its id ("<key>.<name>", lower case:
// "ammocountpos.x", "weapon_textcolor.r", "huddeclut_wpngrp.level0", "fonthud1_hi"), its words, its key,
// line and index, its kind, the game's range of it (least to most; a name has none), what the game does
// with it and its witness, and its value now as the writer writes it.
struct HudField {
	std::string id;
	std::string words;
	std::string key;
	std::string first;
	int index = 0;
	HudFieldKind kind = HudFieldKind::Number;
	int least = 0;
	int most = 0;
	std::string range;
	std::string cite;
	std::string value;
};

// An element's coordinates (its keys' place values), in the order a move reads them, and whether a corner
// of it resizes it: the game reads a size for it on both axes (a rect's corners, a bar's width and height,
// a panel's pads).
const std::vector<HudCoordinate> &hud_element_coordinates(opennova::hud::HudElement element);
bool hud_element_resizable(opennova::hud::HudElement element);
// The HUDDECLUT row whose levels show the element ("" for none: no gate reads one for it), and the
// witness of its gate.
const char *hud_element_detail_row(opennova::hud::HudElement element);

// The element's fields over `hud` (the layout's model as the game reads it now): its places, its sizes,
// its hidden and alignment values, the fonts it writes in, its colour's channels, its detail level's four
// flags. None for an element no line places.
std::vector<HudField> hud_element_fields(opennova::hud::HudElement element, const def::DefHudPosDef &hud);

// One value of a key's line set: the line (its key and first value), the value's index, the text the
// writer writes for it.
struct HudValueChange {
	std::string key;
	std::string first;
	int index = 0;
	std::string value;
};
inline bool operator==(const HudValueChange &a, const HudValueChange &b) {
	return a.index == b.index && a.key == b.key && a.first == b.first && a.value == b.value;
}
inline bool operator!=(const HudValueChange &a, const HudValueChange &b) {
	return !(a == b);
}

// A coordinate's value now: what the layout reads for it, the game's own where the file leaves it out
// (NETWORKINDICATOR's reset corners [orig: CNetQuality_Reset @ 0x4C58C0], PAUSEDPOS's (1000, 4) where a
// field is 0 [orig: HUD_DrawPausedText @ 0x59D65C..0x59D670]). False where the key has no such line.
bool hud_coordinate_value(const def::DefHudPosDef &hud, const HudCoordinate &coordinate, int &out);

// Where a drag of an element began: each coordinate it takes (the primary key's whether the layout says it
// or not, another key's where the layout says it) with its value then. False for an element nothing places.
struct HudDragStart {
	opennova::hud::HudElement element = opennova::hud::HudElement::kCount;
	std::vector<HudCoordinate> coordinates;
	std::vector<int> values;
};
bool hud_drag_start(opennova::hud::HudElement element, const def::DefHudPosDef &hud, HudDragStart &out);

// A handle of an element: the element itself (a move) or one of its corners (a resize).
enum class HudHandle : uint8_t { Move, TopLeft, TopRight, BottomLeft, BottomRight };
// "move", "top_left", "top_right", "bottom_left", "bottom_right", and back (false for none).
const char *hud_handle_token(HudHandle handle);
bool hud_handle_from_token(const std::string &token, HudHandle &out);

// The values a drag of `handle` (dx, dy) design units from where it began writes, each landing on the
// design grid of `grid` units (whole units at 1): a move shifts every coordinate by how far its lead (the
// first near edge or point on the axis) moves to its grid place; a corner moves its two edges, each to its
// grid place, a near edge moved taking its extent with it, a size kept at least 1 unit. False with `error`
// for a corner of an element the game reads no size for.
bool hud_drag_changes(const HudDragStart &start, HudHandle handle, float dx, float dy, int grid,
                      std::vector<HudValueChange> &out, std::string &error);

// The edits that set the changes in `text` (the HUD layout's text; `hud` its model as the game reads it):
// each on the line the game takes of its key (hud_layout_line), the value's token replaced where the line
// holds it and differs, the values it lacks up to the one set added after its last; a key with no line a
// line of its own at the end of the text, as the writer writes it. Each edit carries `gesture` (0: none).
// False with `error` for a key the writer does not write alone.
bool hud_layout_edits(const TextDocument &text, const def::DefHudPosDef &hud, const std::vector<HudValueChange> &changes,
                      uint64_t gesture, std::vector<Edit> &out, std::string &error);

// A field set to `value` (its id an element's field's: hud_element_fields), checked against its kind and
// its range: the change, or false with `error` saying what it takes.
bool hud_field_change(opennova::hud::HudElement element, const def::DefHudPosDef &hud, const std::string &field,
                      const std::string &value, HudValueChange &out, std::string &error);

} // namespace opennova::editor
