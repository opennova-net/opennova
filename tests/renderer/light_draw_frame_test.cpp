// renderer::LightDrawFrame, StaticLightRows and light_corona_frame_inputs: the
// per-draw light pass's draw contexts (the owner/interior groups each drawn
// model declares, the per-ROBJ re-scope, the section-0 fallback), the retained
// static rows' cache and atlas, and the corona walk's inputs
// (renderer/light_draw_frame.h carries the witnesses; the pool's own
// semantics are light_scene_test's). The logic GUT's
// per_model_light_isolation_test drives through ObjectModel nodes.

#include <runtime/renderer/light_draw_frame.h>

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

namespace r = opennova::renderer;

int g_failures = 0;

void check(bool ok, const char *message) {
	if (!ok) {
		std::fprintf(stderr, "FAIL: %s\n", message);
		++g_failures;
	}
}

constexpr int32_t kOne = 65536;

r::LightHandle spawn(r::LightScene &scene, int32_t x, uint64_t owner, int32_t section,
		bool disable_objects = false) {
	r::LightSpawnParams params;
	params.position_fixed = {x * kOne, 0, 0};
	params.radius_fixed = 10 * kOne;
	params.owner_entity = owner;
	params.owner_section = section;
	params.disable_objects = disable_objects;
	return scene.spawn(params);
}

r::LightDrawSubject subject_at(int32_t x, uint64_t owner) {
	r::LightDrawSubject subject;
	subject.query.position_fixed = {x * kOne, 0, 0};
	subject.query.bound_radius_fixed = 2 * kOne;
	subject.owner_entity = owner;
	return subject;
}

size_t lit(const r::LightDrawFrame &frame, size_t target) {
	return frame.selections()[target].count;
}

void an_owned_light_reaches_only_its_owners_skinned_person_draw() {
	r::LightScene scene;
	constexpr uint64_t kShooter = 7;
	spawn(scene, 0, kShooter, 0); // the shooter's muzzle glow
	spawn(scene, 1, 0, 0);        // a world light
	r::LightDrawFrame frame;
	r::LightDrawSubject body = subject_at(0, kShooter);
	body.person_wave = true;
	body.skinned = true;
	frame.add(body);
	r::LightDrawSubject gun = subject_at(0, kShooter); // the rigid held gun
	gun.person_wave = true;
	frame.add(gun);
	r::LightDrawSubject other = subject_at(0, 9);
	other.person_wave = true;
	other.skinned = true;
	frame.add(other);
	r::LightDrawSubject first_person = subject_at(0, 0); // declares no owner
	frame.add(first_person);
	frame.select(scene, r::LightFrameInputs{});
	check(frame.targets().size() == 4, "one context per draw");
	check(lit(frame, 0) == 2, "the shooter's skinned body takes its own glow and the world light");
	check(lit(frame, 1) == 1, "the rigid gun re-scopes to (0, robj): the world light only");
	check(lit(frame, 2) == 1, "another person: the world light only");
	check(lit(frame, 3) == 1, "the first-person pass: the world light only");
	check(!frame.any_per_robj_building(), "no building draw");
}

