#include <editor/preview/hud_layout_edit.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <map>
#include <utility>

#include <base/io/ascii_config.h>
#include <base/io/crt_ftol.h>
#include <base/io/strutil.h>
#include <editor/documents/hud_layout_type.h>
#include <editor/model/text_document.h>
#include <formats/def/def_hudpos_write.h>
#include <runtime/hud/hud_frame.h>

namespace opennova::editor {

namespace {

using opennova::hud::HudElement;

constexpr HudAxis X = HudAxis::X;
constexpr HudAxis Y = HudAxis::Y;
constexpr HudEdge Point = HudEdge::Point;
constexpr HudEdge Near = HudEdge::Near;
constexpr HudEdge Far = HudEdge::Far;
constexpr HudEdge Extent = HudEdge::Extent;

// The design space the HUD's positions are authored in [orig: Viewport_ScaleToVirtualCoords @ 0x5D2B20].
constexpr int kDesignWidth = 1024;
constexpr int kDesignHeight = 768;

// Where the parser reads each key an element's fields name [orig: HUD_ParseHudposToken @ 0x59F370, the
// arm's _stricmp].
struct KeyWitness {
	const char *key;
	const char *cite;
};
constexpr KeyWitness kKeyWitness[] = {
	{ "BREATHTIME", "[orig: HUD_ParseHudposToken, the BREATHTIME arm @ 0x59FB44]" },
	{ "GAMEINFO", "[orig: HUD_ParseHudposToken, the GAMEINFO arm @ 0x5A05D1]" },
	{ "ZONEINFO", "[orig: HUD_ParseHudposToken, the ZONEINFO arm @ 0x5A0636]" },
	{ "STATICFRAME", "[orig: HUD_ParseHudposToken, the STATICFRAME arm @ 0x5A0A42]" },
	{ "HUDHEALTH", "[orig: HUD_ParseHudposToken, the HUDHEALTH arm @ 0x5A136F; HUD_DrawHealthBar @ 0x5A2E50]" },
	{ "HUDWPNICON", "[orig: HUD_ParseHudposToken, the HUDWPNICON arm @ 0x5A0976]" },
	{ "HUDGEARTEXT", "[orig: HUD_ParseHudposToken, the HUDGeartext arm @ 0x59FFB2]" },
	{ "CARGOPOS", "[orig: HUD_ParseHudposToken, the cargopos arm @ 0x5A0C99]" },
	{ "PARACHUTEICON", "[orig: HUD_ParseHudposToken, the ParachuteIcon arm @ 0x5A0A9C]" },
	{ "ARMORICON", "[orig: HUD_ParseHudposToken, the ArmorIcon arm @ 0x5A0AF6]" },
	{ "SHOWIMPACTDISTPOS", "[orig: HUD_ParseHudposToken, the ShowImpactDistPos arm @ 0x5A0C55]" },
	{ "HUDSTANCEPOS", "[orig: HUD_ParseHudposToken, the HUDSTANCEPOS arm @ 0x5A0BCD]" },
	{ "AMMOCOUNTPOS", "[orig: HUD_ParseHudposToken, the AMMOCOUNTPOS arm @ 0x59FC31]" },
	{ "HUDWEAPONNAME", "[orig: HUD_ParseHudposToken, the HUDWEAPONNAME arm @ 0x59F925]" },
	{ "HUDCLIP", "[orig: HUD_ParseHudposToken, the HUDCLIP arm @ 0x5A0932]" },
	{ "HUDHEAT", "[orig: HUD_ParseHudposToken, the HUDHEAT arm @ 0x5A144F; HUD_DrawWeaponHeatBar @ 0x599700]" },
	{ "HUDTIMECLOCK", "[orig: HUD_ParseHudposToken, the HUDTIMECLOCK arm @ 0x59FD9E]" },
	{ "HUDPLAYERCOUNT", "[orig: HUD_ParseHudposToken, the HUDPLAYERCOUNT arm @ 0x59FDE2]" },
	{ "HUDPOWERBAR", "[orig: HUD_ParseHudposToken, the HUDPOWERBAR arm @ 0x5A152F; HUD_DrawPowerThrowChargeBar @ 0x599830 draws "
	                 "(x, y)-(x+w, y+h)]" },
	{ "HUDWPDINFO", "[orig: HUD_ParseHudposToken, the HUDWPDINFO arm @ 0x5A02C9]" },
	{ "HUDTEAMXY", "[orig: HUD_ParseHudposToken, the HUDTEAMXY arm @ 0x5A130A]" },
	{ "HUDLS_SLOT", "[orig: HUD_ParseHudposToken, the HUDLS_SLOT arm @ 0x59FF4B..0x59FF9C]" },
	{ "HUDSCOPERANGEXY", "[orig: HUD_ParseHudposToken, the HUDSCOPERANGEXY arm @ 0x5A123E]" },
	{ "HUDSCOPEZEROXY", "[orig: HUD_ParseHudposToken, the HUDSCOPEZEROXY arm @ 0x5A1282]" },
	{ "HUDSCOPEMAGXY", "[orig: HUD_ParseHudposToken, the HUDSCOPEMAGXY arm @ 0x5A12C6]" },
	{ "LFP_FLAGS", "[orig: HUD_ParseHudposToken, the LFP_FLAGS arm @ 0x5A0549]" },
	{ "HUDSPINMAPX1", "[orig: HUD_ParseHudposToken, the HUDSPINMAPX1 arm @ 0x59F7D4]" },
	{ "HUDSPINMAPY1", "[orig: HUD_ParseHudposToken, the HUDSPINMAPY1 arm @ 0x59F831]" },
	{ "HUDSPINMAPX2", "[orig: HUD_ParseHudposToken, the HUDSPINMAPX2 arm @ 0x59F804]" },
	{ "HUDSPINMAPY2", "[orig: HUD_ParseHudposToken, the HUDSPINMAPY2 arm @ 0x59F85E]" },
	{ "MAPCOORDS", "[orig: HUD_ParseHudposToken, the mapcoords arm @ 0x5A08DB]" },
	{ "HUDVEHSTANCEPOS", "[orig: HUD_ParseHudposToken, the HUDVEHSTANCEPOS arm @ 0x5A0C11]" },
	{ "HUDSYSTEXT", "[orig: HUD_ParseHudposToken, the HUDSYSTEXT arm @ 0x5A09FE]" },
	{ "HUDCHATTEXT", "[orig: HUD_ParseHudposToken, the HUDCHATTEXT arm @ 0x5A09BA]" },
	{ "HUDORDERS", "[orig: HUD_ParseHudposToken, the HUDORDERS arm @ 0x5A04C1]" },
	{ "MRCLIPPYNORMAL", "[orig: HUD_ParseHudposToken, the MRCLIPPYNORMAL arm @ 0x59FA1E; the panel x, y, then its pads "
	                    "past the text @ 0x5B6F44..0x5B6F85]" },
	{ "MRCLIPPYALTERNATE", "[orig: HUD_ParseHudposToken, the MRCLIPPYALTERNATE arm @ 0x59FA88; the panel x, y, then its "
	                       "pads past the text @ 0x5B6F44..0x5B6F85]" },
	{ "PAUSEDPOS", "[orig: HUD_ParseHudposToken, the PAUSEDPOS arm @ 0x59FC96]" },
	{ "NETWORKINDICATOR", "[orig: HUD_ParseHudposToken, the NETWORKINDICATOR arm @ 0x59F98A]" },
	{ "WEAPON_TEXTCOLOR", "[orig: HUD_ParseHudposToken, the weapon_textcolor arm @ 0x5A0FA1]" },
	{ "HUD_TEXTCOLOR", "[orig: HUD_ParseHudposToken, the hud_textcolor arm @ 0x5A0F3D; copied into g_HUDColors[2] every "
	                   "frame by HUD_RenderAllOverlays @ 0x5A8100]" },
	{ "HUDHEALTHBORDER", "[orig: HUD_ParseHudposToken, the HUDHEALTHBORDER arm @ 0x5A13D9]" },
	{ "HUDHEATBORDER", "[orig: HUD_ParseHudposToken, the HUDHEATBORDER arm @ 0x5A14B9]" },
	{ "STANCEICON_COLOR", "[orig: HUD_ParseHudposToken, the stanceicon_color arm @ 0x5A0EC7]" },
	{ "FONTHUD1_HI", "[orig: HUD_ParseHudposToken, the fonthud1_hi arm @ 0x5A15CD; HUD_SelectHudposFont @ 0x591890 takes it "
	                 "above 640 pixels wide]" },
	{ "FONTHUD1_LO", "[orig: HUD_ParseHudposToken, the fonthud1_lo arm @ 0x5A1599; HUD_SelectHudposFont @ 0x591890 takes it "
	                 "at 640 pixels wide and below]" },
};

std::string witness_of(const std::string &key) {
	for (const KeyWitness &row : kKeyWitness)
		if (strutil::iequals(key, row.key)) return row.cite;
	return "[orig: HUD_ParseHudposToken @ 0x59F370]";
}

// The keys whose third value hides what they place and whose fourth is its alignment (the positioned
// texts), and those whose third is the alignment (no hidden value).
struct TextKey {
	const char *key;
	int hidden; // its index, -1 none
	int align;
	const char *hidden_words;
	const char *hidden_cite;
};
constexpr TextKey kTextKeys[] = {
	{ "AMMOCOUNTPOS", 2, 3, "0 draws the ammo count; another value hides it",
	  "[orig: HUD_DrawWeaponAmmoAndName @ 0x5939D0, its gate on the hidden value @ 0x5939F3]" },
	{ "HUDWEAPONNAME", 2, 3, "0 draws the weapon's name; another value hides it",
	  "[orig: HUD_DrawWeaponAmmoAndName @ 0x5939D0, its gate on the hidden value]" },
	{ "GAMEINFO", 2, 3, "0 draws the game info; another value ends its drawer",
	  "[orig: HUD_DrawGameTimerOverlay @ 0x59CC80, its gate @ 0x59CC94 / @ 0x59CCD5]" },
	{ "HUDPLAYERCOUNT", 2, 3, "0 draws the player count; another value hides it",
	  "[orig: HUD_DrawScoreOverlay @ 0x593E50, `cmp dword_272366C, 0` @ 0x593E64]" },
	{ "HUDTEAMXY", 2, 3, "0 draws the team line; another value hides it",
	  "[orig: HUD_DrawTeamIdLine @ 0x59AA30, `cmp dword_2723834, 0` @ 0x59AA44]" },
	{ "HUDWPDINFO", 2, 3, "0 draws the box around the distance; another value hides the box alone",
	  "[orig: HUD_DrawWaypointNameAndDistance @ 0x5947A0; HUD_ParseHudposToken @ 0x5A02C3]" },
	{ "BREATHTIME", -1, 2, "", "" },
	{ "ZONEINFO", -1, 2, "", "" },
};

const TextKey *text_key(const std::string &key) {
	for (const TextKey &row : kTextKeys)
		if (strutil::iequals(key, row.key)) return &row;
	return nullptr;
}

// An element: its coordinates, the HUDDECLUT row that shows it and its gate, the keys of the colours it
// draws in, and whether it writes in the HUD's own font (hudpos.def's FONTHUD1).
struct ElementRow {
	HudElement element;
	std::vector<HudCoordinate> coordinates;
	const char *detail;
	const char *detail_cite;
	std::vector<const char *> colours;
	bool fonts;
};

HudCoordinate at(const char *key, uint8_t index, HudAxis axis, HudEdge edge, bool primary = true, const char *first = "") {
	HudCoordinate out;
	out.key = key;
	out.first = first;
	out.index = index;
	out.axis = axis;
	out.edge = edge;
	out.primary = primary;
	return out;
}

std::vector<HudCoordinate> point(const char *key, uint8_t x = 0, bool primary = true) {
	return { at(key, x, X, Point, primary), at(key, uint8_t(x + 1), Y, Point, primary) };
}

std::vector<HudCoordinate> joined(std::initializer_list<std::vector<HudCoordinate>> parts) {
	std::vector<HudCoordinate> out;
	for (const std::vector<HudCoordinate> &part : parts) out.insert(out.end(), part.begin(), part.end());
	return out;
}

std::vector<HudCoordinate> corners(const char *key) {
	return { at(key, 0, X, Near), at(key, 1, Y, Near), at(key, 2, X, Far), at(key, 3, Y, Far) };
}

std::vector<HudCoordinate> extents(const char *key) {
	return { at(key, 0, X, Near), at(key, 1, Y, Near), at(key, 2, X, Extent), at(key, 3, Y, Extent) };
}

std::vector<HudCoordinate> slots() {
	static const char *const kSlots[] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "10" };
	std::vector<HudCoordinate> out;
	for (const char *slot : kSlots) {
		out.push_back(at("HUDLS_SLOT", 1, X, Point, false, slot));
		out.push_back(at("HUDLS_SLOT", 2, Y, Point, false, slot));
	}
	return out;
}

const std::vector<ElementRow> &element_rows() {
	static const std::vector<ElementRow> rows = {
		{ HudElement::BreathBar, point("BREATHTIME"), "BREATHTIME", "[orig: the BREATHTIME slot's cmp @ 0x59D6F3]", {}, false },
		{ HudElement::GameInfo, joined({ point("GAMEINFO"), point("ZONEINFO", 0, false) }), "", "", { "HUD_TEXTCOLOR" }, false },
		{ HudElement::Frame, point("STATICFRAME", 1), "", "", {}, false },
		{ HudElement::Health, corners("HUDHEALTH"), "DMGBAR", "[orig: the slot-7 cmp @ 0x5A7C99]", { "HUDHEALTHBORDER" },
		  false },
		{ HudElement::Instruments,
		  joined({ point("HUDWPNICON"), point("HUDGEARTEXT", 0, false), point("CARGOPOS", 0, false),
		           point("PARACHUTEICON", 1, false), point("ARMORICON", 1, false) }),
		  "", "", {}, false },
		{ HudElement::OpticalCues, point("SHOWIMPACTDISTPOS"), "", "", {}, true },
		{ HudElement::Stance, point("HUDSTANCEPOS"), "WPNGRP", "[orig: HUD_RenderOverlays @ 0x5A7CBE..0x5A7D55]",
		  { "STANCEICON_COLOR" }, false },
		{ HudElement::AmmoCount, point("AMMOCOUNTPOS"), "WPNGRP", "[orig: the slot-8 cmp @ 0x5A7CC8]",
		  { "WEAPON_TEXTCOLOR" }, true },
		{ HudElement::WeaponName, point("HUDWEAPONNAME"), "WPNGRP", "[orig: the slot-8 cmp @ 0x5A7D04]",
		  { "WEAPON_TEXTCOLOR" }, true },
		{ HudElement::ClipIndicator, point("HUDCLIP"), "WPNGRP", "[orig: the slot-8 cmp @ 0x5A7D42]", { "STANCEICON_COLOR" },
		  false },
		{ HudElement::Crosshair, {}, "XHAIRS", "[orig: the slot-13 cmp in HUD_DrawCrosshair @ 0x592757]", {}, false },
		{ HudElement::Heat, corners("HUDHEAT"), "", "", { "HUDHEATBORDER" }, false },
		{ HudElement::Clock, joined({ point("HUDTIMECLOCK"), point("HUDPLAYERCOUNT", 0, false) }), "CLOCK",
		  "[orig: HUD_RenderOverlays, the slot-21 cmp @ 0x5A7D75]", {}, false },
		{ HudElement::Power, extents("HUDPOWERBAR"), "PWRBAR", "[orig: the slot-20 cmp @ 0x5A7DD2]", {}, false },
		{ HudElement::Waypoint, point("HUDWPDINFO"), "WAYPOINT", "[orig: the slot-3 cmp @ 0x5A7DB8]", { "HUD_TEXTCOLOR" },
		  false },
		{ HudElement::TeamIdLine, point("HUDTEAMXY"), "TEAMID", "[orig: HUD_DrawTeamIdLine's slot-19 gate @ 0x5A7DE7]",
		  { "HUD_TEXTCOLOR" }, false },
		{ HudElement::WeaponSlotBar, slots(), "HUDLS", "[orig: the HUDLS slot's gate @ 0x5A7DF7]", {}, false },
		{ HudElement::ScopeDetails,
		  joined({ point("HUDSCOPERANGEXY"), point("HUDSCOPEZEROXY", 0, false), point("HUDSCOPEMAGXY", 0, false) }), "", "",
		  {}, false },
		{ HudElement::LfpPanel, point("LFP_FLAGS"), "", "", {}, false },
		{ HudElement::Spinmap,
		  { at("HUDSPINMAPX1", 0, X, Near), at("HUDSPINMAPY1", 0, Y, Near), at("HUDSPINMAPX2", 0, X, Far),
		    at("HUDSPINMAPY2", 0, Y, Far), at("MAPCOORDS", 0, X, Point, false), at("MAPCOORDS", 1, Y, Point, false) },
		  "SPINMAP", "[orig: the slot-17 cmp @ 0x5A86E8]", {}, false },
		{ HudElement::VehiclePanel, point("HUDVEHSTANCEPOS"), "", "", {}, false },
		{ HudElement::Feed, joined({ point("HUDSYSTEXT"), point("HUDCHATTEXT", 0, false) }), "CHAT",
		  "[orig: the slot bit @ 0x59AD33]", {}, false },
		{ HudElement::SquadOrders, point("HUDORDERS"), "", "", {}, false },
		{ HudElement::Tip, extents("MRCLIPPYNORMAL"), "", "", {}, false },
		{ HudElement::PausedText, point("PAUSEDPOS"), "", "", {}, false },
		{ HudElement::TipAlternate, extents("MRCLIPPYALTERNATE"), "", "", {}, false },
		{ HudElement::NetQuality,
		  { at("NETWORKINDICATOR", 0, X, Point), at("NETWORKINDICATOR", 1, Y, Point), at("NETWORKINDICATOR", 2, X, Point),
		    at("NETWORKINDICATOR", 3, Y, Point), at("NETWORKINDICATOR", 4, X, Point), at("NETWORKINDICATOR", 5, Y, Point) },
		  "", "", {}, false },
	};
	return rows;
}

const ElementRow *row_of(HudElement element) {
	for (const ElementRow &row : element_rows())
		if (row.element == element) return &row;
	return nullptr;
}

std::string lower(std::string text) {
	for (char &c : text) c = char(std::tolower(static_cast<unsigned char>(c)));
	return text;
}

// Whether the coordinate's key reads a size on its axis.
bool sized(const ElementRow &row, const HudCoordinate &coordinate) {
	for (const HudCoordinate &each : row.coordinates)
		if (strutil::iequals(each.key, coordinate.key) && each.axis == coordinate.axis && each.edge == Extent) return true;
	return false;
}

// A coordinate's field name: x / y for a point (numbered past the first of its key and axis), left / top /
// right / bottom for a rect's edges, x / y / width / height for a place and its size.
std::string coordinate_name(const ElementRow &row, const HudCoordinate &coordinate) {
	const bool horizontal = coordinate.axis == X;
	switch (coordinate.edge) {
	case Near:
		if (sized(row, coordinate)) return horizontal ? "x" : "y";
		return horizontal ? "left" : "top";
	case Far: return horizontal ? "right" : "bottom";
	case Extent: return horizontal ? "width" : "height";
	case Point: break;
	}
	int count = 0, place = 0;
	for (const HudCoordinate &each : row.coordinates) {
		if (!strutil::iequals(each.key, coordinate.key) || std::strcmp(each.first, coordinate.first) != 0 ||
				each.axis != coordinate.axis || each.edge != Point)
			continue;
		if (&each == &coordinate || each.index == coordinate.index) place = count;
		++count;
	}
	std::string name = horizontal ? "x" : "y";
	if (count > 1) name += std::to_string(place + 1);
	return name;
}

std::string field_id(const char *key, const char *first, const std::string &name) {
	std::string id = lower(key);
	if (first && first[0]) id += std::string("_") + first;
	return id + "." + name;
}

bool number_of(const std::string &text, int &out) {
	if (text.empty()) return false;
	char *end = nullptr;
	const double value = std::strtod(text.c_str(), &end);
	if (!end || *end != '\0' || !std::isfinite(value) || value != std::floor(value)) return false;
	out = io::retail_ftol_sse2(value);
	return true;
}

int snapped(double value, int grid) {
	const int step = std::max(grid, 1);
	return int(std::lround(value / step)) * step;
}

// A value's token in a line of the text: where it is, as the game's tokenizer cuts the line.
struct LineTokens {
	int count = 0;
	size_t begin[io::kConfigMaxTokens] = {};
	size_t end[io::kConfigMaxTokens] = {};
	bool quoted[io::kConfigMaxTokens] = {};
};

LineTokens tokens_of(const std::string &line) {
	LineTokens out;
	// The walk cuts a line at its CR LF (a tail line with none loses its last character).
	const std::string cut = line + "\r\n";
	io::for_each_config_line_span(cut.data(), cut.size(), [&](io::ConfigTokens &tokens, const io::ConfigLineSpan &span) {
		out.count = tokens.count;
		for (int i = 0; i < tokens.count; ++i) {
			out.begin[i] = span.token_begin[i];
			out.end[i] = span.token_end[i];
			out.quoted[i] = span.token_begin[i] > 0 && line[span.token_begin[i] - 1] == '"';
		}
		return true;
	});
	return out;
}

// Whether a token as written reads as `value` (a number by its value, a word without case).
bool reads_as(const std::string &written, const std::string &value) {
	if (strutil::iequals(written, value)) return true;
	int a = 0, b = 0;
	const bool numeric = !value.empty() && (std::isdigit(static_cast<unsigned char>(value[0])) || value[0] == '-');
	if (!numeric || !number_of(value, b)) return false;
	a = io::retail_ftol_sse2(io::retail_atof_n(written.c_str(), written.size()));
	return a == b && io::retail_atof_n(written.c_str(), written.size()) == double(b);
}

std::string unquoted(const std::string &value) {
	if (value.size() >= 2 && value.front() == '"' && value.back() == '"') return value.substr(1, value.size() - 2);
	return value;
}

struct Splice {
	size_t offset;
	size_t length;
	std::string text;
};

// Whether the layout says anything of the coordinate's line: its values are not those of a file without it.
bool key_authored(const def::DefHudPosDef &hud, const HudCoordinate &coordinate) {
	std::vector<std::string> values, none;
	if (!def::hudpos_key_values(hud, coordinate.key, coordinate.first, values)) return false;
	def::hudpos_key_values(def::hudpos_unauthored(), coordinate.key, coordinate.first, none);
	return values != none;
}

} // namespace

