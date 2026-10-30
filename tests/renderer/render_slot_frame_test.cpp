// renderer::slot_frame, the render-slot pass's per-slot frame
// (docs/render/render-lighting-re.md, "the render-slot side"): which slots
// capture and drape, the stored slot direction (the raw clamped sun, or the
// unit attached-light direction), the capture-with child walk, the local
// first-person gate, the 80 u cull ahead of the fade, the attached-light and
// sun ambient legs, the grazing lod and the march. The leaf laws have their
// own vectors in render_slot_shadow_test.cpp; these pin how the frame
// composes them, the carrier's refNum walk among them.

#include <runtime/renderer/render_slot_frame.h>

#include <cmath>
#include <cstdio>
#include <map>
#include <vector>

static int failures = 0;

#define CHECK(cond)                                                        \
	do {                                                                   \
		if (!(cond)) {                                                     \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
			++failures;                                                    \
		}                                                                  \
	} while (0)

using namespace opennova::renderer;

namespace {

bool near_f(float a, float b, float eps = 1.0e-5f) {
	return std::fabs(a - b) <= eps;
}

bool same3(const std::array<float, 3> &a, const std::array<float, 3> &b) {
	return a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
}

SlotAssignment assignment(uint64_t id, int record, int order, bool silhouette = true,
		bool excluded = false) {
	SlotAssignment a;
	a.id = id;
	a.record_index = record;
	a.bound = true;
	a.capture_order = silhouette ? order : -1;
	a.draws_silhouette = silhouette;
	a.excluded = excluded;
	return a;
}

SlotFrameCaster caster_at(float x, float y, float z, float bound_radius, float capture_radius) {
	SlotFrameCaster c;
	c.position = {x, y, z};
	c.state.bound_radius = bound_radius;
	c.capture_radius = capture_radius;
	return c;
}

const SlotFrameSlot *find_slot(const SlotFrame &frame, uint64_t id) {
	for (const SlotFrameSlot &slot : frame.slots) {
		if (slot.id == id) {
			return &slot;
		}
	}
	return nullptr;
}

}  // namespace

