#include <runtime/renderer/render_slot_frame.h>

#include <algorithm>
#include <cmath>

namespace opennova::renderer {
namespace {

using Vec3 = std::array<float, 3>;

// The presentation vector's normalize as the device computes it (the squared
// terms summed in order, one sqrt, three divides; zero stays zero), so the
// slot direction the capture axes, the march and the drape read is the one
// the embedder's own vectors carry.
Vec3 normalized(const Vec3 &v) {
	const float x2 = v[0] * v[0];
	const float y2 = v[1] * v[1];
	const float z2 = v[2] * v[2];
	const float length_sq = x2 + y2 + z2;
	if (length_sq == 0.0f) {
		return {0.0f, 0.0f, 0.0f};
	}
	const float length = std::sqrt(length_sq);
	return {v[0] / length, v[1] / length, v[2] / length};
}

float distance(const Vec3 &a, const Vec3 &b) {
	const float dx = a[0] - b[0];
	const float dy = a[1] - b[1];
	const float dz = a[2] - b[2];
	const float x2 = dx * dx;
	const float y2 = dy * dy;
	const float z2 = dz * dz;
	return std::sqrt(x2 + y2 + z2);
}

const SlotPointLight *find_light(const std::vector<SlotPointLight> &lights, uint32_t handle) {
	for (const SlotPointLight &light : lights) {
		if (light.handle == handle) {
			return &light;
		}
	}
	return nullptr;
}

}  // namespace

const SlotFrameCaster *SlotFrameCasters::find(uint64_t id) const {
	const auto it = index_.find(id);
	return it != index_.end() ? &casters[it->second] : nullptr;
}

const SlotCasterLightQuery *SlotFrameCasters::light_query(uint64_t id) const {
	const auto it = index_.find(id);
	return it != index_.end() ? &light_queries[it->second] : nullptr;
}

SlotCandidateState SlotFrameCasters::state_for(uint64_t id) const {
	if (const SlotFrameCaster *caster = find(id)) {
		return caster->state;
	}
	SlotCandidateState gone;
	gone.hidden = true;
	return gone;
}

void slot_frame_casters(const std::vector<SlotCasterFacts> &facts, uint64_t local_id,
		uint64_t local_parent_id, SlotFrameCasters &out) {
	out.ids.clear();
	out.slot_ids.clear();
	out.casters.clear();
	out.light_queries.clear();
	out.links.capture.clear();
	out.links.seat.clear();
	out.links.ride.clear();
	out.index_.clear();
	out.ids.reserve(facts.size());
	out.casters.reserve(facts.size());
	out.light_queries.reserve(facts.size());
	for (const SlotCasterFacts &fact : facts) {
		SlotFrameCaster caster;
		// The two radii: the model sphere sizes the capture extent and the
		// depth clip, the entity bound the lod, the patch and the light query
		// (RenderSlot_RenderEntityAndChildren @ 0x5d7835 reads the model's
		// +0x14, RenderSlot_AllocSlot @ 0x5d5773 entity+0).
		const bool stamped = fact.model_sphere_radius > 0.0f;
		const float fallback = std::max(0.5f, fact.fallback_radius);
		caster.capture_radius = stamped ? fact.model_sphere_radius : fallback;
		SlotCandidateState &state = caster.state;
		state.bound_radius = stamped ? fact.entity_bound_radius : fallback;
		state.pos2d = {fact.position[0], fact.position[2]};
		state.hidden = !slot_sort_shown(fact.visibility) || fact.visibility.scene_hidden;
		// A seated person takes no slot of its own, its carrier's capture
		// drawing it [orig: RenderSlot_SortAndAssign @ 0x5d6593..0x5d65ff]; nor
		// does a draw its owner's submission or scene parent carries.
		state.seat_parented = fact.parented_under_caster || fact.capture_with != 0 ||
				slot_seat_parented(fact.seat_bits);
		// A vehicle standing on a vehicle takes no slot of its own
		// [orig: RenderSlot_SortAndAssign @ 0x5d6604..0x5d6627]; the carrier's
		// refNum walk draws it into the carrier's (the ride links below).
		state.on_vehicle = slot_rides_vehicle(fact.ride_bits);
		state.is_local_player_or_parent =
				fact.id == local_id || (local_parent_id != 0 && fact.id == local_parent_id);
		const SlotCasterLighting &lighting =
				fact.has_light_owner ? fact.owner_lighting : fact.lighting;
		state.interior = lighting.interior;
		state.dynamic = true;
		state.is_person = fact.is_person;
		caster.position = fact.position;
		caster.march_offset = fact.march_offset;
		if (fact.capture_with != 0) {
			out.links.capture.push_back({fact.id, fact.capture_with});
		}
		// A pool-0 member rides its carrier's child walk, whatever slot it holds
		// and whatever its visibility (SlotFrameCapture::walk_begin); only the
		// embedder's own scene hiding drops it [orig: RenderSlot_RenderEntityAndChildren
		// @ 0x5d78ea..0x5d7968].
		if (fact.seat_walk_id != 0 && slot_seat_walk(fact.seat_bits) != SlotSeatWalk::kNone &&
				!fact.visibility.scene_hidden) {
			out.links.seat.push_back({fact.id, fact.seat_walk_id});
		}
		// A member of its vehicle ground entity's refNum group rides that
		// carrier's walk, whatever slot it holds and whatever its visibility
		// (SlotFrameCapture::walk_begin); only the embedder's own scene
		// hiding drops it [orig: RenderSlot_RenderEntityAndChildren
		// @ 0x5d796a..0x5d79d6].
		if (fact.ground_id != 0 && slot_ride_walk_child(fact.ride_bits) &&
				!fact.visibility.scene_hidden) {
			out.links.ride.push_back({fact.id, fact.ground_id});
		}
		SlotCasterLightQuery query;
		query.position = fact.position;
		query.bound_radius = state.bound_radius;
		query.interior_bms = lighting.interior_bms;
		query.interior_section = lighting.interior_bms != 0 ? lighting.interior_section : 0;
		out.index_[fact.id] = out.casters.size();
		out.ids.push_back(fact.id);
		if (fact.slot_record) {
			out.slot_ids.push_back(fact.id);
		}
		out.casters.push_back(caster);
		out.light_queries.push_back(query);
	}
}

void slot_frame(const SlotFrameInput &input, const std::vector<SlotAssignment> &assignments,
		const SlotFrameCasterLookup &caster_for, const SlotFrameLinks &links,
		const SlotFrameLightQuery &lights_for,
		const std::function<float(float, float)> &terrain_height, SlotFrame &out) {
	out.bound = 0;
	out.captures = 0;
	std::size_t count = 0;
	const Vec3 default_direction = normalized(input.sun_direction);
	static const std::vector<SlotPointLight> kNoLights;

	for (const SlotAssignment &assignment : assignments) {
		const SlotFrameCaster *caster = caster_for ? caster_for(assignment.id) : nullptr;
		if (caster == nullptr) {
			continue;
		}
		if (assignment.bound && !assignment.excluded) {
			++out.bound;
		}
		// Only a slot with a silhouette RT draws anything; the light pick, the
		// patch and the depth clip feed that drape alone (the authored-blob leg
		// a bound slot without one would take is dead in JO,
		// render_slot_shadow.h).
		if (!assignment.draws_silhouette || assignment.excluded) {
			continue;
		}
		if (count == out.slots.size()) {
			out.slots.emplace_back();
		}
		SlotFrameSlot &slot = out.slots[count++];
		slot.id = assignment.id;
		SlotFrameCapture &capture = slot.capture;
		SlotFrameDrape &drape = slot.drape;
		drape = SlotFrameDrape{};

		// The dominant-light pick: the clamped sun by default, the strongest
		// nearby point light overriding it (pick_dominant_light). Retail picks
		// once per entity update [orig: Entity_UpdateAllEntities
		// @ 0x4c25e0..0x4c2602 -> RenderSlot_UpdateEntityLight]; this frame picks
		// once per display frame, the cadence render-lighting-re.md accepts.
		const std::vector<SlotPointLight> &lights = lights_for ? lights_for(assignment.id) : kNoLights;
		const SlotLightPick pick = pick_dominant_light(caster->position, default_direction,
				lights.data(), lights.size(), caster->state.interior);
		const SlotPointLight *attached =
				pick.attached_handle != 0 ? find_light(lights, pick.attached_handle) : nullptr;
		const Vec3 direction = normalized(pick.direction);
		// The slot direction as stored: the RAW clamped-negated sun, or the
		// unit attached-light direction — what the grazing rescale's vertical
		// and the depth clip read [orig: slot+0x68..0x70, RenderSlot_Update-
		// EntityLight @ 0x5d6d5c; RenderSlot_DrawSilhouetteDrape @ 0x5d5d66].
		const float stored_y = pick.attached_handle != 0 ? direction[1] : input.sun_direction[1];
		const Vec3 &stored_direction = pick.attached_handle != 0 ? direction : input.sun_direction;

		// The capture: the rotation-only look-at along the slot direction,
		// sized from the MODEL sphere [orig: RenderSlot_RenderEntityAndChildren
		// @ 0x5d7835 — float24 = min(1.25 gpm[5], gpm[5] + 0.75)], on the
		// refresh cadence (slot_refresh_mask_for carries the local player's
		// every-frame exception).
		capture.order = assignment.capture_order;
		capture.size = slot_texture_size(assignment.capture_order, input.shadow_detail);
		capture.axes = slot_capture_view_axes(direction);
		capture.center = caster->position;
		capture.half_extent = silhouette_half_extent(caster->capture_radius);
		capture.armed = slot_refresh_due(assignment.record_index, input.frame,
				slot_refresh_mask_for(input.shadow_detail, caster->state.is_local_player_or_parent),
				assignment.capture_dirty);
		// The caster through its own render callback, with the draws its
		// submission carries: the models linked capture-with it (held weapons,
		// overlays, the virtual display) [orig: RenderSlot_RenderEntityAndChildren
		// @ 0x5d78e5]. The links follow the entity hierarchy, not the slot table:
		// a linked child the full table refused (no row of its own) still rides
		// its parent.
		capture.casters.clear();
		capture.casters.push_back(assignment.id);
		const auto children_of = [&](uint64_t parent) {
			for (const std::pair<uint64_t, uint64_t> &link : links.capture) {
				if (link.second == parent) {
					capture.casters.push_back(link.first);
				}
			}
		};
		children_of(assignment.id);
		// The pool-0 walk riders' submissions carry theirs as well (a seated
		// person's gun and overlays); the riders themselves follow below.
		for (const std::pair<uint64_t, uint64_t> &link : links.seat) {
			if (link.second == assignment.id) {
				children_of(link.first);
			}
		}
		// Then the pool-0 child walk: every pool-0 entity seated on this caster
		// (parentSlot 1/2/5), and every UseGun gunner whose refNum gun stands on
		// it [orig: RenderSlot_RenderEntityAndChildren @ 0x5d78ea..0x5d7968].
		capture.walk_begin = capture.casters.size();
		for (const std::pair<uint64_t, uint64_t> &link : links.seat) {
			if (link.second == assignment.id) {
				capture.casters.push_back(link.first);
			}
		}
		// Then, for a vehicle carrier, its refNum walk: every member of its
		// group list standing on it, under camera mode 1 [orig:
		// RenderSlot_RenderEntityAndChildren @ 0x5d796a..0x5d79dc]. A ride link
		// exists only for a rider whose ground is ItemDef type 1, the carrier
		// slot's own bit 0 the walk tests (slot_ride_walk_child).
		for (const std::pair<uint64_t, uint64_t> &link : links.ride) {
			if (link.second == assignment.id) {
				capture.casters.push_back(link.first);
			}
		}

		// The local player's first-person drape gate.
		if (assignment.id == input.local_id &&
				local_first_person_drape_skipped(input.local_first_person, input.local_prone,
						input.shadow_detail)) {
			continue;
		}
		// The drape distance: the entity-to-camera 3D distance, taken once per
		// slot. At >= 80 u the drape is skipped outright; inside it the one fade
		// scales the whole patch's material ambient [orig: RenderSlot_Draw-
		// SilhouetteDrape @ 0x5d5cc4..0x5d5d59]. The capture is unaffected.
		const float camera_distance = distance(caster->position, input.camera_position);
		if (drape_culled(camera_distance)) {
			continue;
		}
		const float fade = drape_fade(camera_distance);
		drape.drawn = true;
		if (pick.attached_handle != 0) {
			// The attached-light leg lights the patch with D3D light 4 over a
			// white material under a white D3DRS_AMBIENT (drape_attached_light);
			// a handle the fill cannot read leaves the white material alone.
			drape.ambient = {1.0f, 1.0f, 1.0f};
			if (attached != nullptr) {
				drape.light = drape_attached_light(*attached, fade);
			}
		} else {
			// The sun leg reads the STORED slot vertical — the raw
			// clamped-negated sun, not its normalized twin [orig: fabs of
			// slot+0x6C @ 0x5d5f63].
			drape.ambient = drape_sun_ambient(input.sun_rgb, input.sky_rgb, stored_y, fade);
		}
		// The patch around the marched anchor, from the slot direction, at the
		// grazing-rescaled lod; the march starts at the entity's march start
		// (slot_march_start_offset over its position).
		drape.lod = grazing_slot_lod(slot_lod_for_radius(caster->state.bound_radius), stored_y);
		if (input.terrain_pass_drawn) {
			const Vec3 start{caster->position[0] + caster->march_offset[0],
					caster->position[1] + caster->march_offset[1],
					caster->position[2] + caster->march_offset[2]};
			std::array<float, 2> anchor{start[0], start[2]};
			if (terrain_height) {
				// A sub-quantum gap over the ground is contact (slot_march_start_height).
				const float start_y = slot_march_start_height(start[1],
						terrain_height(start[0], start[2]), kSlotTerrainHeightQuantumUnits);
				anchor = march_shadow_anchor({start[0], start_y, start[2]}, direction, terrain_height);
			}
			// Planar mission north is presentation -z.
			drape.patch = slot_patch_bounds(anchor[0], -anchor[1], drape.lod);
			drape.patch_drawn = true;
		}
		drape.clip = slot_depth_clip(stored_direction, capture.half_extent,
				caster->state.is_person, caster->position);
		++out.captures;
	}
	out.slots.resize(count);
}

}  // namespace opennova::renderer