const std::vector<HudCoordinate> &hud_element_coordinates(HudElement element) {
	static const std::vector<HudCoordinate> none;
	const ElementRow *row = row_of(element);
	return row ? row->coordinates : none;
}

bool hud_element_resizable(HudElement element) {
	bool x = false, y = false;
	for (const HudCoordinate &coordinate : hud_element_coordinates(element)) {
		if (coordinate.edge != Far && coordinate.edge != Extent) continue;
		(coordinate.axis == X ? x : y) = true;
	}
	return x && y;
}

const char *hud_element_detail_row(HudElement element) {
	const ElementRow *row = row_of(element);
	return row ? row->detail : "";
}

bool hud_coordinate_value(const def::DefHudPosDef &hud, const HudCoordinate &coordinate, int &out) {
	if (strutil::iequals(coordinate.key, "NETWORKINDICATOR") && !hud.network_indicator_present) {
		out = opennova::hud::kNetIndicatorResetPos[coordinate.index];
		return true;
	}
	if (strutil::iequals(coordinate.key, "PAUSEDPOS") && (hud.paused_pos[0] == 0 || hud.paused_pos[1] == 0)) {
		out = coordinate.index == 0 ? 1000 : 4;
		return true;
	}
	std::vector<std::string> values;
	if (!def::hudpos_key_values(hud, coordinate.key, coordinate.first, values) || coordinate.index >= values.size())
		return false;
	return number_of(values[coordinate.index], out);
}