void a_building_selects_per_drawn_robj_as_its_own_interior_group() {
	r::LightScene scene;
	constexpr uint64_t kBuilding = 40;
	spawn(scene, 0, kBuilding, 2); // the room light of section 2
	r::LightDrawFrame frame;
	const int32_t robjs[] = {1, 2};
	r::LightDrawSubject building = subject_at(0, kBuilding);
	building.building = true;
	building.robjs = robjs;
	building.robj_count = 2;
	frame.add(building);
	r::LightDrawSubject empty = subject_at(0, kBuilding);
	empty.building = true; // draws no ROBJ: one ROBJ-0 context
	frame.add(empty);
	frame.select(scene, r::LightFrameInputs{});
	check(frame.targets().size() == 3, "two drawn ROBJs and the section-0 fallback");
	check(frame.targets()[0].per_robj && frame.targets()[0].robj_index == 1, "ROBJ 1's row");
	check(frame.draws()[0].groups.interior_group_entity == kBuilding &&
					frame.draws()[0].groups.interior_group_section == 0,
			"a building is its own interior group at section zero");
	check(frame.draws()[1].groups.owner_group_section == 2, "the owner section re-scoped per ROBJ");
	check(lit(frame, 0) == 0, "ROBJ 1 does not take section 2's light");
	check(lit(frame, 1) == 1, "ROBJ 2 takes it");
	check(frame.targets()[2].per_robj && frame.targets()[2].robj_index == 0 && lit(frame, 2) == 0,
			"the fallback row is ROBJ 0, never the wider entity admission");
	check(frame.subject_count() == 2 && frame.any_per_robj_building(), "two subjects, buildings");
}

void a_rigid_draw_at_interior_section_zero_splits_per_robj() {
	r::LightScene scene;
	constexpr uint64_t kBuilding = 40;
	spawn(scene, 0, kBuilding, 3);
	r::LightDrawFrame frame;
	const int32_t robjs[] = {0, 3};
	r::LightDrawSubject crate = subject_at(0, 11);
	crate.interior_entity = kBuilding;
	crate.interior_section = 0;
	crate.robjs = robjs;
	crate.robj_count = 2;
	frame.add(crate);
	r::LightDrawSubject inside = subject_at(0, 12);
	inside.interior_entity = kBuilding;
	inside.interior_section = 3;
	inside.robjs = robjs;
	inside.robj_count = 2;
	frame.add(inside);
	r::LightDrawSubject skinned = subject_at(0, 13);
	skinned.interior_entity = kBuilding;
	skinned.skinned = true;
	skinned.robjs = robjs;
	skinned.robj_count = 2;
	frame.add(skinned);
	frame.select(scene, r::LightFrameInputs{});
	check(frame.targets().size() == 4, "section zero splits; a set section or a skinned draw does not");
	check(lit(frame, 0) == 0 && lit(frame, 1) == 1, "the gate falls back to each ROBJ's section");
	check(!frame.targets()[2].per_robj && lit(frame, 2) == 1, "the interior section admits it whole");
	check(!frame.targets()[3].per_robj && lit(frame, 3) == 0, "a skinned draw keeps section 0");
}

void the_entity_query_is_shared_and_the_objects_disable_gates() {
	r::LightScene scene;
	spawn(scene, 0, 0, 0, true); // objects disabled
	spawn(scene, 30, 0, 0);      // out of the entity cube
	r::LightDrawFrame frame;
	frame.add(subject_at(0, 1));
	frame.select(scene, r::LightFrameInputs{});
	check(lit(frame, 0) == 0, "objects-disabled and out-of-cube lights never reach a draw");
}

r::StaticLightRow row_at(int32_t x) {
	r::StaticLightRow row;
	row.active = true;
	row.query.position_fixed = {x * kOne, 0, 0};
	row.query.bound_radius_fixed = 2 * kOne;
	row.lane = {0.5f, 1.0f, 0.25f, -3.0f};
	return row;
}

