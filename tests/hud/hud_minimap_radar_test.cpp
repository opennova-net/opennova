// The spinmap's content-mask bit-10 legs: the dmgslice sector marks (count,
// order, colours, the tick & 8 blink, the NoTracers and texture gates, the
// rotated 0.05..0.95 window) and the threat ring (288 strip triangles, the
// radii, the per-state vertex colours, the missile/red-12 states), plus the
// draw split ahead of the compass and the radar feed codec.
// [orig: HUD_DrawWeaponDirectionIndicators @0x59c350;
//  HUD_DrawTimerOverlayBox @0x59c7b0 -> HUD_DrawDirectionalIndicatorRing
//  @0x598180; HUD_DrawMapOverlay @0x5a78ff..0x5a7931]
#include <runtime/hud/hud_minimap.h>
#include <runtime/hud/hud_minimap_feed.h>

#include <base/io/bam.h>

#include <cmath>
#include <cstdio>
#include <vector>

using namespace opennova::hud;

static int g_failures = 0;
#define CHECK(cond, msg)                                                       \
	do {                                                                       \
		if (!(cond)) {                                                         \
			std::printf("FAIL line %d: %s\n", __LINE__, msg);                  \
			++g_failures;                                                      \
		}                                                                      \
	} while (0)

namespace {

// A 1024x768 surface keeps Viewport_ScaleToVirtualCoords the identity: the
// rect is 200 px square around (900, 650).
HudMinimapInput radar_input() {
	HudMinimapInput input;
	input.rect_x1 = 800.0f;
	input.rect_y1 = 550.0f;
	input.rect_x2 = 1000.0f;
	input.rect_y2 = 750.0f;
	input.surface_w = 1024.0f;
	input.surface_h = 768.0f;
	input.radar_slices_loaded = true;
	input.ticks = 8; // bit 3 set: the lit phase of the blink
	return input;
}

std::vector<const HudMapSprite *> marks(const HudMapPass &pass) {
	std::vector<const HudMapSprite *> out;
	for (const HudMapSprite &sprite : pass.sprites)
		if (sprite.texture == kHudMapSpriteRadar || sprite.texture == kHudMapSpriteRadarNarrow)
			out.push_back(&sprite);
	return out;
}

float mark_rotation(uint32_t acc) {
	return static_cast<float>(-(static_cast<double>(acc >> 16) *
			static_cast<double>(9.58738019107841e-05f)));
}

bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

void test_marks_order_colours_and_geometry() {
	HudMinimapInput input = radar_input();
	input.radar.red12[2] = 1;
	input.radar.olive12[5] = 1;
	input.radar.red12[5] = 0;
	input.radar.red24[4] = 1;
	input.radar.olive24[10] = 1;
	input.radar.olive24[4] = 1; // red wins the colour when both hold a sector
	HudMinimapCompiler compiler;
	const HudMapPass &pass = compiler.compile(input);
	const auto m = marks(pass);
	CHECK(m.size() == 4, "one mark per lit sector: two 12-ring, two 24-ring");
	if (m.size() != 4) return;
	// 12-ring first, then the 24-ring, each in sector order.
	CHECK(m[0]->texture == kHudMapSpriteRadar && m[1]->texture == kHudMapSpriteRadar,
			"the 12-ring rides dmgslice.tga");
	CHECK(m[2]->texture == kHudMapSpriteRadarNarrow && m[3]->texture == kHudMapSpriteRadarNarrow,
			"the 24-ring rides dmgslc_n.tga");
	// Hit 0xFFFF2020 / miss 0xFF808020 as the raw diffuse: the device runs
	// the material's MODULATE2X(TEXTURE, DIFFUSE) stage on these sprites.
	CHECK(m[0]->color == 0xFFFF2020u, "a red sector draws the hit diffuse");
	CHECK(m[1]->color == 0xFF808020u, "an olive-only sector draws the miss diffuse");
	CHECK(m[2]->color == 0xFFFF2020u, "red beats olive on a shared 24-sector");
	CHECK(m[3]->color == 0xFF808020u, "the olive 24-sector draws the miss diffuse");
	for (const HudMapSprite *s : m)
		CHECK(s->modulate2x, "every mark asks the device for the MODULATE2X stage");
	// Float centre, base = the half-height under bit 16, quad = base x 1.25.
	for (const HudMapSprite *s : m) {
		CHECK(near(s->center_x, 900.0f) && near(s->center_y, 650.0f), "the marks centre on the rect");
		CHECK(near(s->half_w, 125.0f) && near(s->half_h, 125.0f), "the quad spans the base x1.25");
		CHECK(near(s->u0, 0.05f) && near(s->v0, 0.05f) && near(s->u1, 0.95f) &&
				near(s->v1, 0.95f), "the centred 90% texture window");
		CHECK(s->layer == 5, "the marks ride the compass layer");
	}
	// The angle accumulators: 12-ring from 0xD5555580 in 0x15555540 steps,
	// 24-ring from 0xCAAAAAE0 in 0x0AAAAAA0 steps; the canvas turns by -theta.
	CHECK(near(m[0]->rotation_rad, mark_rotation(0xD5555580u + 2u * 0x15555540u)),
			"12-ring sector 2 turns by its accumulator");
	CHECK(near(m[0]->rotation_rad, 0.0f), "12-ring sector 2 (dead ahead) is unrotated");
	CHECK(near(m[1]->rotation_rad, mark_rotation(0xD5555580u + 5u * 0x15555540u)),
			"12-ring sector 5 turns by its accumulator");
	CHECK(near(m[2]->rotation_rad, mark_rotation(0xCAAAAAE0u + 4u * 0x0AAAAAA0u)),
			"24-ring sector 4 turns by its accumulator");
	CHECK(near(m[3]->rotation_rad, mark_rotation(0xCAAAAAE0u + 10u * 0x0AAAAAA0u)),
			"24-ring sector 10 turns by its accumulator");
}

void test_mark_gates() {
	HudMinimapInput input = radar_input();
	input.radar.red12[0] = 1;
	input.radar.olive24[23] = 1;
	HudMinimapCompiler compiler;
	CHECK(marks(compiler.compile(input)).size() == 2, "both lit sectors draw in the lit phase");
	input.ticks = 7; // bit 3 clear
	CHECK(marks(compiler.compile(input)).empty(), "the blink's dark phase draws no mark");
	input.ticks = 8 + 16;
	input.radar.rules_no_tracers = true;
	CHECK(marks(compiler.compile(input)).empty(), "NoTracers suppresses the marks");
	input.radar.rules_no_tracers = false;
	input.radar_slices_loaded = false;
	CHECK(marks(compiler.compile(input)).empty(), "no slice textures, no marks");
	input.radar_slices_loaded = true;
	input.flags &= ~0x400u;
	const HudMapPass &no_bit10 = compiler.compile(input);
	CHECK(marks(no_bit10).empty() && no_bit10.ring_tris.empty(), "bit 10 clear skips both legs");
	input.flags |= 0x400u;
	input.flags &= ~0x40u;
	const HudMapPass &no_bit6 = compiler.compile(input);
	CHECK(marks(no_bit6).empty() && no_bit6.ring_tris.empty(),
			"the legs share the compass gate (bits 9 and 6)");
	input.flags |= 0x40u;
	input.map_mode = 2;
	CHECK(marks(compiler.compile(input)).empty(), "the big-map mask carries no bit 10");
}

// The ring vertex for segment k, band e, strip vertex v (0..9).
const HudMapColorVertex &ring_vertex(const HudMapPass &pass, int k, int e, int tri) {
	return pass.ring_tris[static_cast<size_t>((k * 3 + e) * 8 + tri)].a;
}

void test_threat_ring_geometry_and_states() {
	HudMinimapInput input = radar_input();
	HudMinimapCompiler compiler;
	const HudMapPass &idle = compiler.compile(input);
	CHECK(idle.ring_tris.size() == 12u * 3u * 8u, "12 segments x 3 bands x one 10-vertex strip");
	// The compass follows the ring: the split sits right at it.
	CHECK(idle.ring_tris_before_sprite < idle.sprites.size() &&
			idle.sprites[idle.ring_tris_before_sprite].texture == 1,
			"the ring draws right before the compass sprite");
	CHECK(idle.ring_tris_before_sprite < idle.sprites.size() &&
			!idle.sprites[idle.ring_tris_before_sprite].modulate2x,
			"the compass keeps its compile-side fold (a white diffuse)");
	// Radii at 1024 wide: x' = cx -/+ 100 under bit 16, rho0 = 100 - 4 = 96,
	// R = 96 - 2 = 94 -> bands {93..94, 94..96, 96..97}.
	const double c0 = opennova::io::bam_table_cos(opennova::io::bam_table_index(0x0ACAA800u));
	const double s0 = opennova::io::bam_table_sin(opennova::io::bam_table_index(0x0ACAA800u));
	const HudMapColorVertex &inner = ring_vertex(idle, 0, 0, 0);
	CHECK(near(inner.x, static_cast<float>(900.0 + 93 * c0)) &&
			near(inner.y, static_cast<float>(650.0 - 93 * s0)),
			"segment 0 band 0 starts on the R - 1 radius, +Y up");
	const HudMapColorVertex &outer = idle.ring_tris[0].b;
	CHECK(near(outer.x, static_cast<float>(900.0 + 94 * c0)), "band 0's outer edge is R");
	CHECK(inner.color == 0x000F0F0Fu && outer.color == 0x807F7F7Fu,
			"state 0 fades in from transparent to the grey band");
	CHECK(ring_vertex(idle, 0, 1, 0).color == 0x807F7F7Fu, "the solid band is grey at rest");
	const HudMapColorVertex &band2_outer = idle.ring_tris[2 * 8].b;
	CHECK(near(band2_outer.x, static_cast<float>(900.0 + 97 * c0)), "band 2 ends at R + s2 + 1");
	CHECK(band2_outer.color == 0x000F0F0Fu, "band 2 fades out to transparent");

	// A missile due east of the local player at yaw 0 lights segment 2; the
	// state-2 band pushes its edges out by 4 and turns red.
	input.radar.local_x = 100 << 16;
	input.radar.local_y = 50 << 16;
	input.radar.threats.push_back({1, 400 << 16, 50 << 16});
	input.radar.threats.push_back({0, 0, 0}); // a null row draws nothing
	const HudMapPass &hot = compiler.compile(input);
	CHECK(ring_vertex(hot, 2, 1, 0).color == 0xFFFF4040u, "the missile's segment is red");
	CHECK(ring_vertex(hot, 2, 0, 0).color == 0x00800F0Fu, "the red state's faded edge");
	CHECK(ring_vertex(hot, 0, 1, 0).color == 0x807F7F7Fu, "other segments stay grey");
	const double c2 = opennova::io::bam_table_cos(
			opennova::io::bam_table_index(0x0ACAA800u + 2u * 0x15555000u));
	CHECK(near(ring_vertex(hot, 2, 0, 0).x, static_cast<float>(900.0 + 89 * c2)),
			"state 2 pulls the inner edge in by 2 x state");
	CHECK(near(hot.ring_tris[static_cast<size_t>((2 * 3 + 2) * 8)].b.x,
			static_cast<float>(900.0 + 101 * c2)), "and pushes the outer edge out");

	// The red-12 damage sectors blink the ring red too, unless NoTracers.
	HudMinimapInput dmg = radar_input();
	dmg.radar.red12[7] = 1;
	CHECK(ring_vertex(compiler.compile(dmg), 7, 1, 0).color == 0xFFFF4040u,
			"a red-12 sector turns its ring segment red in the lit phase");
	dmg.ticks = 0;
	CHECK(ring_vertex(compiler.compile(dmg), 7, 1, 0).color == 0x807F7F7Fu,
			"and back to grey in the dark phase");
	dmg.ticks = 8;
	dmg.radar.rules_no_tracers = true;
	CHECK(ring_vertex(compiler.compile(dmg), 7, 1, 0).color == 0x807F7F7Fu,
			"NoTracers drops the damage sectors from the ring");
	dmg.radar.threats.push_back({1, 0, 1 << 16}); // north of (0,0) at yaw 0
	CHECK(ring_vertex(compiler.compile(dmg), 5, 1, 0).color == 0xFFFF4040u,
			"the missiles have no rules gate");
}

void test_bearing_sectors() {
	// Viewer at the origin, yaw 0: east 2, north 5, west 8, south 11.
	const auto sector = [](int32_t x, int32_t y, uint32_t yaw, int n) {
		return radar_sector(radar_bearing_raw(x, -y, -kRadarBearingScale), yaw, n);
	};
	CHECK(sector(10 << 16, 0, 0, 12) == 2, "east is sector 2");
	CHECK(sector(0, 10 << 16, 0, 12) == 5, "north is sector 5");
	CHECK(sector(-(10 << 16), 0, 0, 12) == 8, "west is sector 8");
	CHECK(sector(0, -(10 << 16), 0, 12) == 11, "south is sector 11");
	CHECK(sector(0, 10 << 16, 0x40000000u, 12) == 2, "a north-facing viewer puts north dead ahead");
	CHECK(sector(10 << 16, 0, 0, 24) == 4, "east is 24-sector 4");
	CHECK(sector(0, 10 << 16, 0, 24) == 10, "north is 24-sector 10");
}

void test_feed_roundtrip() {
	HudMinimapRadar radar;
	radar.red12[3] = 1;
	radar.olive12[11] = 1;
	radar.red24[23] = 1;
	radar.olive24[0] = 1;
	radar.rules_no_tracers = true;
	radar.local_x = -(5 << 16);
	radar.local_y = 7 << 16;
	radar.threats.push_back({0, 0, 0});
	radar.threats.push_back({1, 123, -456});
	std::vector<int32_t> feed;
	radar_feed_encode(radar, feed);
	CHECK(feed.size() == static_cast<size_t>(kRadarFeedHeaderSize + 2 * kRadarFeedThreatStride),
			"header + two threat rows");
	HudMinimapRadar back;
	CHECK(radar_feed_decode(feed.data(), feed.size(), back), "the feed decodes");
	CHECK(back.red12 == radar.red12 && back.olive12 == radar.olive12 &&
			back.red24 == radar.red24 && back.olive24 == radar.olive24, "the sectors survive");
	CHECK(back.rules_no_tracers && back.local_x == radar.local_x && back.local_y == radar.local_y,
			"the scalars survive");
	CHECK(back.threats.size() == 2 && back.threats[0].present == 0 &&
			back.threats[1].present == 1 && back.threats[1].x == 123 && back.threats[1].y == -456,
			"the threat rows survive, the null row included");
	feed[0] = 99;
	CHECK(!radar_feed_decode(feed.data(), feed.size(), back) && back.threats.empty(),
			"a foreign version decodes to nothing");
}

} // namespace

int main() {
	test_marks_order_colours_and_geometry();
	test_mark_gates();
	test_threat_ring_geometry_and_states();
	test_bearing_sectors();
	test_feed_roundtrip();
	if (g_failures != 0) {
		std::printf("hud_minimap_radar: %d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("hud_minimap_radar: OK\n");
	return 0;
}