std::vector<HudField> hud_element_fields(HudElement element, const def::DefHudPosDef &hud) {
	std::vector<HudField> out;
	const ElementRow *row = row_of(element);
	if (!row) return out;
	const std::string design = "the 1024 x 768 design space the HUD scales to the screen [orig: "
	                           "Viewport_ScaleToVirtualCoords @ 0x5D2B20]";
	// The places and the sizes.
	std::vector<std::string> seen_keys;
	for (const HudCoordinate &coordinate : row->coordinates) {
		int value = 0;
		if ((!coordinate.primary && !key_authored(hud, coordinate)) || !hud_coordinate_value(hud, coordinate, value))
			continue;
		HudField field;
		const std::string name = coordinate_name(*row, coordinate);
		field.id = field_id(coordinate.key, coordinate.first, name);
		field.key = coordinate.key;
		field.first = coordinate.first;
		field.index = coordinate.index;
		field.kind = HudFieldKind::Number;
		const bool horizontal = coordinate.axis == X;
		field.least = 0;
		field.most = horizontal ? kDesignWidth : kDesignHeight;
		field.words = std::string(coordinate.key) + (coordinate.first[0] ? std::string(" ") + coordinate.first : "") +
		              " " + name;
		field.range = std::string(horizontal ? "0 to 1024" : "0 to 768") + ", " + design;
		field.cite = witness_of(coordinate.key);
		field.value = std::to_string(value);
		out.push_back(std::move(field));
		if (std::find(seen_keys.begin(), seen_keys.end(), lower(coordinate.key)) == seen_keys.end())
			seen_keys.push_back(lower(coordinate.key));
	}
	// The positioned texts' hidden value and alignment.
	for (const std::string &key : seen_keys) {
		const TextKey *text = text_key(key);
		std::vector<std::string> values;
		if (!text || !def::hudpos_key_values(hud, key, "", values)) continue;
		if (text->hidden >= 0 && size_t(text->hidden) < values.size()) {
			HudField field;
			field.id = key + ".hidden";
			field.key = text->key;
			field.index = text->hidden;
			field.kind = HudFieldKind::Number;
			field.least = 0;
			field.most = 1;
			field.words = std::string(text->key) + " hidden";
			field.range = std::string("0 or 1: ") + text->hidden_words;
			field.cite = text->hidden_cite;
			field.value = values[size_t(text->hidden)];
			out.push_back(std::move(field));
		}
		if (size_t(text->align) < values.size()) {
			HudField field;
			field.id = key + ".align";
			field.key = text->key;
			field.index = text->align;
			field.kind = HudFieldKind::Align;
			field.words = std::string(text->key) + " anchor";
			field.range = "left, right or center: the text's place is its left end, its right end or its middle "
			              "(any other word reads left)";
			field.cite = "[orig: HUD_ParseTextAlignment @ 0x59D6B0]";
			field.value = values[size_t(text->align)];
			out.push_back(std::move(field));
		}
	}
	// The fonts the HUD writes it in.
	if (row->fonts)
		for (const char *key : { "fonthud1_hi", "fonthud1_lo" }) {
			std::vector<std::string> values;
			def::hudpos_key_values(hud, key, "", values);
			HudField field;
			field.id = key;
			field.key = key;
			field.kind = HudFieldKind::Name;
			field.words = std::string(key) + " font";
			field.range = std::strcmp(key, "fonthud1_hi") == 0 ? "a .fnt file: the HUD's font above 640 pixels wide"
			                                                     : "a .fnt file: the HUD's font at 640 pixels wide and below";
			field.cite = witness_of(key);
			field.value = values.empty() ? std::string() : unquoted(values[0]);
			out.push_back(std::move(field));
		}
	// Its colours' channels: a, r, g, b where the key reads four, r, g, b where three.
	for (const char *key : row->colours) {
		std::vector<std::string> values;
		if (!def::hudpos_key_values(hud, key, "", values)) continue;
		const bool argb = !strutil::iequals(key, "HUD_TEXTCOLOR") && !strutil::iequals(key, "WEAPON_TEXTCOLOR");
		static const char *const kArgb[] = { "a", "r", "g", "b" };
		static const char *const kRgb[] = { "r", "g", "b" };
		const size_t channels = argb ? 4 : 3;
		for (size_t i = 0; i < channels && i < values.size(); ++i) {
			HudField field;
			const char *channel = argb ? kArgb[i] : kRgb[i];
			field.id = lower(key) + "." + channel;
			field.key = key;
			field.index = int(i);
			field.kind = HudFieldKind::Number;
			field.least = 0;
			field.most = 255;
			field.words = std::string(key) + " " + channel;
			field.range = std::string("0 to 255, a byte of the packed colour") +
			              (argb ? " (a, r, g, b: the first its alpha)" : " (r, g, b)");
			field.cite = witness_of(key);
			field.value = values[i];
			out.push_back(std::move(field));
		}
	}
	// Its detail level: the HUDDECLUT row's flag at each of the four levels F6 steps.
	if (row->detail[0]) {
		const std::string key = std::string("HUDDECLUT_") + row->detail;
		std::vector<std::string> values;
		def::hudpos_key_values(hud, key, "", values);
		for (size_t level = 0; level < 4 && level < values.size(); ++level) {
			HudField field;
			field.id = lower(key) + ".level" + std::to_string(level);
			field.key = key;
			field.index = int(level);
			field.kind = HudFieldKind::Number;
			field.least = 0;
			field.most = 1;
			field.words = key + " level " + std::to_string(level);
			field.range = "0 or 1: shown at HUD detail level " + std::to_string(level) +
			              " when 1 (level 3 hides the HUD whatever it says) " + row->detail_cite;
			field.cite = "[orig: HUD_ParseHudposToken ORs the level's bit per value read nonzero into byte_2723CE0; "
			             "CRenderState_SetLayerVisibility @ 0x59B0F0]";
			field.value = values[level];
			out.push_back(std::move(field));
		}
	}
	return out;
}

