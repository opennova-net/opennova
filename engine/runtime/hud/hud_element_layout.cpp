#include <runtime/hud/hud_element_layout.h>

#include <array>
#include <cmath>
#include <cstdlib>
#include <initializer_list>

#include <base/io/crt_ftol.h>
#include <base/io/strutil.h>
#include <formats/def/def_hudpos_write.h>
#include <runtime/hud/hud_frame.h>

namespace opennova::hud {

namespace {

constexpr HudAxis X = HudAxis::X;
constexpr HudAxis Y = HudAxis::Y;
constexpr HudEdge Point = HudEdge::Point;
constexpr HudEdge Near = HudEdge::Near;
constexpr HudEdge Far = HudEdge::Far;
constexpr HudEdge Extent = HudEdge::Extent;

// The keys whose third value hides what they place and whose fourth is its alignment (the positioned
// texts), and those whose third is the alignment (no hidden value).
constexpr HudTextKey kTextKeys[] = {
	{ "AMMOCOUNTPOS", 2, 3, "[orig: HUD_DrawWeaponAmmoAndName @ 0x5939D0, its gate on the hidden value @ 0x5939F3]" },
	{ "HUDWEAPONNAME", 2, 3, "[orig: HUD_DrawWeaponAmmoAndName @ 0x5939D0, its gate on the hidden value]" },
	{ "GAMEINFO", 2, 3, "[orig: HUD_DrawGameTimerOverlay @ 0x59CC80, its gate @ 0x59CC94 / @ 0x59CCD5]" },
	{ "HUDPLAYERCOUNT", 2, 3, "[orig: HUD_DrawScoreOverlay @ 0x593E50, `cmp dword_272366C, 0` @ 0x593E64]" },
	{ "HUDTEAMXY", 2, 3, "[orig: HUD_DrawTeamIdLine @ 0x59AA30, `cmp dword_2723834, 0` @ 0x59AA44]" },
	{ "HUDWPDINFO", 2, 3, "[orig: HUD_DrawWaypointNameAndDistance @ 0x5947A0; HUD_ParseHudposToken @ 0x5A02C3]" },
	{ "BREATHTIME", -1, 2, "" },
	{ "ZONEINFO", -1, 2, "" },
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

const std::vector<HudElementLayout> &element_rows() {
	static const std::vector<HudElementLayout> rows = {
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

} // namespace

const HudElementLayout *hud_element_layout(HudElement element) {
	for (const HudElementLayout &row : element_rows())
		if (row.element == element) return &row;
	return nullptr;
}

const std::vector<HudCoordinate> &hud_element_coordinates(HudElement element) {
	static const std::vector<HudCoordinate> none;
	const HudElementLayout *row = hud_element_layout(element);
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
	const HudElementLayout *row = hud_element_layout(element);
	return row ? row->detail : "";
}

const HudTextKey *hud_text_key(const std::string &key) {
	for (const HudTextKey &row : kTextKeys)
		if (strutil::iequals(key, row.key)) return &row;
	return nullptr;
}

bool hud_whole_value(const std::string &text, int &out) {
	if (text.empty()) return false;
	char *end = nullptr;
	const double value = std::strtod(text.c_str(), &end);
	if (!end || *end != '\0' || !std::isfinite(value) || value != std::floor(value)) return false;
	out = io::retail_ftol_sse2(value);
	return true;
}

bool hud_coordinate_authored(const def::DefHudPosDef &hud, const HudCoordinate &coordinate) {
	std::vector<std::string> values, none;
	if (!def::hudpos_key_values(hud, coordinate.key, coordinate.first, values)) return false;
	def::hudpos_key_values(def::hudpos_unauthored(), coordinate.key, coordinate.first, none);
	return values != none;
}

bool hud_coordinate_value(const def::DefHudPosDef &hud, const HudCoordinate &coordinate, int &out) {
	if (strutil::iequals(coordinate.key, "NETWORKINDICATOR") && !hud.network_indicator_present) {
		if (coordinate.index >= kNetIndicatorResetPos.size()) return false;
		out = kNetIndicatorResetPos[coordinate.index];
		return true;
	}
	if (strutil::iequals(coordinate.key, "PAUSEDPOS") && coordinate.index < 2) {
		// The pause word's anchor as its drawer takes it: the authored pair, else the drawer's own.
		out = paused_text_pos(hud.paused_pos[0], hud.paused_pos[1])[coordinate.index];
		return true;
	}
	std::vector<std::string> values;
	if (!def::hudpos_key_values(hud, coordinate.key, coordinate.first, values) || coordinate.index >= values.size())
		return false;
	return hud_whole_value(values[coordinate.index], out);
}

} // namespace opennova::hud
