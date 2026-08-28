// Render-slot entity ground-shadow planner — pins the witnessed direction,
// LOD, cadence, scoring/assignment, dominant-light, march, and drape-color
// laws of docs/render/render-lighting-re.md ("the render-slot side").

#include <runtime/renderer/render_slot_shadow.h>

#include <cmath>
#include <cstdio>

static int failures = 0;

#define CHECK(cond)                                                        \
	do {                                                                   \
		if (!(cond)) {                                                     \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
			++failures;                                                    \
		}                                                                  \
	} while (0)

using namespace opennova::renderer;

static bool near_f(float a, float b, float eps = 1.0e-5f) {
	return std::fabs(a - b) <= eps;
}

int main() {
	// --- slot_projection_direction: the 0.25 vertical clamp then negation
	// [orig: render_shadow_pass @ 0x5d7bdc..0x5d7c30].
	{
		const auto noon = slot_projection_direction({0.0f, 1.0f, 0.0f});
		CHECK(near_f(noon[0], 0.0f) && near_f(noon[1], -1.0f) &&
				near_f(noon[2], 0.0f));
		// A 7-degree dawn sun (y ~ 0.122) clamps to the 0.25 floor: the
		// projection behaves as if the sun sat at ~14.5 degrees.
		const auto dawn = slot_projection_direction({0.97f, 0.122f, 0.2f});
		CHECK(near_f(dawn[1], -0.25f));
		CHECK(near_f(dawn[0], -0.97f) && near_f(dawn[2], -0.2f));
		// Above the floor the tuple passes through negated.
		const auto high = slot_projection_direction({0.5f, 0.7f, 0.1f});
		CHECK(near_f(high[1], -0.7f));
	}

	// --- registration LOD laws [orig: RenderSlot_AllocSlot @ 0x5d5690].
	CHECK(slot_lod_for_radius(1.0f) == 6);    // 2*1+1=3 -> floor 6
	CHECK(slot_lod_for_radius(4.0f) == 9);    // 2*4+1
	CHECK(slot_lod_for_radius(50.0f) == 20);  // ceiling
	CHECK(slot_lod_for_blob(6.0f, 8.0f) == 15);  // max(w,l)+7
	CHECK(slot_lod_for_blob(0.0f, 0.0f) == 7);
	CHECK(slot_lod_for_blob(40.0f, 2.0f) == 20);

	// --- grazing rescale [orig: @ 0x5d6d5c..0x5d6dac].
	// Straight-down (y=-1): (0.5+0.5)*base = base.
	CHECK(grazing_slot_lod(8, -1.0f) == 8);
	// The clamp floor (|y|=0.25): (0.5+2)*base = 2.5*base, ceiling 20.
	CHECK(grazing_slot_lod(8, -0.25f) == 20);
	CHECK(grazing_slot_lod(6, -0.25f) == 15);
	CHECK(grazing_slot_lod(2, -1.0f) == 6);  // floor 6

	// --- silhouette capture extent [orig: @ 0x5d783e..0x5d7871].
	CHECK(near_f(silhouette_half_extent(1.0f), 1.25f));   // r*1.25
	CHECK(near_f(silhouette_half_extent(8.0f), 8.75f));   // clamp r+0.75

	// --- RT chain [orig: RenderSlot_InitTextureChain @ 0x5d5320].
	CHECK(slot_texture_size(0, 2) == 512);
	CHECK(slot_texture_size(1, 2) == 512);
	CHECK(slot_texture_size(2, 2) == 256);
	CHECK(slot_texture_size(3, 2) == 256);
	CHECK(slot_texture_size(8, 2) == 32);
	CHECK(slot_texture_size(11, 2) == 32);
	CHECK(slot_texture_size(0, 1) == 256);
	CHECK(slot_texture_size(0, 4) == 1024);
	CHECK(slot_texture_size(9, 4) == 64);
	CHECK(slot_texture_size(11, 4) == 32);

	// --- refresh cadence [orig: @ 0x5d76d9..0x5d7748].
	CHECK(slot_refresh_mask(0) == 7);
	CHECK(slot_refresh_mask(1) == 7);
	CHECK(slot_refresh_mask(2) == 3);
	CHECK(slot_refresh_mask(3) == 1);
	CHECK(slot_refresh_mask(4) == 0);
	CHECK(slot_refresh_due(5, 5, 3, false));   // 5&3 == 5&3
	CHECK(!slot_refresh_due(5, 6, 3, false));
	CHECK(slot_refresh_due(5, 6, 3, true));    // dirty forces
	CHECK(slot_refresh_due(3, 9000, 0, false));  // detail 4+: every frame

	// --- drape fade / cull [orig: RenderSlot_DrawSilhouetteDrape @ 0x5d5d30..0x5d5d53].
	CHECK(near_f(drape_fade(10.0f), 0.0f));
	CHECK(near_f(drape_fade(40.0f), 0.0f));
	CHECK(near_f(drape_fade(60.0f), 0.5f));
	CHECK(near_f(drape_fade(80.0f), 1.0f));
	CHECK(!drape_culled(79.9f));
	CHECK(drape_culled(80.0f));

	// --- the sun drape ambient law [orig: @ 0x5d5f63..0x5d6008].
	{
		// Equal sun and sky at |y| = 1: q = 0.5, ambient = 0.5.
		const auto q =
				drape_shadow_term({1, 1, 1}, {1, 1, 1}, -1.0f);
		CHECK(near_f(q[0], 0.5f) && near_f(q[1], 0.5f) && near_f(q[2], 0.5f));
		// The grazing clamp floor: |y| = 0.25 quarters the sun term.
		const auto qg = drape_shadow_term({1, 1, 1}, {1, 1, 1}, -0.25f);
		CHECK(near_f(qg[0], 0.2f));
		// Ambient = 1 - (1-fade)*q.
		const auto amb = drape_sun_ambient({1, 1, 1}, {1, 1, 1}, -1.0f, 0.0f);
		CHECK(near_f(amb[0], 0.5f));
		const auto amb_faded =
				drape_sun_ambient({1, 1, 1}, {1, 1, 1}, -1.0f, 0.5f);
		CHECK(near_f(amb_faded[0], 0.75f));
		// Zero sky: the shadow removes the whole sun term.
		const auto amb_dark =
				drape_sun_ambient({1, 1, 1}, {0, 0, 0}, -1.0f, 0.0f);
		CHECK(near_f(amb_dark[0], 0.0f));
		// The channel ratio is per-channel: a blue sky protects blue.
		const auto amb_rgb = drape_sun_ambient(
				{1.0f, 0.9f, 0.7f}, {0.2f, 0.3f, 0.6f}, -1.0f, 0.0f);
		CHECK(amb_rgb[2] > amb_rgb[0]);
	}

	// --- fixed-function silhouette composition [orig: @ 0x5d5ca0].
	{
		// White is the untouched RT clear and must remain a no-op.
		const auto background = drape_silhouette_factor(
				{1.0f, 1.0f, 1.0f}, {0.2f, 0.5f, 1.0f}, 0.0f, 0.0f);
		CHECK(near_f(background[0], 1.0f) &&
				near_f(background[1], 1.0f) &&
				near_f(background[2], 1.0f));

		// A fully black silhouette leaves only the per-channel ambient.
		const auto full = drape_silhouette_factor(
				{0.0f, 0.0f, 0.0f}, {0.2f, 0.5f, 1.0f}, 0.0f, 0.0f);
		CHECK(near_f(full[0], 0.8f) && near_f(full[1], 0.5f) &&
				near_f(full[2], 0.0f));

		// At dawn a gray resolved/filter edge saturates back to lit. The old
		// alpha product yielded 1 - 0.2*0.2 = 0.96 and created the faint tail.
		const auto edge = drape_silhouette_factor(
				{0.8f, 0.8f, 0.8f}, {0.2f, 0.2f, 0.2f}, 0.0f, 0.0f);
		CHECK(near_f(edge[0], 1.0f) && near_f(edge[1], 1.0f) &&
				near_f(edge[2], 1.0f));

		// Distance fade raises the ambient; white shadowztex suppresses the
		// silhouette entirely. The final stage saturates every channel.
		const auto faded = drape_silhouette_factor(
				{0.0f, 0.0f, 0.0f}, {0.4f, 0.4f, 0.4f}, 0.5f, 0.0f);
		CHECK(near_f(faded[0], 0.8f));
		const auto clipped = drape_silhouette_factor(
				{0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, 0.0f, 1.0f);
		CHECK(near_f(clipped[0], 1.0f) && near_f(clipped[1], 1.0f) &&
				near_f(clipped[2], 1.0f));
	}

	// --- slot lighting darkening constants
	// [orig: RenderSlot_SetupNextLighting @ 0x5d73d3..0x5d740d].
	{
		const auto dark = slot_light_darkening({1.0f, 1.0f, 1.0f});
		// lum = 0.3+0.6+0.1 = 1; (1+1)*0.5*-3 = -3 per channel.
		CHECK(near_f(dark[0], -3.0f) && near_f(dark[1], -3.0f) &&
				near_f(dark[2], -3.0f));
		const auto scale =
				drape_attached_light_scale({1.0f, 1.0f, 1.0f}, 0.5f);
		// -(c+lum)*(1-fade) = -2*0.5 = -1.
		CHECK(near_f(scale[0], -1.0f));
	}

	// --- dominant-light pick [orig: RenderSlot_UpdateEntityLight @ 0x5d6a30].
	{
		const std::array<float, 3> entity{0.0f, 0.0f, 0.0f};
		const std::array<float, 3> sun_dir{0.0f, -1.0f, 0.0f};
		SlotPointLight weak;
		weak.position = {10.0f, 0.0f, 0.0f};
		weak.color = {0.1f, 0.1f, 0.1f};
		weak.attenuation = {1.0f, 0.0f, 1.0f, 0.0f};
		weak.handle = 7;
		// lum 0.1 / (100 + 1) — far below the 0.1 threshold.
		auto pick = pick_dominant_light(entity, sun_dir, &weak, 1, false);
		CHECK(pick.attached_handle == 0);
		CHECK(near_f(pick.direction[1], -1.0f));
		// A close bright light wins and the direction becomes
		// normalize(entity - light).
		SlotPointLight strong;
		strong.position = {0.0f, 1.0f, 0.0f};
		strong.color = {1.0f, 1.0f, 1.0f};
		strong.attenuation = {1.0f, 0.0f, 0.1f, 0.0f};
		strong.handle = 9;
		pick = pick_dominant_light(entity, sun_dir, &strong, 1, false);
		CHECK(pick.attached_handle == 9);
		CHECK(near_f(pick.direction[1], -1.0f));
		// Interior zeroes the threshold: the weak light now wins.
		pick = pick_dominant_light(entity, sun_dir, &weak, 1, true);
		CHECK(pick.attached_handle == 7);
		// The strongest of several wins.
		SlotPointLight both[2] = {weak, strong};
		pick = pick_dominant_light(entity, sun_dir, both, 2, false);
		CHECK(pick.attached_handle == 9);
	}

	// --- anchor march [orig: @ 0x5d6c86..0x5d6d67].
	{
		// Flat terrain at 0, entity 2 u up, light along +x at the clamp
		// floor: planar unit steps, the TRUE vertical rate of -0.25/step is
		// kept (the -0.5 substitute is for non-descending steps only
		// @ 0x5d6cd7..0x5d6cdf) -> 8 steps to ground.
		const auto flat = [](float, float) { return 0.0f; };
		const auto anchor = march_shadow_anchor(
				{0.0f, 2.0f, 0.0f}, {1.0f, -0.25f, 0.0f}, flat);
		CHECK(near_f(anchor[0], 8.0f));
		CHECK(near_f(anchor[1], 0.0f));
		// A steep direction keeps its own vertical rate.
		const auto steep = march_shadow_anchor(
				{0.0f, 2.0f, 0.0f}, {0.5f, -1.0f, 0.0f}, flat);
		CHECK(near_f(steep[0], 1.0f));
		// A level or rising direction takes the -0.5 u substitute.
		const auto level = march_shadow_anchor(
				{0.0f, 2.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, flat);
		CHECK(near_f(level[0], 4.0f));
		// Terrain above the start: the anchor stays at the entity.
		const auto high = [](float, float) { return 10.0f; };
		const auto at_start = march_shadow_anchor(
				{3.0f, 2.0f, 4.0f}, {1.0f, -1.0f, 0.0f}, high);
		CHECK(near_f(at_start[0], 3.0f) && near_f(at_start[1], 4.0f));

		// 03TR's authored M939 stands seven Q16 ticks above a raw16 terrain
		// sample (2.500107 vs 2.5). Retail's live vehicle has settled below
		// the pad, so its march exits at step zero. The presentation-only
		// caster must reconcile a sub-quantum gap before applying the same
		// exact march; otherwise one planar step crosses the lod-20 4 u snap.
		const float truck_y = 2.500107f;
		const float pad_y = 2.5f;
		const float grounded_y = slot_march_start_height(truck_y, pad_y,
				kSlotTerrainHeightQuantumUnits);
		CHECK(near_f(grounded_y, pad_y));
		const auto truck_anchor = march_shadow_anchor(
				{-587.4066f, grounded_y, 1070.15f},
				{-0.910269f, -0.244322f, 0.334242f},
				[pad_y](float, float) { return pad_y; });
		const SlotPatch truck_patch =
				slot_patch_bounds(truck_anchor[0], -truck_anchor[1], 20);
		CHECK(near_f(truck_anchor[0], -587.4066f));
		CHECK(near_f(truck_patch.min_x, -596.0f));
		// A caster more than one height quantum above the same pad remains
		// airborne and still enters the retail march.
		CHECK(near_f(slot_march_start_height(2.51f, pad_y,
						 kSlotTerrainHeightQuantumUnits),
				2.51f));
	}

	// --- the drape patch [orig: RenderSlot_RebuildPatchVertexBuffer
	// @ 0x5d5130 — origin anchor - lod/2 east / + lod/2 north, rounded to
	// the lod band's grid, lod cells east and south].
	{
		// lod 6 (1 u grid): anchor (10.3, 20.6) -> origin (7, 24) rounded
		// from (7.3, 23.6); the square runs [7, 13] x [18, 24].
		const SlotPatch p6 = slot_patch_bounds(10.3f, 20.6f, 6);
		CHECK(near_f(p6.min_x, 7.0f) && near_f(p6.max_x, 13.0f));
		CHECK(near_f(p6.min_north, 18.0f) && near_f(p6.max_north, 24.0f));
		// lod 12 (2 u grid): (10.3 - 6, 20.6 + 6) = (4.3, 26.6) -> (4, 26).
		const SlotPatch p12 = slot_patch_bounds(10.3f, 20.6f, 12);
		CHECK(near_f(p12.min_x, 4.0f) && near_f(p12.max_x, 16.0f));
		CHECK(near_f(p12.min_north, 14.0f) && near_f(p12.max_north, 26.0f));
		// lod 20 (4 u grid): (0.3, 30.6) -> (0, 32); the 20 u cap.
		const SlotPatch p20 = slot_patch_bounds(10.3f, 20.6f, 20);
		CHECK(near_f(p20.min_x, 0.0f) && near_f(p20.max_x, 20.0f));
		CHECK(near_f(p20.min_north, 12.0f) && near_f(p20.max_north, 32.0f));
	}

	// --- the depth-clip stage [orig: shadow_system_init_resources
	// @ 0x5d6260..0x5d62a7; build_shadow_cascade_uv_matrices @ 0x58cf10].
	{
		const auto px = shadowztex_pixels();
		CHECK(px.size() == 128);
		CHECK(px[0] == 0xFFFFFFFFu && px[15] == 0xFFFFFFFFu);  // row 0 white below 16
		CHECK(px[16] == 0xFF808080u);                           // the gray texel
		CHECK(px[17] == 0xFF000000u && px[31] == 0xFF000000u);  // black beyond
		CHECK(px[2 * 32 + 20] == 0xFF000000u);                  // row 2 same
		CHECK(px[3 * 32 + 20] == 0xFFFFFFFFu);                  // row 3 all white
		// A unit slot direction straight down the +x slope (x 0.8, y -0.6),
		// half size 2 -> k = 0.25; entity at the origin, lp = -dir.
		const std::array<float, 3> dir = {0.8f, -0.6f, 0.0f};
		const SlotDepthClip clip = slot_depth_clip(dir, 2.0f, false, {0, 0, 0});
		const auto u_at = [&](float x, float y, float z) {
			return clip.u_axis[0] * x + clip.u_axis[1] * y + clip.u_axis[2] * z +
					clip.u_offset;
		};
		const auto v_at = [&](float x, float y, float z) {
			return clip.v_axis[0] * x + clip.v_axis[1] * y + clip.v_axis[2] * z +
					clip.v_offset;
		};
		// The caster plane through lp: u2 = 0.5 + k * dir.(p - lp); at p = lp
		// u2 = 0.5, at the entity (p - lp = dir) u2 = 0.75 — beyond the plane
		// (away from the light) the black half draws.
		CHECK(near_f(u_at(-0.8f, 0.6f, 0.0f), 0.5f));
		CHECK(near_f(u_at(0.0f, 0.0f, 0.0f), 0.75f));
		// Toward the light (p - lp = -dir) the white half suppresses.
		CHECK(u_at(-1.6f, 1.2f, 0.0f) < 0.5f);
		// v2 = 0.5 + 0.333 k^2 dir.(p - lp): 4 u along the light past lp is
		// 0.5 + 0.333 * 0.0625 * 4.
		CHECK(near_f(v_at(-0.8f + 3.2f, 0.6f - 2.4f, 0.0f),
				0.5f + 0.333f * 0.0625f * 4.0f));
		// A person steepens the clip direction's vertical x4 before the
		// normalize: f2 = normalize(0.8, -2.4, 0) = (0.316, -0.949, 0).
		const SlotDepthClip person = slot_depth_clip(dir, 2.0f, true, {0, 0, 0});
		CHECK(near_f(person.u_axis[0], 0.25f * 0.3162f, 1.0e-3f));
		CHECK(near_f(person.u_axis[1], 0.25f * -0.9487f, 1.0e-3f));
		// ...and leaves the v (far-fade) row on the unscaled direction.
		CHECK(near_f(person.v_axis[0], clip.v_axis[0]));
		CHECK(near_f(person.v_axis[1], clip.v_axis[1]));
	}

	// --- priority scoring [orig: RenderSlot_SortAndAssign
	// @ 0x5d6530].
	{
		SlotCandidateState near_front;
		near_front.pos2d = {0.0f, 10.0f};
		SlotCandidateState far_front = near_front;
		far_front.pos2d = {0.0f, 50.0f};
		const std::array<float, 2> cam{0.0f, 0.0f};
		const std::array<float, 2> view{0.0f, 1.0f};
		const int32_t s_near = slot_priority_score(cam, view, near_front);
		const int32_t s_far = slot_priority_score(cam, view, far_front);
		CHECK(s_near < s_far);
		// Behind the camera weights 2.5 vs 0.5 in front.
		SlotCandidateState behind = near_front;
		behind.pos2d = {0.0f, -10.0f};
		CHECK(slot_priority_score(cam, view, behind) == 5 * s_near);
		// The local player halves.
		SlotCandidateState player = near_front;
		player.is_local_player_or_parent = true;
		CHECK(slot_priority_score(cam, view, player) == s_near / 2);
		// Exclusions.
		SlotCandidateState dead = near_front;
		dead.dead = true;
		CHECK(slot_priority_score(cam, view, dead) == kSlotScoreExcluded);
		SlotCandidateState seated = near_front;
		seated.seat_parented = true;
		CHECK(slot_priority_score(cam, view, seated) == kSlotScoreExcluded);
		SlotCandidateState riding = near_front;
		riding.on_vehicle = true;
		CHECK(slot_priority_score(cam, view, riding) == kSlotScoreExcluded);
		// The 320 u bind horizon [orig: @ 0x5d66b2].
		SlotCandidateState distant = near_front;
		distant.pos2d = {0.0f, 321.0f};
		CHECK(slot_priority_score(cam, view, distant) == kSlotScoreExcluded);
		distant.pos2d = {0.0f, 300.0f};
		CHECK(slot_priority_score(cam, view, distant) != kSlotScoreExcluded);
	}

	// --- assignment: 24-patch / 12-capture partition, sticky orders,
	// blob fallback [orig: @ 0x5d68ed..0x5d6a25; RenderSlot_DrawAllDrapes
	// @ 0x5d6e20].
	{
		RenderSlotPlan plan;
		for (uint64_t id = 1; id <= 30; ++id) {
			CHECK(plan.register_entity(id));
		}
		const auto state_for = [](uint64_t id) {
			SlotCandidateState state;
			// ids 1..30 at increasing forward distance; id 20 authors a
			// blob decal; id 29 is seat-parented.
			state.pos2d = {0.0f, static_cast<float>(id) * 2.0f};
			state.dynamic = true;
			state.has_blob_texture = id == 20;
			state.seat_parented = id == 29;
			return state;
		};
		const std::array<float, 2> cam{0.0f, 0.0f};
		const std::array<float, 2> view{0.0f, 1.0f};
		auto out = plan.assign(cam, view, state_for);
		int bound = 0;
		int captures = 0;
		int blobs = 0;
		for (const auto &a : out) {
			bound += a.bound ? 1 : 0;
			captures += a.draws_silhouette ? 1 : 0;
			blobs += a.draws_blob ? 1 : 0;
		}
		CHECK(bound == 24);
		CHECK(captures == 12);
		// id 20 ranks 19th: bound to a drape patch but past the 12-capture
		// budget -> the authored blob drapes in place of a silhouette
		// [orig: RenderSlot_DrawAllDrapes @ 0x5d6eb6..0x5d6ec4].
		CHECK(blobs == 1);
		CHECK(out[19].id == 20 && out[19].bound &&
				out[19].capture_order == -1 && out[19].draws_blob);
		// The nearest candidate captures at order 0.
		CHECK(out[0].id == 1 && out[0].capture_order == 0 &&
				out[0].capture_dirty);
		// The seat-parented record never binds.
		CHECK(!out[28].bound && out[28].excluded);
		// Sticky captures: a second identical frame is clean.
		out = plan.assign(cam, view, state_for);
		CHECK(out[0].capture_order == 0 && !out[0].capture_dirty);
		// Reordering re-marks dirty only where the order changed.
		const auto swapped = [&](uint64_t id) {
			SlotCandidateState state = state_for(id);
			if (id == 1) {
				state.pos2d = {0.0f, 4.5f};  // now ranks second
			}
			return state;
		};
		out = plan.assign(cam, view, swapped);
		CHECK(out[0].capture_order == 1 && out[0].capture_dirty);
		CHECK(out[1].capture_order == 0 && out[1].capture_dirty);
		CHECK(out[2].capture_order == 2 && !out[2].capture_dirty);
		// Release frees the record.
		plan.release_entity(1);
		CHECK(plan.registered_count() == 29);
	}

	// --- the per-slot refresh mask and the first-person drape gate
	// [orig: @ 0x5d7713..0x5d7734; RenderSlot_DrawAllDrapes
	// @ 0x5d6e70..0x5d6e90].
	{
		CHECK(slot_refresh_mask_for(2, false) == 3);
		CHECK(slot_refresh_mask_for(2, true) == 3);  // below detail 3: no exception
		CHECK(slot_refresh_mask_for(3, false) == 1);
		CHECK(slot_refresh_mask_for(3, true) == 0);  // the local player: every frame
		CHECK(slot_refresh_mask_for(4, false) == 0);
		CHECK(local_first_person_drape_skipped(true, true, 4));
		CHECK(local_first_person_drape_skipped(true, false, 1));
		CHECK(!local_first_person_drape_skipped(true, false, 2));
		CHECK(!local_first_person_drape_skipped(false, true, 1));
	}

	// --- fixed-index records: a release never re-phases the slots behind
	// it [orig: RenderSlot_AllocSlot @ 0x5d5690 — an entity's index is
	// its own for life]; the freed index is the next one claimed (the device
	// fold render_slot_shadow.h describes — retail never releases).
	{
		RenderSlotPlan plan;
		for (uint64_t id = 1; id <= 5; ++id) {
			CHECK(plan.register_entity(id));
		}
		const auto state_for = [](uint64_t id) {
			SlotCandidateState state;
			state.pos2d = {0.0f, static_cast<float>(id) * 2.0f};
			return state;
		};
		const std::array<float, 2> cam{0.0f, 0.0f};
		const std::array<float, 2> view{0.0f, 1.0f};
		auto out = plan.assign(cam, view, state_for);
		CHECK(out.size() == 5 && out[4].id == 5 && out[4].record_index == 4);
		plan.release_entity(2);
		CHECK(plan.registered_count() == 4);
		out = plan.assign(cam, view, state_for);
		CHECK(out.size() == 4);
		int index_of_5 = -1;
		for (const auto &a : out) {
			if (a.id == 5) {
				index_of_5 = a.record_index;
			}
		}
		CHECK(index_of_5 == 4);
		CHECK(plan.register_entity(9));
		out = plan.assign(cam, view, state_for);
		int index_of_9 = -1;
		for (const auto &a : out) {
			if (a.id == 9) {
				index_of_9 = a.record_index;
			}
		}
		CHECK(index_of_9 == 1);
		CHECK(plan.register_entity(9));  // idempotent on a live record
		CHECK(plan.registered_count() == 5);
	}

	// --- registration cap [orig: RenderSlot_AllocSlot @ 0x5d56d6].
	{
		RenderSlotPlan plan;
		for (uint64_t id = 1; id <= 256; ++id) {
			CHECK(plan.register_entity(id));
		}
		CHECK(!plan.register_entity(999));
		CHECK(plan.register_entity(37));  // idempotent re-register
	}

	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("render_slot_shadow_test: all checks passed\n");
	return 0;
}