void static_rows_pack_their_atlas_and_keep_it_in_place() {
	r::LightScene scene;
	const r::LightHandle light = spawn(scene, 0, 0, 0);
	r::StaticLightRows rows;
	std::vector<r::StaticLightRow> set;
	set.push_back(row_at(0));
	set.push_back(row_at(100)); // unlit
	r::StaticLightRow carved = row_at(0);
	carved.active = false;
	set.push_back(carved);
	rows.set_rows(4, set);
	check(rows.select(scene, r::LightFrameInputs{}) == 1, "one lit active row");
	check(rows.take_dirty(), "the first frame writes the atlas");
	check(rows.texture_rows() == 3 && rows.active_draws() == 2, "three rows, two active");
	const std::vector<float> &texels = rows.texels();
	check(texels.size() == 3u * r::kStaticLightRowTexels * 4u, "ten RGBA texels per row");
	check(texels[0] == 1.0f, "row 0 holds one light");
	const size_t lane = r::kStaticLightRowLaneTexel * 4;
	check(texels[lane] == 0.5f && texels[lane + 3] == -3.0f, "row 0 carries its lane");
	const size_t row1 = r::kStaticLightRowTexels * 4;
	check(texels[row1] == 0.0f && texels[row1 + lane] == 0.5f, "an unlit active row keeps its lane");
	const size_t row2 = 2 * r::kStaticLightRowTexels * 4;
	check(texels[row2 + lane] == 0.0f, "a carved row stays zero");
	check(rows.select(scene, r::LightFrameInputs{}) == 1 && !rows.take_dirty(),
			"a quiet steady frame writes nothing");
	scene.set_blend(light, 0.5f);
	rows.select(scene, r::LightFrameInputs{});
	check(rows.take_dirty(), "a blend change rewrites the cached row in place");
	const std::vector<float> maintained = rows.texels();
	r::StaticLightRows fresh;
	fresh.set_rows(-1, set);
	fresh.select(scene, r::LightFrameInputs{});
	check(fresh.texels() == maintained, "the in-place maintenance matches a full rebuild");
	rows.clear_output();
	check(rows.take_dirty() && rows.texels()[0] == 0.0f, "the output clear zeroes the payload");
	rows.select(scene, r::LightFrameInputs{});
	check(rows.take_dirty() && rows.texels() == maintained,
			"the next frame rewrites every active row");
}

void corona_inputs_fold_the_camera_plane() {
	const std::array<int32_t, 3> camera = {10 * kOne, 0, 0};
	std::vector<r::LightCoronaOwnerMask> masks(1);
	masks[0].owner_entity = 3;
	r::LightCoronaFog fog;
	fog.enabled = true;
	fog.type = 2;
	fog.end = 50.0f;
	const r::LightCoronaFrameInputs inputs = r::light_corona_frame_inputs(
			camera, {2.0f, 0.0f, 0.0f}, r::LightFrameInputs{}, 3, fog, masks);
	check(inputs.depth_plane_normal[0] == 1.0f, "the forward normalizes");
	check(std::fabs(inputs.depth_plane_w + 10.0f) < 1e-5f, "depth is zero at the camera");
	check(inputs.frame_index == 3 && inputs.owner_mask_count == 1, "the phase and the masks");
	check(inputs.fog_enabled && inputs.fog_type == 2 && inputs.fog_end == 50.0f, "the primary fog");
}

void the_row_lane_folds_the_entity_lighting_and_the_mirror_clip() {
	const std::array<float, 4> outdoor =
			r::static_light_row_lane(false, 0, 0.0f, false, 0, 4 * kOne);
	check(outdoor[0] == 1.0f && outdoor[1] == 0.0f, "an outdoor static is unscaled, no lerp");
	const std::array<float, 4> contained =
			r::static_light_row_lane(false, 0, 0.0f, true, 0, 4 * kOne);
	check(contained[1] == 1.0f, "a contained static lerps");
}

} // namespace

int main() {
	an_owned_light_reaches_only_its_owners_skinned_person_draw();
	a_building_selects_per_drawn_robj_as_its_own_interior_group();
	a_rigid_draw_at_interior_section_zero_splits_per_robj();
	the_entity_query_is_shared_and_the_objects_disable_gates();
	static_rows_pack_their_atlas_and_keep_it_in_place();
	corona_inputs_fold_the_camera_plane();
	the_row_lane_folds_the_entity_lighting_and_the_mirror_clip();
	if (g_failures != 0) {
		std::fprintf(stderr, "%d check(s) failed\n", g_failures);
		return 1;
	}
	std::puts("light_draw_frame: ok");
	return 0;
}
