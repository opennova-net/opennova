// Which hudpos.def values place each HUD element (runtime/hud/hud_element_layout.h): the element ->
// key table, its HUDDECLUT rows, the positioned texts' hidden / alignment indices, a coordinate's value
// as the game reads it (the NETWORKINDICATOR reset corners [orig: CNetQuality_Reset @0x4c5908..0x4c591e],
// PAUSEDPOS's fallback [orig: HUD_DrawPausedText @0x59d65c..0x59d670]), and a written value read back
// whole.

#include <runtime/hud/hud_element_layout.h>
#include <runtime/hud/hud_frame.h>

#include <cstdio>
#include <cstring>

#include <formats/def/def.h>

using namespace opennova::def;
using namespace opennova::hud;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

static bool parse(const char *text, DefHudPosFile &file) {
	std::memset(&file, 0, sizeof(file));
	return def_parse_hudpos_memory(reinterpret_cast<const uint8_t *>(text), std::strlen(text), &file) == 0;
}

static int value_of(const DefHudPosDef &hud, const HudCoordinate &coordinate) {
	int out = -12345;
	CHECK(hud_coordinate_value(hud, coordinate, out));
	return out;
}

static void the_table() {
	// A rect's corners: near and far on each axis, so a corner resizes it.
	const std::vector<HudCoordinate> &health = hud_element_coordinates(HudElement::Health);
	CHECK(health.size() == 4);
	CHECK(std::strcmp(health[0].key, "HUDHEALTH") == 0 && health[0].edge == HudEdge::Near && health[0].axis == HudAxis::X);
	CHECK(health[3].index == 3 && health[3].edge == HudEdge::Far && health[3].axis == HudAxis::Y);
	CHECK(hud_element_resizable(HudElement::Health));
	CHECK(std::strcmp(hud_element_detail_row(HudElement::Health), "DMGBAR") == 0);
	// A place and its extents: resizable too.
	CHECK(hud_element_resizable(HudElement::Power));
	CHECK(hud_element_coordinates(HudElement::Power)[2].edge == HudEdge::Extent);
	CHECK(std::strcmp(hud_element_detail_row(HudElement::Power), "PWRBAR") == 0);
	// A point is moved, never resized; the weapon cluster rides WPNGRP.
	CHECK(!hud_element_resizable(HudElement::AmmoCount));
	CHECK(std::strcmp(hud_element_detail_row(HudElement::AmmoCount), "WPNGRP") == 0);
	const HudElementLayout *ammo = hud_element_layout(HudElement::AmmoCount);
	CHECK(ammo && ammo->fonts && ammo->colours.size() == 1 && std::strcmp(ammo->colours[0], "WEAPON_TEXTCOLOR") == 0);
	// The crosshair: no line places it, its XHAIRS row gates it.
	CHECK(hud_element_coordinates(HudElement::Crosshair).empty());
	CHECK(std::strcmp(hud_element_detail_row(HudElement::Crosshair), "XHAIRS") == 0);
	// The weapon slot bar: each HUDLS_SLOT by its slot, none of them primary.
	const std::vector<HudCoordinate> &slots = hud_element_coordinates(HudElement::WeaponSlotBar);
	CHECK(slots.size() == 20);
	CHECK(std::strcmp(slots[0].first, "1") == 0 && std::strcmp(slots[19].first, "10") == 0 && !slots[0].primary);
	CHECK(slots[0].index == 1 && slots[1].index == 2);
	// The corner map's four edges, each its own key.
	CHECK(hud_element_resizable(HudElement::Spinmap));
	CHECK(std::strcmp(hud_element_detail_row(HudElement::Spinmap), "SPINMAP") == 0);
	// An element no row names.
	CHECK(hud_element_layout(HudElement::SightsCard) == nullptr);
	CHECK(hud_element_coordinates(HudElement::SightsCard).empty());
	CHECK(!hud_element_resizable(HudElement::SightsCard));
	CHECK(std::strcmp(hud_element_detail_row(HudElement::SightsCard), "") == 0);
}

static void the_text_keys() {
	const HudTextKey *ammo = hud_text_key("ammocountpos");
	CHECK(ammo && ammo->hidden == 2 && ammo->align == 3);
	const HudTextKey *breath = hud_text_key("BREATHTIME");
	CHECK(breath && breath->hidden == -1 && breath->align == 2);
	CHECK(hud_text_key("HUDHEALTH") == nullptr);
}

static void whole_values() {
	int out = 0;
	CHECK(hud_whole_value("12", out) && out == 12);
	CHECK(hud_whole_value("-4", out) && out == -4);
	CHECK(!hud_whole_value("1.5", out));
	CHECK(!hud_whole_value("3x", out));
	CHECK(!hud_whole_value("left", out));
	CHECK(!hud_whole_value("", out));
}

static void paused_anchor() {
	CHECK((paused_text_pos(0, 9) == std::array<int, 2>{ 1000, 4 }));
	CHECK((paused_text_pos(500, 0) == std::array<int, 2>{ 1000, 4 }));
	CHECK((paused_text_pos(500, 6) == std::array<int, 2>{ 500, 6 }));
}

static void coordinate_values() {
	DefHudPosFile file;
	CHECK(parse("HUDHEALTH\t2,739,141,757\r\n"
	            "PAUSEDPOS\t0,9\r\n"
	            "HUDLS_SLOT\t3,40,50\r\n",
	            file));
	const DefHudPosDef &hud = file.hud;
	const std::vector<HudCoordinate> &health = hud_element_coordinates(HudElement::Health);
	CHECK(value_of(hud, health[0]) == 2 && value_of(hud, health[1]) == 739);
	CHECK(value_of(hud, health[2]) == 141 && value_of(hud, health[3]) == 757);
	CHECK(hud_coordinate_authored(hud, health[0]));
	// HUDHEAT is not in the file: what a file without it reads, unauthored.
	CHECK(!hud_coordinate_authored(hud, hud_element_coordinates(HudElement::Heat)[0]));
	// PAUSEDPOS with a zero field: the drawer's (1000, 4).
	const std::vector<HudCoordinate> &paused = hud_element_coordinates(HudElement::PausedText);
	CHECK(value_of(hud, paused[0]) == 1000 && value_of(hud, paused[1]) == 4);
	// No NETWORKINDICATOR line: the reset corners.
	const std::vector<HudCoordinate> &net = hud_element_coordinates(HudElement::NetQuality);
	CHECK(net.size() == 6);
	for (size_t i = 0; i < net.size(); ++i) CHECK(value_of(hud, net[i]) == kNetIndicatorResetPos[i]);
	// A slot by its first value.
	const std::vector<HudCoordinate> &slots = hud_element_coordinates(HudElement::WeaponSlotBar);
	CHECK(value_of(hud, slots[4]) == 40 && value_of(hud, slots[5]) == 50);
	CHECK(hud_coordinate_authored(hud, slots[4]));
	CHECK(!hud_coordinate_authored(hud, slots[0]));
	def_free_hudpos(&file);

	CHECK(parse("PAUSEDPOS\t500,6\r\nNETWORKINDICATOR\t1,2,3,4,5,6\r\n", file));
	CHECK(value_of(file.hud, paused[0]) == 500 && value_of(file.hud, paused[1]) == 6);
	for (size_t i = 0; i < net.size(); ++i) CHECK(value_of(file.hud, net[i]) == int(i) + 1);
	def_free_hudpos(&file);
}

int main() {
	the_table();
	the_text_keys();
	whole_values();
	paused_anchor();
	coordinate_values();
	if (failures) {
		std::printf("hud_element_layout: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("hud_element_layout: ok\n");
	return 0;
}