bool hud_drag_start(HudElement element, const def::DefHudPosDef &hud, HudDragStart &out) {
	out = HudDragStart();
	out.element = element;
	for (const HudCoordinate &coordinate : hud_element_coordinates(element)) {
		int value = 0;
		if ((!coordinate.primary && !key_authored(hud, coordinate)) || !hud_coordinate_value(hud, coordinate, value)) continue;
		out.coordinates.push_back(coordinate);
		out.values.push_back(value);
	}
	return !out.coordinates.empty();
}

const char *hud_handle_token(HudHandle handle) {
	switch (handle) {
	case HudHandle::Move: return "move";
	case HudHandle::TopLeft: return "top_left";
	case HudHandle::TopRight: return "top_right";
	case HudHandle::BottomLeft: return "bottom_left";
	case HudHandle::BottomRight: return "bottom_right";
	}
	return "move";
}

bool hud_handle_from_token(const std::string &token, HudHandle &out) {
	for (const HudHandle handle :
	     { HudHandle::Move, HudHandle::TopLeft, HudHandle::TopRight, HudHandle::BottomLeft, HudHandle::BottomRight })
		if (token == hud_handle_token(handle)) {
			out = handle;
			return true;
		}
	return false;
}

bool hud_drag_changes(const HudDragStart &start, HudHandle handle, float dx, float dy, int grid,
		std::vector<HudValueChange> &out, std::string &error) {
	out.clear();
	const size_t count = std::min(start.coordinates.size(), start.values.size());
	auto change = [&](size_t i, int value) {
		const HudCoordinate &coordinate = start.coordinates[i];
		out.push_back({ coordinate.key, coordinate.first, coordinate.index, std::to_string(value) });
	};
	if (handle == HudHandle::Move) {
		// Each axis moves by how far its lead lands from where it began, on the grid.
		int shift[2] = { 0, 0 };
		bool led[2] = { false, false };
		for (size_t i = 0; i < count; ++i) {
			const HudCoordinate &coordinate = start.coordinates[i];
			const int axis = coordinate.axis == X ? 0 : 1;
			if (led[axis] || coordinate.edge == Extent) continue;
			const int from = start.values[i];
			shift[axis] = snapped(double(from) + (axis == 0 ? dx : dy), grid) - from;
			led[axis] = true;
		}
		for (size_t i = 0; i < count; ++i) {
			const HudCoordinate &coordinate = start.coordinates[i];
			if (coordinate.edge == Extent) continue;
			change(i, start.values[i] + shift[coordinate.axis == X ? 0 : 1]);
		}
		return true;
	}
	if (!hud_element_resizable(start.element)) {
		error = std::string("The game reads no size for the ") + opennova::hud::hud_element_token(start.element) +
		        ": it is moved, never resized.";
		return false;
	}
	const bool left = handle == HudHandle::TopLeft || handle == HudHandle::BottomLeft;
	const bool top = handle == HudHandle::TopLeft || handle == HudHandle::TopRight;
	for (int axis = 0; axis < 2; ++axis) {
		const HudAxis which = axis == 0 ? X : Y;
		const bool near_side = axis == 0 ? left : top;
		const float by = axis == 0 ? dx : dy;
		// The axis's near edge, its far edge or its extent, by index.
		int near_at = -1, far_at = -1, extent_at = -1;
		for (size_t i = 0; i < count; ++i) {
			const HudCoordinate &coordinate = start.coordinates[i];
			if (coordinate.axis != which) continue;
			if (coordinate.edge == Near && near_at < 0) near_at = int(i);
			if (coordinate.edge == Far && far_at < 0) far_at = int(i);
			if (coordinate.edge == Extent && extent_at < 0) extent_at = int(i);
		}
		if (near_at < 0 || (far_at < 0 && extent_at < 0)) continue;
		const int near_from = start.values[size_t(near_at)];
		const int far_from = far_at >= 0 ? start.values[size_t(far_at)] : near_from + start.values[size_t(extent_at)];
		if (near_side) {
			const int near_to = std::min(snapped(double(near_from) + by, grid), far_from - 1);
			change(size_t(near_at), near_to);
			if (extent_at >= 0) change(size_t(extent_at), far_from - near_to);
		} else {
			const int far_to = std::max(snapped(double(far_from) + by, grid), near_from + 1);
			if (far_at >= 0) change(size_t(far_at), far_to);
			else change(size_t(extent_at), far_to - near_from);
		}
	}
	return true;
}

