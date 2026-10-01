// The spinmap's corrected and newly ported legs (hud/hud_minimap_view.h and
// its leg TUs), driven through the public compiler: the bit16 squaring and
// width scale, the invisible bit0 mask and the missing-sector tile, the
// pointer geometry and its 34-degree label race, the ring band, the bank walk,
// the blip crop and flash gates, and the tether / FARP / zone / pool-3 /
// waypoint / name / tracked-callout / route legs.
// [orig: HUD_DrawMapOverlay @0x5A5F40 and the callees cited per test]
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include <runtime/hud/hud_minimap.h>

using namespace opennova::hud;

static int failures = 0;
#define CHECK(c, msg)                                                                      \
	do {                                                                                   \
		if (!(c)) {                                                                        \
			std::printf("FAIL %s:%d  %s  (%s)\n", __FILE__, __LINE__, msg, #c);            \
			++failures;                                                                    \
		}                                                                                  \
	} while (0)

namespace {

// The 210 px corner rect at the design surface: centre (915, 657), half-height
// 105, disc R = 105 - scaleX(2) = 103, span scaleY(10) = 10, and the default
// zoom's 65536 / (210 * 200) world units per pixel.
constexpr float kCx = 915.0f;
constexpr float kCy = 657.0f;
constexpr float kR = 103.0f;
const float kScale = 65536.0f / (210.0f * 200.0f);

HudMinimapInput corner() {
	HudMinimapInput in;
	in.rect_x1 = 810.0f;
	in.rect_y1 = 552.0f;
	in.rect_x2 = 1020.0f;
	in.rect_y2 = 762.0f;
	return in;
}

HudMinimapInput big(int mode) {
	HudMinimapInput in = corner();
	in.map_mode = mode;
	return in;
}

HudMinimapMarker live(int32_t x, int32_t y, uint8_t icon, uint32_t color,
		HudMinimapBank bank) {
	HudMinimapMarker m;
	m.bank = static_cast<uint8_t>(bank);
	m.handle = 0x1100;
	m.x = x;
	m.y = y;
	m.icon = icon;
	m.color = color;
	m.entity_known = 1;
	m.entity_bits = kMarkerEntityHasModel;
	m.entity_x = m.anchor_x = x;
	m.entity_y = m.anchor_y = y;
	return m;
}

int count_icon_sprites(const HudMapPass &p) {
	int n = 0;
	for (const HudMapSprite &s : p.sprites)
		if (s.texture == 0) ++n;
	return n;
}

const HudMapLabel *find_label(const HudMapPass &p, const char *text) {
	for (const HudMapLabel &l : p.labels)
		if (std::strcmp(l.text, text) == 0) return &l;
	return nullptr;
}

// A2 — bit16 squares the rect on its half-height about the centre, and the
// scale divides the zoom by the (squared) WIDTH x 200; without bit16 the
// width is the authored one. [orig: @0x5a63bf..0x5a642e; @0x5a6501]
void test_bit16_squaring_and_width_scale() {
	HudMinimapInput in = corner();
	in.rect_x1 = 800.0f; // 220 wide, 210 high
	HudMinimapCompiler compiler;
	const HudMapPass &sq = compiler.compile(in);
	CHECK(sq.clip_x1 == 805.0f && sq.clip_x2 == 1015.0f,
			"bit16 squares the viewport to 210 px about the centre");
	float x = 0.0f, y = 0.0f;
	project_spinmap_point(in, 100 << 16, 0, false, x, y);
	CHECK(std::fabs((kCy - y) - 100.0f / kScale) < 0.01f,
			"the squared width sets the scale");
	HudMinimapInput wide = in;
	wide.flags &= ~0x10000u;
	HudMinimapCompiler compiler2;
	const HudMapPass &raw = compiler2.compile(wide);
	CHECK(raw.clip_x1 == 800.0f && raw.clip_x2 == 1020.0f,
			"without bit16 the rect stays as authored");
	project_spinmap_point(wide, 100 << 16, 0, false, x, y);
	const float wide_scale = 65536.0f / (220.0f * 200.0f);
	CHECK(std::fabs((kCy - y) - 100.0f / wide_scale) < 0.01f,
			"the scale divides by the rect WIDTH, not its height");
	// Mode 3 carries no bit16: the fullscreen map scales by its 1023 px width.
	HudMinimapInput full = big(3);
	project_spinmap_point(full, 0, 100 << 16, false, x, y);
	const float full_scale = static_cast<float>(kBigMapZoomDefault) / (1023.0f * 200.0f);
	CHECK(std::fabs((383.0f - y) - 100.0f / full_scale) < 0.01f,
			"the fullscreen map's scale uses its width");
}

// A1 — no colour reaches the screen from the bit0 disc (no backing geometry);
// a missing sector still draws, every UV on Colormap0's corner texel; bit9
// crops the tiles to the disc, the big map only to its rect.
// [orig: @0x5a6527..0x5a6667; Render_TerrainDecal @0x60744C/@0x6075D9;
//  use_alt_blend @0x6071F7]
void test_mask_and_missing_sector_tiles() {
	HudMinimapInput in = corner();
	in.terrain.present = true;
	in.terrain.sector_count = 16;
	in.terrain.sector_rows = 16;
	in.terrain.sector_grid.fill(0); // every sector missing
	HudMinimapCompiler compiler;
	const HudMapPass &p = compiler.compile(in);
	CHECK(!p.terrain.empty(), "missing sectors still draw their tile");
	bool corner_texel = true;
	float max_r = 0.0f;
	for (const HudMapTri &t : p.terrain) {
		for (const HudMapVertex *v : {&t.a, &t.b, &t.c}) {
			if (std::fabs(v->u - 0.5f / 1024.0f) > 1e-6f ||
					std::fabs(v->v - 0.5f / 1024.0f) > 1e-6f)
				corner_texel = false;
			max_r = std::max(max_r, std::hypot(v->x - kCx, v->y - kCy));
		}
	}
	CHECK(corner_texel, "a missing sector samples Colormap0's corner texel");
	CHECK(max_r <= kR + 0.01f, "bit9 crops the corner tiles to the disc");

	HudMinimapInput full = big(3);
	full.terrain = in.terrain;
	HudMinimapCompiler compiler2;
	const HudMapPass &q = compiler2.compile(full);
	float max_x = 0.0f;
	for (const HudMapTri &t : q.terrain)
		for (const HudMapVertex *v : {&t.a, &t.b, &t.c}) max_x = std::max(max_x, v->x);
	CHECK(std::fabs(max_x - 1023.0f) < 0.01f,
			"the big map's tiles reach the rect edge (no disc crop)");
}

// A3 — the tip clamps at R - span with the chevron at half-size span; inside
// that circle the dot sits AT the target at 0.75 span; the distance label
// draws only when the race (within 34 degrees of screen-up) stored one.
// [orig: HUD_DrawMapTargetPointer @0x5992C5 / @0x599366 / @0x599542..0x599616]
void test_pointer_geometry_and_race() {
	HudMinimapInput in = corner();
	in.waypoint_present = true;
	in.waypoint_x = 800 << 16; // straight ahead = screen up
	HudMinimapCompiler compiler;
	const HudMapPass &out = compiler.compile(in);
	const HudMapSprite *tip = nullptr;
	for (const HudMapSprite &s : out.sprites)
		if (s.layer == 4) tip = &s;
	CHECK(tip != nullptr, "the pointer's tip cell draws");
	if (tip != nullptr) {
		CHECK(std::fabs(tip->half_w - 10.0f) < 1e-4f, "the chevron's half-size is the span");
		CHECK(std::fabs(tip->center_x - kCx) < 0.05f &&
				std::fabs(tip->center_y - (kCy - (kR - 10.0f))) < 0.05f,
				"the chevron clamps at R - span");
	}
	CHECK(out.lines.size() == 1 && std::fabs(out.lines[0].y1 - (kCy - (kR - 10.0f))) < 0.05f,
			"the line ends at the clamped tip");
	const HudMapLabel *label = find_label(out, "800m");
	CHECK(label != nullptr && label->x == kCx && label->y == kCy - 118.0f,
			"the race stores the distance and its R + span + span/2 slot");

	in.waypoint_x = 20 << 16; // inside R - span
	const HudMapPass &near = compiler.compile(in);
	tip = nullptr;
	for (const HudMapSprite &s : near.sprites)
		if (s.layer == 4) tip = &s;
	CHECK(tip != nullptr && std::fabs(tip->half_w - 7.5f) < 1e-4f,
			"the inside dot's half-size is 0.75 span");
	CHECK(tip != nullptr && std::fabs(tip->center_y - (kCy - 20.0f / kScale)) < 0.05f,
			"the inside dot sits at the target");

	in.waypoint_x = -(800 << 16); // behind: 180 degrees from up
	const HudMapPass &behind = compiler.compile(in);
	CHECK(find_label(behind, "800m") == nullptr,
			"no race winner beyond 34 degrees, so no distance label");
	in.waypoint_x = 0;
	in.waypoint_y = 800 << 16; // left: 90 degrees
	CHECK(compiler.compile(in).labels.size() == 1,
			"a sideways waypoint leaves only the MAPCOORDS label");
}

// A5 — rings are Render_DrawRingOverlay's anti-aliased band: 253's main ring
// (R = 4 px -> 16 segments x 6 tris) and its filled 1-px dot (4 x 7), each
// special drawn on three layer passes.
// [orig: Render_DrawRingOverlay @0x5D4270; Render_MinimapSlotBlip 253 arm
//  @0x5be3db..0x5be471]
void test_ring_band() {
	HudMinimapInput in = big(3);
	HudMinimapMarker ring;
	ring.bank = static_cast<uint8_t>(HudMinimapBank::kSpecial);
	ring.flags = 0x40;
	ring.icon = 253;
	ring.color = 0xFF208020u;
	ring.remaining_ticks = 10;
	in.markers.push_back(ring);
	in.ticks = 8; // pulse phase 0: the colour stays
	HudMinimapCompiler compiler;
	const HudMapPass &p = compiler.compile(in);
	int bands = 0;
	int main_rings = 0;
	int dots = 0;
	for (const HudMapSprite &s : p.sprites) {
		if (s.texture != kHudMapTextureNone) continue;
		++bands;
		if (s.geom_count == 16u * 6u * 3u) ++main_rings;
		if (s.geom_count == 4u * 7u * 3u) ++dots;
	}
	CHECK(bands == 6 && main_rings == 3 && dots == 3,
			"two bands per draw, three layer passes");
	bool opaque_ring = false;
	bool clear_edge = false;
	for (const HudMapGeomVertex &v : p.geom) {
		if (v.color == 0xFF208020u) opaque_ring = true;
		if (v.color == 0x00208020u) clear_edge = true;
	}
	CHECK(opaque_ring && clear_edge,
			"the band feathers from the opaque ring to its alpha-0 edges");
}

// A6 — a persistent slot whose entity has no model never draws; a special
// with flag bit 7 draws on layers 1, 2 and 3.
// [orig: MapOverlay_RenderAllByLayer @0x5BE6C4 / @0x5BE780..0x5BE7D7]
void test_bank_walk_gates() {
	HudMinimapInput in = corner();
	HudMinimapMarker m = live(10 << 16, 0, 10, 0xFF304080u, HudMinimapBank::kPersistent);
	m.entity_bits = 0;
	in.markers.push_back(m);
	HudMinimapCompiler compiler;
	CHECK(count_icon_sprites(compiler.compile(in)) == 0,
			"a model-less persistent slot never draws");
	in.markers[0].entity_bits = kMarkerEntityHasModel;
	CHECK(count_icon_sprites(compiler.compile(in)) == 1, "a model-bearing one does");
	in.markers[0].bank = static_cast<uint8_t>(HudMinimapBank::kTransient);
	in.markers[0].entity_bits = 0;
	CHECK(count_icon_sprites(compiler.compile(in)) == 1,
			"the transient bank has no model gate");
	HudMinimapMarker special;
	special.bank = static_cast<uint8_t>(HudMinimapBank::kSpecial);
	special.flags = 0xC0;
	special.icon = 12;
	special.remaining_ticks = 5;
	in.markers = {special};
	CHECK(count_icon_sprites(compiler.compile(in)) == 3,
			"a bit-7 special draws on layers 1, 2 and 3");
}

// A7 — a blip culls only against the rect; the disc crops it per pixel.
// [orig: Minimap_DrawBlip @0x597DA1..0x597DF7]
void test_blip_crop() {
	HudMinimapInput in = corner();
	const int32_t edge = static_cast<int32_t>(kR * kScale * 65536.0f);
	in.markers.push_back(live(edge, 0, 10, 0xFF304080u, HudMinimapBank::kTransient));
	HudMinimapCompiler compiler;
	const HudMapPass &p = compiler.compile(in);
	const HudMapSprite *blip = nullptr;
	for (const HudMapSprite &s : p.sprites)
		if (s.texture == 0) blip = &s;
	CHECK(blip != nullptr && blip->geom_count > 0,
			"a blip straddling the disc edge draws its cropped part");
	bool inside = true;
	if (blip != nullptr) {
		for (uint32_t i = 0; i < blip->geom_count; ++i) {
			const HudMapGeomVertex &v = p.geom[blip->geom_first + i];
			if (std::hypot(v.x - kCx, v.y - kCy) > kR + 0.01f) inside = false;
		}
	}
	CHECK(inside, "the cropped blip stays inside the disc");
	// Inside the rect's corner but wholly outside the disc: culled per pixel.
	const int32_t diag = static_cast<int32_t>(95.0f * kScale * 65536.0f);
	in.markers = {live(diag, diag, 10, 0xFF304080u, HudMinimapBank::kTransient)};
	CHECK(count_icon_sprites(compiler.compile(in)) == 0,
			"a blip in the rect corner outside the disc leaves nothing");
}

// The flash gates: a lit phase hides the table-1..3 colour under timer 12
// and cell 3 under timer 10. [orig: Minimap_DrawBlip @0x597DFD..0x597E92]
void test_blip_flash_gates() {
	HudMinimapInput in = corner();
	in.markers.push_back(live(0, 0, 10, 0xFF204080u, HudMinimapBank::kTransient));
	HudMinimapCompiler compiler;
	in.item_flash[12] = 0x10;
	CHECK(count_icon_sprites(compiler.compile(in)) == 0, "timer 12 lit hides it");
	in.item_flash[12] = 0x20;
	CHECK(count_icon_sprites(compiler.compile(in)) == 1, "its dark phase draws it");
	in.item_flash[12] = 0;
	in.markers[0].icon = 3;
	in.item_flash[10] = 0x30;
	CHECK(count_icon_sprites(compiler.compile(in)) == 0, "timer 10 lit hides cell 3");
}

HudMinimapMarker zone_marker(uint8_t zone_number, uint8_t source, uint8_t team) {
	HudMinimapMarker z = live(0, 0, 0, 0xFF304080u, HudMinimapBank::kTransient);
	z.entity_bits = static_cast<uint8_t>(kMarkerEntityHasModel | kMarkerEntityZoneDef);
	z.zone_number = zone_number;
	z.source = source;
	z.team = team;
	z.def_type = 5;
	z.zone_index = 0;
	return z;
}

// bit2 — an objective zone tethers a line + chevron in its team colour, the
// race distance less its entity+0 radius, and suppresses the waypoint.
// [orig: @0x5a6d33..0x5a6df6; @0x5a7853]
void test_objective_tethers() {
	HudMinimapInput in = corner();
	HudMinimapMarker z = zone_marker(1, 0x80, 1);
	z.entity_x = 500 << 16;
	z.bound_radius_q16 = 100 << 16;
	in.markers.push_back(z);
	in.waypoint_present = true;
	in.waypoint_x = -(300 << 16);
	HudMinimapCompiler compiler;
	const HudMapPass &p = compiler.compile(in);
	CHECK(p.lines.size() == 1 && p.lines[0].color == 0xFF6080FFu,
			"one tether line in the doubled team-1 colour, no waypoint line");
	CHECK(find_label(p, "400m") != nullptr,
			"the tether's race distance subtracts the entity radius");
	in.markers[0].source = 0;
	const HudMapPass &q = compiler.compile(in);
	CHECK(q.lines.size() == 1 && q.lines[0].color != 0xFF6080FFu,
			"without the objective bit the waypoint pointer returns");
}

// bit7 — the nearest FARP: a chevron only (no line), g_HUDColors.active,
// gated by flash timer 14. [orig: @0x5a77f8..0x5a7834]
void test_farp_chevron() {
	HudMinimapInput in = corner();
	in.farp_present = true;
	in.farp_x = 600 << 16;
	in.overlay_color = 0xFF40A040u;
	HudMinimapCompiler compiler;
	const HudMapPass &p = compiler.compile(in);
	int tips = 0;
	for (const HudMapSprite &s : p.sprites)
		if (s.layer == 4 && s.color == 0xFF80FF80u) ++tips;
	CHECK(tips == 1 && p.lines.empty(), "one chevron, no line");
	in.item_flash[14] = 0x20;
	CHECK(compiler.compile(in).sprites.size() == 1, "a dark timer-14 phase hides it (compass remains)");
}

// bit3 — the zone letter, twice at the anchor less (scaleX(8)/2,
// scaleY(16)/2), in the letter palette. [orig: @0x5a732f..0x5a74fa]
void test_zone_letters() {
	HudMinimapInput in = corner();
	HudMinimapMarker z = zone_marker(0, 0, 1);
	z.zone_index = 2;
	in.markers.push_back(z);
	HudMinimapCompiler compiler;
	const HudMapPass &p = compiler.compile(in);
	int letters = 0;
	for (const HudMapLabel &l : p.labels) {
		if (std::strcmp(l.text, "C") != 0) continue;
		++letters;
		CHECK(l.x == kCx - 4.0f && l.y == kCy - 8.0f && l.color == 0xFF80A0FFu && l.clip == 1,
				"the letter sits at the offset anchor in the team-1 letter colour");
	}
	CHECK(letters == 2, "each zone letter draws twice");
	in.markers[0].def_type = 1;
	CHECK(find_label(compiler.compile(in), "C") == nullptr, "a def-type-1 zone has no letter");
}

// bit14 (big map) — numbered zones show their WPNames name wrapped in the
// objective/defensive format, plain names at 0.75, unnumbered their letter.
// [orig: @0x5a6fc2..0x5a7329]
void test_zone_waypoint_labels() {
	HudMinimapOverlays ov;
	ov.zone_wp_names = {"Alpha"};
	ov.objective_point_format = "OBJ %s";
	ov.defensive_position_format = "DEF %s";
	HudMinimapInput in = big(3);
	in.overlays = &ov;
	in.ticks = 8; // phase 0
	in.markers = {zone_marker(1, 0x80, 1)};
	HudMinimapCompiler compiler;
	const HudMapLabel *obj = find_label(compiler.compile(in), "OBJ Alpha");
	CHECK(obj != nullptr && obj->color == 0xFF304080u, "the objective format, unpulsed at phase 0");
	in.markers = {zone_marker(1, 0x40, 2)};
	CHECK(find_label(compiler.compile(in), "DEF Alpha") != nullptr, "the defensive format");
	in.markers = {zone_marker(1, 0, 1)};
	const HudMapLabel *plain = find_label(compiler.compile(in), "Alpha");
	CHECK(plain != nullptr && plain->color == 0xFF243060u, "a plain name at 0.75 brightness");
	in.markers = {zone_marker(0, 0, 1)};
	CHECK(find_label(compiler.compile(in), "A") != nullptr, "an unnumbered zone's letter");
	HudMinimapInput corner_in = corner();
	corner_in.overlays = &ov;
	corner_in.markers = {zone_marker(1, 0x80, 1)};
	CHECK(find_label(compiler.compile(corner_in), "OBJ Alpha") == nullptr,
			"the corner mask carries no bit14");
}

// The pool-3 walk — the 2044 location label (bit11), the 6027 capture ring,
// and the 6006 KOTH ring (bit4) over the score delta.
// [orig: @0x5a7504..0x5a76f5; Minimap_DrawKothZoneRing @0x5974E0]
void test_pool3_walk() {
	HudMinimapOverlays ov;
	ov.location_names = {"Hill 60"};
	HudMinimapPoolEntity loc;
	loc.def_id = 2044;
	HudMinimapPoolEntity capture;
	capture.def_id = 6027;
	capture.x = 30 << 16;
	ov.pool3 = {loc, capture};
	HudMinimapInput in = big(3);
	in.overlays = &ov;
	HudMinimapCompiler compiler;
	const HudMapPass &p = compiler.compile(in);
	const HudMapLabel *label = find_label(p, "Hill 60");
	CHECK(label != nullptr && label->font == 0 && label->clip == 1,
			"the location label, bold, cropped to the viewport");
	bool red_ring = false;
	bool red_fill = false;
	for (const HudMapSprite &s : p.sprites)
		if (s.texture == kHudMapTextureNone && s.color == 0xFFFF2020u) red_ring = true;
	for (const HudMapGeomVertex &v : p.geom)
		if (v.color == 0x40FF2020u) red_fill = true;
	CHECK(red_ring && red_fill, "the 6027 ring with its 0x40 fill");

	HudMinimapPoolEntity koth;
	koth.def_id = 6006;
	koth.radius_q16 = 20 << 16;
	ov.pool3 = {koth};
	ov.zone_score_delta = 3;
	HudMinimapInput corner_in = corner();
	corner_in.overlays = &ov;
	bool blue = false;
	for (const HudMapSprite &s : compiler.compile(corner_in).sprites)
		if (s.texture == kHudMapTextureNone && s.color == 0xFF4060FFu) blue = true;
	CHECK(blue, "a leading KOTH delta rings blue");
}

// bit17 (big map) — player waypoint names, LEFT-aligned.
// [orig: @0x5a76fe..0x5a77ac]
void test_player_waypoints() {
	HudMinimapOverlays ov;
	HudMinimapPlayerWaypoint w;
	w.name = "WP1";
	ov.player_waypoints = {w};
	HudMinimapInput in = big(3);
	in.overlays = &ov;
	HudMinimapCompiler compiler;
	const HudMapLabel *label = find_label(compiler.compile(in), "WP1");
	CHECK(label != nullptr && label->align == 2, "the player waypoint label is left-aligned");
	in.big_map_params.player_waypoints = false;
	CHECK(find_label(compiler.compile(in), "WP1") == nullptr, "params+6 == 0 clears bit17");
}

// bit5 — loop 1 draws a slot's blip (or the medic cross); loop 2 (bit13,
// the big map) names the same-team transient persons in the regular face.
// [orig: HUD_DrawEntityLabelsAndMarkers @0x5A49E0]
void test_entity_labels_and_markers() {
	HudMinimapOverlays ov;
	ov.hud_present = true;
	ov.hud_team = 1;
	ov.hud_handle = 0x0001;
	HudMinimapPlayerSlot slot;
	slot.active = true;
	slot.name_slot = true;
	slot.name = "Kilo";
	slot.clan = "RR";
	slot.blip = live(0, 0, 3, 0xFF304080u, HudMinimapBank::kTransient);
	slot.blip.team = 1;
	slot.blip.def_type = 3;
	ov.player_slots = {slot};
	HudMinimapInput in = corner();
	in.overlays = &ov;
	HudMinimapCompiler compiler;
	const HudMapPass &p = compiler.compile(in);
	CHECK(count_icon_sprites(p) == 1, "loop 1 draws the slot's cell-3 blip");
	CHECK(find_label(p, "Kilo<ch>RR<co>") == nullptr, "no names without bit13");
	ov.player_slots[0].medic = true;
	const HudMapPass &medic = compiler.compile(in);
	CHECK(count_icon_sprites(medic) == 0 && medic.overlays.size() == 6,
			"a same-team medic draws the cross instead");

	ov.player_slots.clear();
	HudMinimapMarker person = live(10 << 16, 0, 3, 0xFF304080u, HudMinimapBank::kTransient);
	person.handle = 0x0002;
	person.def_type = 3;
	person.team = 1;
	HudMinimapName name;
	name.handle = 0x0002;
	name.text = "^SGT  Brown";
	ov.names = {name};
	HudMinimapInput full = big(3);
	full.overlays = &ov;
	full.markers = {person};
	const HudMapLabel *label = find_label(compiler.compile(full), "^SGT  Brown");
	CHECK(label != nullptr && label->font == 2, "loop 2 names the person in the regular face");
	full.markers[0].team = 2;
	CHECK(find_label(compiler.compile(full), "^SGT  Brown") == nullptr,
			"another team's person is not named");
}

// bit19 — the tracked callout: outside the R - span circle a half-alpha line
// + chevron and its own race; the label keeps its alpha; inside, an enemy's
// cell-28 blip at the snapshot. [orig: Render_LaserSightEffect @0x59D110;
// @0x5a7aec..0x5a7b8d]
void test_tracked_callout() {
	HudMinimapOverlays ov;
	ov.tracked.ticks = 186; // alpha 255
	ov.tracked.serial = 1;
	ov.tracked.snap_x = 500 << 16;
	HudMinimapInput in = corner();
	in.overlays = &ov;
	HudMinimapCompiler compiler;
	const HudMapPass &p = compiler.compile(in);
	CHECK(p.lines.size() == 1 && (p.lines[0].color >> 24) == 0x7Fu,
			"the outside line rides half the alpha");
	const HudMapLabel *label = find_label(p, "500m");
	CHECK(label != nullptr && label->keep_alpha == 1 && (label->color >> 24) == 0x7Fu,
			"the label keeps the halved alpha");
	ov.tracked.snap_x = 10 << 16;
	ov.tracked.blip = live(10 << 16, 0, 28, 0, HudMinimapBank::kTransient);
	ov.tracked.blip.def_type = 3;
	const HudMapPass &q = compiler.compile(in);
	CHECK(count_icon_sprites(q) == 1, "an enemy inside draws the cell-28 blip");
	ov.tracked.friendly = true;
	CHECK(count_icon_sprites(compiler.compile(in)) == 0,
			"a friendly one is left to the bit5 walk");
}

// The big map's parameter bytes: labels off clears bits 11/14/17, glow off
// drops the bit12 grid. [orig: HUD_BuildMapOverlayView @0x5a7e8a..0x5a7eb2]
void test_big_map_params() {
	HudMinimapOverlays ov;
	ov.location_names = {"Hill 60"};
	HudMinimapPoolEntity loc;
	loc.def_id = 2044;
	ov.pool3 = {loc};
	HudMinimapInput in = big(3);
	in.overlays = &ov;
	HudMinimapCompiler compiler;
	CHECK(find_label(compiler.compile(in), "Hill 60") != nullptr, "the default block labels");
	CHECK(!compiler.compile(in).lines_under.empty(), "the default block rules the grid");
	in.big_map_params.labels = false;
	CHECK(find_label(compiler.compile(in), "Hill 60") == nullptr, "params+7 == 0 clears bit11");
	in.big_map_params.glow = false;
	CHECK(compiler.compile(in).lines_under.empty(), "no glow byte, no bit12 grid");
}

// bit15 (g_GameType 0x10010) — adjacent route numbers link their entities
// with a two-colour line, each pair twice. [orig: @0x5a6dfc..0x5a6fbc]
void test_route_lines() {
	HudMinimapOverlays ov;
	ov.game_type = 0x10010;
	HudMinimapInput in = big(3);
	in.overlays = &ov;
	HudMinimapMarker a = live(0, 0, 0, 0xFF112233u, HudMinimapBank::kTransient);
	a.source = 1;
	HudMinimapMarker b = live(40 << 16, 0, 0, 0xFF445566u, HudMinimapBank::kTransient);
	b.handle = 0x1101;
	b.source = 2;
	HudMinimapMarker c = live(0, 40 << 16, 0, 0xFF778899u, HudMinimapBank::kTransient);
	c.handle = 0x1102;
	c.source = 5;
	in.markers = {a, b, c};
	HudMinimapCompiler compiler;
	const HudMapPass &p = compiler.compile(in);
	int routes = 0;
	for (const HudMapLine &l : p.lines)
		if (l.color_end != 0) ++routes;
	CHECK(routes == 2, "the one adjacent pair draws twice");
	ov.game_type = 0;
	int none = 0;
	for (const HudMapLine &l : compiler.compile(in).lines)
		if (l.color_end != 0) ++none;
	CHECK(none == 0, "other game types draw no routes");
}

} // namespace

int main() {
	test_bit16_squaring_and_width_scale();
	test_mask_and_missing_sector_tiles();
	test_pointer_geometry_and_race();
	test_ring_band();
	test_bank_walk_gates();
	test_blip_crop();
	test_blip_flash_gates();
	test_objective_tethers();
	test_farp_chevron();
	test_zone_letters();
	test_zone_waypoint_labels();
	test_pool3_walk();
	test_player_waypoints();
	test_entity_labels_and_markers();
	test_tracked_callout();
	test_big_map_params();
	test_route_lines();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("hud_minimap_legs: all passed\n");
	return 0;
}
