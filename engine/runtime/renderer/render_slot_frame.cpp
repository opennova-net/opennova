#include <runtime/renderer/render_slot_frame.h>

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

void slot_frame(const SlotFrameInput &input, const std::vector<SlotAssignment> &assignments,
		const SlotFrameCasterLookup &caster_for,
		const std::vector<std::pair<uint64_t, uint64_t>> &capture_links,
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
		// The retail child walk: models linked capture-with an admitted caster
		// (held weapons, mounted children) render into the parent's slot
		// [orig: RenderSlot_RenderEntityAndChildren @ 0x5d78ef..0x5d79d6]. The
		// walk follows the entity hierarchy, not the slot table: a linked child
		// the full table refused (no row of its own) still rides its parent.
		capture.casters.clear();
		capture.casters.push_back(assignment.id);
		for (const std::pair<uint64_t, uint64_t> &link : capture_links) {
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