bool hud_layout_edits(const TextDocument &text, const def::DefHudPosDef &hud, const std::vector<HudValueChange> &changes,
		uint64_t gesture, std::vector<Edit> &out, std::string &error) {
	out.clear();
	const std::string &source = text.text();
	const std::vector<HudLayoutLine> lines = hud_layout_lines(source);
	// The changes by line, in the order they come, each line's key as its first change spells it.
	std::vector<std::pair<std::string, std::string>> order;
	std::map<std::pair<std::string, std::string>, std::map<int, std::string>> by_line;
	std::map<std::pair<std::string, std::string>, std::string> spelled;
	for (const HudValueChange &change : changes) {
		const auto line = std::make_pair(lower(change.key), change.first);
		if (!by_line.count(line)) {
			order.push_back(line);
			spelled[line] = change.key;
		}
		by_line[line][change.index] = change.value;
	}
	std::vector<Splice> splices;
	std::string added;
	for (const auto &line_key : order) {
		const std::string &key = spelled[line_key];
		const std::string &first = line_key.second;
		std::vector<std::string> values;
		if (!def::hudpos_key_values(hud, key, first, values)) {
			error = "hudpos.def's " + key + (first.empty() ? std::string() : " " + first) +
			        " is not a line the HUD's handles write.";
			return false;
		}
		const std::map<int, std::string> &set = by_line[line_key];
		int last = -1;
		for (const auto &each : set) {
			if (each.first < 0 || size_t(each.first) >= values.size()) {
				error = "hudpos.def's " + key + " has no value " + std::to_string(each.first) + ".";
				return false;
			}
			values[size_t(each.first)] = each.second;
			last = std::max(last, each.first);
		}
		const HudLayoutLine *found = hud_layout_line(lines, key.c_str(), first.empty() ? nullptr : first.c_str());
		if (!found) {
			// A line of its own at the end of the text, as the writer writes it.
			def::HudposLine written;
			written.key = key;
			written.values = values;
			added += def::hudpos_line_text(written) + "\r\n";
			continue;
		}
		const std::string line = source.substr(found->offset, found->length);
		const LineTokens tokens = tokens_of(line);
		for (const auto &each : set) {
			const int token = each.first + 1;
			if (token >= tokens.count) continue;
			const std::string written = line.substr(tokens.begin[token], tokens.end[token] - tokens.begin[token]);
			std::string value = each.second;
			if (tokens.quoted[token]) value = unquoted(value);
			if (reads_as(written, unquoted(each.second))) continue;
			splices.push_back({ found->offset + tokens.begin[token], tokens.end[token] - tokens.begin[token], value });
		}
		// The values the line lacks, up to the last one set, after its last token.
		if (last + 1 >= tokens.count && tokens.count > 0) {
			std::string tail;
			for (int i = tokens.count - 1; i <= last; ++i) tail += "," + values[size_t(i)];
			splices.push_back({ found->offset + tokens.end[tokens.count - 1], 0, tail });
		}
	}
	if (!added.empty()) {
		if (!source.empty() && source.back() != '\n') added = "\r\n" + added;
		splices.push_back({ source.size(), 0, added });
	}
	// Each against the text the ones before it left: from the end of the text back.
	std::stable_sort(splices.begin(), splices.end(), [](const Splice &a, const Splice &b) { return a.offset > b.offset; });
	for (Splice &splice : splices)
		out.push_back(TextDocument::replace(text.span_at(splice.offset, splice.length), std::move(splice.text), false, gesture));
	return true;
}