// The frame's casters from their facts (slot_frame_casters): the hidden rule
// with the render claims lifted, the exclusions (the vehicle ride's among
// them), the refNum-walk links, the local priority, the owner's lighting and
// the radii.
void test_frame_casters_from_facts() {
	std::vector<SlotCasterFacts> facts;
	facts.reserve(32);
	const auto fact = [&](uint64_t id) -> SlotCasterFacts & {
		facts.emplace_back();
		SlotCasterFacts &f = facts.back();
		f.id = id;
		f.position = {float(id), 2.0f, -float(id)};
		f.model_sphere_radius = 1.5f;
		f.entity_bound_radius = 2.0f;
		return f;
	};
	fact(1);                                       // plain, shown
	fact(2).visibility.present_visible = false;    // unclaimed sim hide: the draw hidden too
	facts.back().visibility.draw_visible = false;
	fact(3).visibility.render_claimed = true;      // occluded / sub-pixel, sim shown
	facts.back().visibility.draw_visible = false;
	fact(4).visibility.render_claimed = true;      // claimed and sim hidden
	facts.back().visibility.present_visible = false;
	facts.back().visibility.draw_visible = false;
	fact(5).visibility.draw_visible = false;       // a direct hide of an attached draw
	fact(6).visibility.scene_hidden = true;        // the editor hid its layer
	fact(7).capture_with = 1;                      // a held weapon in 1's slot
	fact(8).parented_under_caster = true;          // a seated rider
	fact(9).ride_bits = kSlotRideVehicle | kSlotRideGroundVehicle;  // a vehicle on a vehicle, not in its group
	fact(10);                                      // the local player
	fact(11);                                      // the vehicle it rides
	SlotCasterFacts &weapon = fact(12);            // an attached draw
	weapon.lighting = {false, 0, 0};
	weapon.has_light_owner = true;
	weapon.owner_lighting = {true, 77, 3};
	SlotCasterFacts &unstamped = fact(13);
	unstamped.model_sphere_radius = 0.0f;
	unstamped.fallback_radius = 0.2f;
	SlotCasterFacts &preview = fact(14);
	preview.model_sphere_radius = 0.0f;
	preview.fallback_radius = 3.0f;
	SlotCasterFacts &groupless = fact(15);
	groupless.lighting = {true, 0, 9};
	fact(16).ride_bits = kSlotRideVehicle;          // a vehicle on a non-vehicle (a building deck)
	// A gun in carrier 1's refNum group, standing on it, with no slot of its own.
	SlotCasterFacts &gun = fact(17);
	gun.slot_record = false;
	gun.ride_bits = kSlotRideGroundVehicle | kSlotRideGroundRefChild;
	gun.ground_id = 1;
	// A vehicle in carrier 1's group riding it, hidden by the sim: excluded, walked.
	SlotCasterFacts &jeep = fact(18);
	jeep.ride_bits = kSlotRideVehicle | kSlotRideGroundVehicle | kSlotRideGroundRefChild;
	jeep.ground_id = 1;
	jeep.visibility.present_visible = false;
	jeep.visibility.draw_visible = false;
	// In the group of a ground that is no vehicle: the walk is the vehicle carrier's.
	SlotCasterFacts &deck = fact(19);
	deck.ride_bits = kSlotRideGroundRefChild;
	deck.ground_id = 1;
	// A walk rider the embedder's scene hides, and one whose ground is not drawn.
	SlotCasterFacts &layer = fact(20);
	layer.ride_bits = kSlotRideGroundVehicle | kSlotRideGroundRefChild;
	layer.ground_id = 1;
	layer.visibility.scene_hidden = true;
	fact(21).ride_bits = kSlotRideGroundVehicle | kSlotRideGroundRefChild;

	SlotFrameCasters casters;
	slot_frame_casters(facts, 10, 11, casters);
	CHECK(casters.casters.size() == facts.size());
	CHECK(!casters.state_for(1).hidden);
	CHECK(casters.state_for(2).hidden);
	CHECK(!casters.state_for(3).hidden);  // the slot reads no render gate
	CHECK(casters.state_for(4).hidden);
	CHECK(casters.state_for(5).hidden);
	CHECK(casters.state_for(6).hidden);
	CHECK(casters.state_for(7).seat_parented && casters.state_for(8).seat_parented);
	CHECK(!casters.state_for(1).seat_parented);
	CHECK(casters.capture_links.size() == 1 && casters.capture_links[0].first == 7 &&
			casters.capture_links[0].second == 1);
	// The vehicle ride: a vehicle whose ground is a vehicle, in its carrier's
	// group or not [orig: RenderSlot_SortAndAssign @ 0x5d6604..0x5d6627].
	CHECK(casters.state_for(9).on_vehicle && casters.state_for(18).on_vehicle);
	CHECK(!casters.state_for(16).on_vehicle && !casters.state_for(17).on_vehicle);
	CHECK(!casters.state_for(1).on_vehicle);
	// The refNum walk links the vehicle carrier's group riders, the sim-hidden
	// one too [orig: RenderSlot_RenderEntityAndChildren @ 0x5d796a..0x5d79d6].
	CHECK(casters.ride_links.size() == 2 && casters.ride_links[0].first == 17 &&
			casters.ride_links[0].second == 1 && casters.ride_links[1].first == 18 &&
			casters.ride_links[1].second == 1);
	// Every fact is a caster; only a slot record registers.
	CHECK(casters.find(17) != nullptr && casters.ids.size() == facts.size());
	CHECK(casters.slot_ids.size() == facts.size() - 1);
	bool gun_registered = false;
	for (const uint64_t id : casters.slot_ids) {
		gun_registered = gun_registered || id == 17;
	}
	CHECK(!gun_registered);
	CHECK(casters.state_for(10).is_local_player_or_parent);
	CHECK(casters.state_for(11).is_local_player_or_parent);
	CHECK(!casters.state_for(1).is_local_player_or_parent);
	CHECK(casters.state_for(99).hidden && casters.find(99) == nullptr);

	// The attached draw reads its owner's lighting, and its query the owner's group.
	CHECK(casters.state_for(12).interior);
	const SlotCasterLightQuery *query = casters.light_query(12);
	CHECK(query != nullptr && query->interior_bms == 77 && query->interior_section == 3);
	CHECK(query != nullptr && near_f(query->bound_radius, 2.0f) && near_f(query->position[0], 12.0f));
	// No group, no section.
	const SlotCasterLightQuery *groupless_query = casters.light_query(15);
	CHECK(groupless_query != nullptr && groupless_query->interior_section == 0);
	CHECK(casters.state_for(15).interior);

	// The radii: stamped, the 0.5 u floor of the fallback, the fallback itself.
	const SlotFrameCaster *plain = casters.find(1);
	CHECK(plain != nullptr && near_f(plain->capture_radius, 1.5f) &&
			near_f(plain->state.bound_radius, 2.0f));
	CHECK(plain != nullptr && near_f(plain->state.pos2d[0], 1.0f) &&
			near_f(plain->state.pos2d[1], -1.0f));
	const SlotFrameCaster *floor = casters.find(13);
	CHECK(floor != nullptr && near_f(floor->capture_radius, 0.5f) &&
			near_f(floor->state.bound_radius, 0.5f));
	const SlotFrameCaster *bounds = casters.find(14);
	CHECK(bounds != nullptr && near_f(bounds->capture_radius, 3.0f) &&
			near_f(bounds->state.bound_radius, 3.0f));

	// A frame with no local parent never matches id 0.
	SlotFrameCasters none;
	std::vector<SlotCasterFacts> zero(1);
	slot_frame_casters(zero, 10, 0, none);
	CHECK(!none.state_for(0).is_local_player_or_parent);
}

