// The death-piece draw (world/death_piece_draw.h): the collect's box, depth
// and viewport gates over the pool, the level walk over the piece model's
// RLOD table, and the section draw's pivot, collapse mask and water flag.
// [orig: DeathPiece_CollectVisible @ 0x57b560, DeathPiece_RenderVisible
//  @ 0x57b830, DeathPiece_RenderSection @ 0x57b690]
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

#include <runtime/renderer/object_lod.h>
#include <runtime/world/death_piece_draw.h>
#include <runtime/world/occlusion.h>
#include <runtime/world/occlusion_camera.h>
#include <runtime/world/world.h>

using namespace opennova::world;
namespace r = opennova::renderer;

static int failures = 0;

#define CHECK(c)                                                                  \
	do {                                                                          \
		if (!(c)) {                                                               \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);              \
			++failures;                                                           \
		}                                                                         \
	} while (0)

namespace {

int32_t q16(double v) { return static_cast<int32_t>(v * 65536.0); }

// The detail factor is the quality term alone: 3 * 0.33 + 0.34 on the
// shipped profile, as a float [orig: @ 0x57b831..0x57b84a].
void test_lod_scale() {
	CHECK(r::death_piece_lod_scale(3) ==
			static_cast<float>(3.0 * double(0.33f) + double(0.34f)));
	CHECK(r::death_piece_lod_scale(0) == 0.34f);
}

// The level chain [orig: @ 0x57b86f..0x57b8ca].
void test_lod_level_chain() {
	const std::vector<int32_t> four = {200 << 16, 60 << 16, 20 << 16, 0};
	// The scaled, truncated radius at or under 0.75 px draws nothing.
	CHECK(r::death_piece_lod_level(0xC000, 1.0f, four) == -1);
	CHECK(r::death_piece_lod_level(0xC001, 1.0f, four) == 3);
	CHECK(r::death_piece_lod_level(0xC001, 0.5f, four) == -1);
	CHECK(r::death_piece_lod_level((200 << 16) + 1, 1.0f, four) == 0);
	CHECK(r::death_piece_lod_level(200 << 16, 1.0f, four) == 1);
	CHECK(r::death_piece_lod_level(60 << 16, 1.0f, four) == 2);
	CHECK(r::death_piece_lod_level(20 << 16, 1.0f, four) == 3);
	// The scale multiplies before the walk.
	CHECK(r::death_piece_lod_level(100 << 16, 2.5f, four) == 0);
	// A one-level model is always level 0.
	CHECK(r::death_piece_lod_level(1 << 16, 1.0f, {200 << 16}) == 0);
	// Two levels: under both thresholds the chain lands on 3 and clamps to 1.
	CHECK(r::death_piece_lod_level(1 << 16, 1.0f, {200 << 16, 60 << 16}) == 1);
	// Three levels under a nonzero third threshold: level 3 == the count
	// stays (the empty slot the section draw skips).
	CHECK(r::death_piece_lod_level(1 << 16, 1.0f, {200 << 16, 60 << 16, 20 << 16}) == 3);
	CHECK(r::death_piece_lod_level(30 << 16, 1.0f, {200 << 16, 60 << 16, 20 << 16}) == 2);
}

// The drawn section is the first clear bit below the level's section count
// [orig: @ 0x57b6c5..0x57b6f4].
void test_render_section() {
	CHECK(death_piece_render_section(0xDu, 4) == 1);
	CHECK(death_piece_render_section(0xBu, 4) == 2);
	CHECK(death_piece_render_section(0xFu, 4) == 0);
	CHECK(death_piece_render_section(0x0u, 0) == 0);
	// The mask bit is the index & 31 (shl).
	CHECK(death_piece_render_section(0xFFFFFFFFu, 40) == 0);
}

DeathPieceModel three_section_model() {
	DeathPieceModel model;
	model.lod_threshold_q16 = {200 << 16, 60 << 16, 20 << 16};
	model.lod_section_count = {3, 3, 2};
	model.section_origin_q16 = {{0, 0, 0}, {q16(1.0), q16(2.0), q16(3.0)},
			{q16(-1.5), q16(0.5), q16(0.25)}};
	model.radius_q16 = q16(4.0);
	return model;
}

DeathPiece section_piece(int section) {
	DeathPiece piece;
	piece.active = true;
	piece.item_id = 900;
	piece.section = static_cast<uint8_t>(section);
	piece.hidden_mask = 0x7u & ~(1u << section);
	piece.pos = Vec3{10.0f, 20.0f, 5.0f};
	piece.heading = 45.0f;
	piece.pitch = -10.0f;
	piece.roll = 5.0f;
	piece.render_scale = 1.3f;
	piece.radius_q16 = q16(4.0);
	return piece;
}

// One piece through the level walk and the section draw
// [orig: @ 0x57b863..0x57b8dc -> @ 0x57b69b..0x57b81a].
void test_piece_draw_row() {
	const DeathPieceModel model = three_section_model();
	DeathPiece piece = section_piece(2);
	DeathPieceDraw draw;
	CHECK(death_piece_draw(piece, model, 100 << 16, 1.0f, q16(-1000.0), draw));
	CHECK(draw.lod_level == 1);
	CHECK(draw.item_id == 900);
	CHECK(draw.hidden_mask == 0x3u);
	CHECK(draw.section == 2);
	CHECK(draw.pivoted);
	CHECK(draw.pivot_q16[0] == q16(-1.5) && draw.pivot_q16[1] == q16(0.5) &&
			draw.pivot_q16[2] == q16(0.25));
	CHECK(draw.heading == 45.0f && draw.pitch == -10.0f && draw.roll == 5.0f);
	// The scale rides the matrix as ftol(scale * 65536.0).
	CHECK(draw.scale == static_cast<float>(static_cast<int32_t>(1.3f * 65536.0)) / 65536.0f);
	CHECK(!draw.below_water);
	// At or under the water plane the transparents take the below queue.
	CHECK(death_piece_draw(piece, model, 100 << 16, 1.0f, q16(5.0), draw));
	CHECK(draw.below_water);
	CHECK(death_piece_draw(piece, model, 100 << 16, 1.0f, q16(4.5), draw));
	CHECK(!draw.below_water);
	// The coarse level draws with ITS section count: section 2 is past the
	// two-section level, so the first clear bit below 2 is none -> 0.
	CHECK(death_piece_draw(piece, model, 30 << 16, 1.0f, q16(-1000.0), draw));
	CHECK(draw.lod_level == 2);
	CHECK(draw.section == 0);
	CHECK(draw.pivot_q16[0] == 0);
	// Under the third threshold: the empty fourth slot, no draw.
	CHECK(!death_piece_draw(piece, model, 1 << 16, 1.0f, q16(-1000.0), draw));
	// Sub-pixel: no draw.
	CHECK(!death_piece_draw(piece, model, 0xC000, 1.0f, q16(-1000.0), draw));
	// No hidden mask: the plain pose matrix, no pivot.
	piece.hidden_mask = 0;
	CHECK(death_piece_draw(piece, model, 300 << 16, 1.0f, q16(-1000.0), draw));
	CHECK(!draw.pivoted);
	CHECK(draw.section == 0);
}

OcclusionFrameCamera camera_at(float x, float y, float z, float fog) {
	// Presentation frame: eye at mission (x, y, z) looking down mission +x.
	OcclusionViewSpec view;
	view.eye[0] = x;
	view.eye[1] = z;
	view.eye[2] = -y;
	view.forward[0] = 1.0f;
	view.forward[1] = 0.0f;
	view.forward[2] = 0.0f;
	view.right[0] = 0.0f;
	view.right[1] = 0.0f;
	view.right[2] = 1.0f;
	view.up[0] = 0.0f;
	view.up[1] = 1.0f;
	view.up[2] = 0.0f;
	view.fov_y_deg = 60.0f;
	view.aspect = 16.0f / 9.0f;
	view.viewport_width = 1920.0f;
	view.fog_dist_units = fog;
	view.water_z_units = -1000.0f;
	OcclusionFrameCamera cam;
	occlusion_camera_from_view(view, cam);
	return cam;
}

// The collect's gates over the pool, pool order kept, each projecting the
// piece model radius [orig: DeathPiece_CollectVisible @ 0x57b5a1..0x57b627].
void test_collect_gates() {
	auto w_heap = std::make_unique<World>();
	World &w = *w_heap;
	ItemDeathTraits traits;
	traits.piece_model = three_section_model();
	w.tables.item_death_traits.set(900, traits);
	const OcclusionFrameCamera cam = camera_at(0.0f, 0.0f, 2.0f, 100.0f);
	auto add = [&](float x, float y) -> DeathPiece & {
		DeathPiece &p = w.death_pieces.alloc();
		p = section_piece(1);
		p.generation = 7;
		p.pos = Vec3{x, y, 2.0f};
		return p;
	};
	add(30.0f, 0.0f);          // slot 0: ahead, in view
	add(-30.0f, 0.0f);         // slot 1: behind the eye
	add(30.0f, 150.0f);        // slot 2: past the fog box in y
	add(150.0f, 0.0f);         // slot 3: past the fog box in x
	add(60.0f, 1.0f);          // slot 4: ahead, farther
	DeathPiece &modelless = add(40.0f, 0.0f); // slot 5: an item with no piece model
	modelless.item_id = 901;
	std::vector<DeathPieceDraw> draws;
	OcclusionWorld occlusion;
	occlusion.collect_death_piece_draws(w, cam, draws);
	CHECK(draws.size() == 2);
	if (draws.size() == 2) {
		CHECK(draws[0].slot == 0 && draws[1].slot == 4);
		CHECK(draws[0].generation == 7);
		// The recorded radius is the viewport projection of the piece radius
		// at the view depth (Viewport_TransformAndClipPoint).
		const int32_t expected = r::project_bound_sphere_radius_q16(q16(4.0), q16(30.0),
				cam.focal_pixels);
		CHECK(std::abs(draws[0].projected_radius_q16 - expected) <= expected / 1000 + 2);
		CHECK(draws[1].projected_radius_q16 < draws[0].projected_radius_q16);
		CHECK(draws[0].section == 1);
	}
	// An inactive slot is never collected.
	w.death_pieces.pieces[0].active = false;
	occlusion.collect_death_piece_draws(w, cam, draws);
	CHECK(draws.size() == 1 && draws[0].slot == 4);
}

} // namespace

int main() {
	test_lod_scale();
	test_lod_level_chain();
	test_render_section();
	test_piece_draw_row();
	test_collect_gates();
	if (failures == 0) std::printf("death_piece_draw_test: all checks passed\n");
	return failures == 0 ? 0 : 1;
}