bool hud_field_change(HudElement element, const def::DefHudPosDef &hud, const std::string &field, const std::string &value,
		HudValueChange &out, std::string &error) {
	const std::vector<HudField> fields = hud_element_fields(element, hud);
	const auto found = std::find_if(fields.begin(), fields.end(), [&](const HudField &each) { return each.id == field; });
	if (found == fields.end()) {
		std::string ids;
		for (const HudField &each : fields) ids += (ids.empty() ? "" : ", ") + each.id;
		error = std::string("The ") + opennova::hud::hud_element_token(element) + " has no field \"" + field + "\"" +
		        (ids.empty() ? std::string(": no line of hudpos.def places it.") : " (it has " + ids + ").");
		return false;
	}
	out = HudValueChange{ found->key, found->first, found->index, value };
	switch (found->kind) {
	case HudFieldKind::Number: {
		int number = 0;
		if (!number_of(value, number) || number < found->least || number > found->most) {
			error = field + " is a whole number, " + found->range + ".";
			return false;
		}
		out.value = std::to_string(number);
		return true;
	}
	case HudFieldKind::Align: {
		const std::string word = lower(value);
		if (word != "left" && word != "right" && word != "center") {
			error = field + " is left, right or center.";
			return false;
		}
		out.value = word;
		return true;
	}
	case HudFieldKind::Name: {
		const bool blank = value.empty() || value.find_first_of("\"\r\n") != std::string::npos;
		if (blank) {
			error = field + " is a file's name, one the tokenizer reads whole (no quote, no line end).";
			return false;
		}
		const bool quoted = value.find_first_of(" ,\t;") != std::string::npos || value.find("//") != std::string::npos;
		out.value = quoted ? "\"" + value + "\"" : value;
		return true;
	}
	}
	return false;
}

} // namespace opennova::editor