int main() {
	test_frame_casters_from_facts();
	SlotFrameInput input;
	input.sun_direction = slot_projection_direction({0.6f, 0.8f, 0.0f});
	input.sun_rgb = {0.9f, 0.8f, 0.7f};
	input.sky_rgb = {0.3f, 0.3f, 0.4f};
	input.camera_position = {0.0f, 2.0f, 0.0f};
	input.frame = 4;
	input.shadow_detail = 2;
	input.local_id = 3;
	input.local_first_person = true;
	input.local_prone = true;

	std::map<uint64_t, SlotFrameCaster> casters;
	casters[1] = caster_at(10.0f, 2.0f, 0.0f, 1.0f, 2.0f);   // sun-lit, near
	casters[2] = caster_at(100.0f, 0.0f, 0.0f, 1.0f, 1.0f);  // past the 80 u cull
	casters[3] = caster_at(5.0f, 0.0f, 5.0f, 1.0f, 1.0f);    // the local player, prone
	casters[3].state.is_local_player_or_parent = true;
	casters[4] = caster_at(0.0f, 0.0f, 30.0f, 4.0f, 3.0f);   // a point light wins
	casters[4].state.is_person = true;
	casters[6] = caster_at(1.0f, 0.0f, 1.0f, 1.0f, 1.0f);    // excluded this frame
	casters[8] = caster_at(2.0f, 0.0f, 2.0f, 1.0f, 1.0f);    // bound, no RT
	const SlotFrameCasterLookup lookup = [&](uint64_t id) -> const SlotFrameCaster * {
		const auto found = casters.find(id);
		return found != casters.end() ? &found->second : nullptr;
	};

	SlotPointLight lamp;
	lamp.position = {4.0f, 2.0f, 30.0f};
	lamp.color = {1.0f, 1.0f, 1.0f};
	lamp.attenuation = {1.0f, 0.0f, 0.01f, 0.0f};
	lamp.range = 10.0f;
	lamp.handle = 77;
	const std::vector<SlotPointLight> lamps{lamp};
	const std::vector<SlotPointLight> none;
	const SlotFrameLightQuery lights = [&](uint64_t id) -> const std::vector<SlotPointLight> & {
		return id == 4 ? lamps : none;
	};
	const std::function<float(float, float)> flat = [](float, float) { return 0.0f; };

	// A held weapon (5) linked capture-with caster 1; it has no slot of its own.
	const std::vector<std::pair<uint64_t, uint64_t>> links{{5, 1}, {9, 2}};
	// Riders 10 and 11 in carrier 1's refNum walk; 12 rides the excluded 6.
	const std::vector<std::pair<uint64_t, uint64_t>> rides{{10, 1}, {12, 6}, {11, 1}};
	std::vector<SlotAssignment> assignments{assignment(1, 0, 0), assignment(2, 1, 1),
			assignment(3, 2, 2), assignment(4, 3, 3), assignment(6, 4, 4, true, true),
			assignment(7, 5, 5), assignment(8, 6, -1, false)};
	assignments[1].capture_dirty = true;

	SlotFrame frame;
	slot_frame(input, assignments, lookup, links, rides, lights, flat, frame);

	// Bound: every found assignment holding a patch and not excluded (1, 2, 3,
	// 4, 8; 7 is gone, 6 excluded). Slots: the four that own a silhouette RT.
	CHECK(frame.bound == 5);
	CHECK(frame.slots.size() == 4);
	CHECK(frame.captures == 2);  // 1 and 4 drape; 2 is culled, 3 gated
	CHECK(find_slot(frame, 6) == nullptr && find_slot(frame, 7) == nullptr &&
			find_slot(frame, 8) == nullptr);

	const std::array<float, 3> sun_raw = input.sun_direction;
	const float sun_len = std::sqrt(sun_raw[0] * sun_raw[0] + sun_raw[1] * sun_raw[1] +
			sun_raw[2] * sun_raw[2]);
	const std::array<float, 3> sun_unit{sun_raw[0] / sun_len, sun_raw[1] / sun_len,
			sun_raw[2] / sun_len};

	// --- the sun-lit slot.
	{
		const SlotFrameSlot *slot = find_slot(frame, 1);
		CHECK(slot != nullptr);
		if (slot != nullptr) {
			CHECK(slot->capture.order == 0);
			CHECK(slot->capture.size == slot_texture_size(0, 2));
			// frame 4 & 3 == record 0 & 3: the cadence arms it.
			CHECK(slot->capture.armed);
			// The model sphere sizes the capture: min(2 x 1.25, 2 + 0.75).
			CHECK(near_f(slot->capture.half_extent, 2.5f));
			CHECK(same3(slot->capture.center, {10.0f, 2.0f, 0.0f}));
			const SlotCaptureViewAxes axes = slot_capture_view_axes(sun_unit);
			CHECK(same3(slot->capture.axes.x, axes.x) && same3(slot->capture.axes.z, axes.z));
			// The caster, its capture-with child, then its refNum walk.
			CHECK(slot->capture.casters.size() == 4 && slot->capture.casters[0] == 1 &&
					slot->capture.casters[1] == 5 && slot->capture.casters[2] == 10 &&
					slot->capture.casters[3] == 11);
			CHECK(slot->capture.ride_walk_begin == 2);
			CHECK(slot->drape.drawn);
			// The sun leg reads the RAW stored vertical (-0.8), not the unit one.
			const std::array<float, 3> ambient =
					drape_sun_ambient(input.sun_rgb, input.sky_rgb, sun_raw[1], 0.0f);
			CHECK(same3(slot->drape.ambient, ambient));
			CHECK(slot->drape.light.range == 0.0f);
			// The bound radius sizes the lod: (0.5 + |0.5 / -0.8|) x 6 = 6.75 -> 6.
			CHECK(slot->drape.lod == 6);
			// The march from (10, 2, 0) along (-0.6, -0.8, 0) over flat ground:
			// unit planar steps of -x with -4/3 of drop, two steps to the ground.
			CHECK(slot->drape.patch_drawn);
			const SlotPatch patch = slot_patch_bounds(8.0f, 0.0f, 6);
			CHECK(slot->drape.patch.min_x == patch.min_x && slot->drape.patch.max_x == patch.max_x &&
					slot->drape.patch.min_north == patch.min_north &&
					slot->drape.patch.max_north == patch.max_north);
			const SlotDepthClip clip = slot_depth_clip(sun_raw, 2.5f, false, {10.0f, 2.0f, 0.0f});
			CHECK(same3(slot->drape.clip.u_axis, clip.u_axis) &&
					slot->drape.clip.u_offset == clip.u_offset &&
					same3(slot->drape.clip.v_axis, clip.v_axis));
		}
	}

	// --- past 80 u: captured (a dirty order arms off-cadence), never draped.
	{
		const SlotFrameSlot *slot = find_slot(frame, 2);
		CHECK(slot != nullptr);
		if (slot != nullptr) {
			CHECK(slot->capture.armed);  // record 1 is off-phase; the dirty bit arms it
			CHECK(slot->capture.casters.size() == 2 && slot->capture.casters[1] == 9);
			CHECK(slot->capture.ride_walk_begin == 2);  // no riders: the walk is empty
			CHECK(!slot->drape.drawn && !slot->drape.patch_drawn);
		}
	}

	// --- the local first-person player, prone: captured, its own drape gated.
	{
		const SlotFrameSlot *slot = find_slot(frame, 3);
		CHECK(slot != nullptr);
		if (slot != nullptr) {
			CHECK(!slot->capture.armed);  // record 2 off-phase, mask 3 below detail 3
			CHECK(!slot->drape.drawn);
		}
	}

	// --- the attached-light slot.
	{
		const SlotFrameSlot *slot = find_slot(frame, 4);
		CHECK(slot != nullptr);
		if (slot != nullptr) {
			// normalize(entity - light) = (-4, -2, 0) / sqrt(20).
			const float len = std::sqrt(20.0f);
			const std::array<float, 3> unit{-4.0f / len, -2.0f / len, 0.0f};
			const SlotCaptureViewAxes axes = slot_capture_view_axes(unit);
			CHECK(near_f(slot->capture.axes.z[0], axes.z[0]) &&
					near_f(slot->capture.axes.z[1], axes.z[1]) &&
					near_f(slot->capture.axes.x[2], axes.x[2]));
			CHECK(slot->drape.drawn);
			CHECK(same3(slot->drape.ambient, {1.0f, 1.0f, 1.0f}));
			const float distance = std::sqrt(30.0f * 30.0f + 2.0f * 2.0f);
			const SlotDrapeLight light = drape_attached_light(lamp, drape_fade(distance));
			CHECK(same3(slot->drape.light.position, lamp.position));
			CHECK(slot->drape.light.range == 10.0f);
			CHECK(same3(slot->drape.light.diffuse, light.diffuse));
			// The stored vertical is the unit light direction's: (0.5 + 0.5 /
			// 0.447) x 9 (bound 4) = 14.56 -> 14.
			CHECK(slot->drape.lod == 14);
			// The person class steepens its depth clip.
			const SlotDepthClip clip = slot_depth_clip(
					unit, silhouette_half_extent(3.0f), true, {0.0f, 0.0f, 30.0f});
			CHECK(near_f(slot->drape.clip.u_axis[1], clip.u_axis[1]) &&
					near_f(slot->drape.clip.v_offset, clip.v_offset, 1.0e-4f));
		}
	}

	// --- a closed terrain pass keeps the captures and the terms, no patches;
	// no terrain keeps the anchor under the march start; the output vectors
	// are refilled in place.
	{
		input.terrain_pass_drawn = false;
		slot_frame(input, assignments, lookup, links, rides, lights, flat, frame);
		CHECK(frame.slots.size() == 4 && frame.captures == 2);
		for (const SlotFrameSlot &slot : frame.slots) {
			CHECK(!slot.drape.patch_drawn);
		}
		input.terrain_pass_drawn = true;
		casters[1].march_offset = {0.0f, 0.5f, -1.0f};
		slot_frame(input, assignments, lookup, links, {}, nullptr, nullptr, frame);
		const SlotFrameSlot *slot = find_slot(frame, 1);
		CHECK(slot != nullptr);
		if (slot != nullptr) {
			const SlotPatch patch = slot_patch_bounds(10.0f, 1.0f, 6);
			CHECK(slot->drape.patch.min_x == patch.min_x &&
					slot->drape.patch.max_north == patch.max_north);
		}
		// No light query: slot 4 takes the sun.
		const SlotFrameSlot *lit = find_slot(frame, 4);
		CHECK(lit != nullptr && lit->drape.light.range == 0.0f &&
				!same3(lit->drape.ambient, {1.0f, 1.0f, 1.0f}));
	}

	// --- a grazing sun the 0.25 clamp leaves non-unit: the sun leg's ambient,
	// the lod and the depth clip read the RAW stored direction
	// (-1, -0.25, -1), never its unit twin (y -0.174).
	{
		SlotFrameInput low = input;
		low.sun_direction = slot_projection_direction({1.0f, 0.0f, 1.0f});
		const std::array<float, 3> raw = low.sun_direction;
		const float len = std::sqrt(raw[0] * raw[0] + raw[1] * raw[1] + raw[2] * raw[2]);
		const float unit_y = raw[1] / len;
		CHECK(near_f(raw[1], -0.25f) && !near_f(unit_y, raw[1], 0.05f));
		std::map<uint64_t, SlotFrameCaster> one;
		one[1] = caster_at(10.0f, 2.0f, 0.0f, 1.0f, 2.0f);
		const SlotFrameCasterLookup only = [&](uint64_t id) -> const SlotFrameCaster * {
			const auto found = one.find(id);
			return found != one.end() ? &found->second : nullptr;
		};
		SlotFrame grazing;
		slot_frame(low, {assignment(1, 0, 0)}, only, {}, {}, nullptr, flat, grazing);
		CHECK(grazing.slots.size() == 1);
		if (grazing.slots.size() == 1) {
			const SlotFrameDrape &drape = grazing.slots[0].drape;
			CHECK(same3(drape.ambient, drape_sun_ambient(low.sun_rgb, low.sky_rgb, raw[1], 0.0f)));
			CHECK(!same3(drape.ambient, drape_sun_ambient(low.sun_rgb, low.sky_rgb, unit_y, 0.0f)));
			// (0.5 + 0.5 / 0.25) x 6 = 15; the unit vertical would give 20.
			CHECK(drape.lod == 15);
			const SlotDepthClip raw_clip = slot_depth_clip(raw, 2.5f, false, {10.0f, 2.0f, 0.0f});
			const SlotDepthClip unit_clip = slot_depth_clip(
					{raw[0] / len, raw[1] / len, raw[2] / len}, 2.5f, false, {10.0f, 2.0f, 0.0f});
			CHECK(drape.clip.u_offset == raw_clip.u_offset &&
					drape.clip.v_offset == raw_clip.v_offset);
			CHECK(!near_f(drape.clip.v_offset, unit_clip.v_offset, 1.0e-7f) ||
					!near_f(drape.clip.u_offset, unit_clip.u_offset, 1.0e-7f));
		}
	}

	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("render_slot_frame: ok\n");
	return 0;
}
