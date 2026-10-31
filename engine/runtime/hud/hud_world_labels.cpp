#include <runtime/hud/hud_world_labels.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <runtime/hud/hud_config_tokens.h> // friendly_tag_anchor
#include <runtime/world/geom.h> // to_fixed
#include <runtime/world/local_player.h>

namespace opennova::hud {

namespace {

std::array<int32_t, 3> fixed_point(const world::Vec3 &v) {
	return { world::to_fixed(v.x), world::to_fixed(v.y), world::to_fixed(v.z) };
}

// |anchor - eye| as the label pass measures it: the 32-bit differences squared and summed (exact
// in the x87's 64-bit mantissa), the square root held at 2147418112.0 (flt_7C19E0, stored as
// 0x4EFFFE00), truncated: the integer square root's floor, capped.
// [orig: HUD_DrawEntityLabel @0x5A3A9C..0x5A3AFB - the first difference's fild @0x5a3aba]
int32_t label_distance_q16(const std::array<int32_t, 3> &anchor,
		const std::array<int32_t, 3> &eye) {
	uint64_t n = 0;
	for (size_t i = 0; i < 3; ++i) {
		const int64_t d = static_cast<int32_t>(
				static_cast<uint32_t>(anchor[i]) - static_cast<uint32_t>(eye[i]));
		n += static_cast<uint64_t>(d * d);
	}
	uint64_t r = static_cast<uint64_t>(std::sqrt(static_cast<double>(n)));
	while (r > 0 && r * r > n) --r;
	while ((r + 1) * (r + 1) <= n) ++r;
	return static_cast<int32_t>(std::min<uint64_t>(r, 0x7FFF0000u));
}

} // namespace

void project_attach_labels(const std::vector<world::AttachLabel> &labels,
		const HudViewport &viewport, const GameTextLookup &gametext,
		std::vector<HudAttachLabel> &out) {
	out.reserve(out.size() + labels.size());
	for (const world::AttachLabel &l : labels) {
		const HudProjectedPoint point = hud_project_world_point(viewport, fixed_point(l.world_pos));
		if (!point.valid || point.clip != 0) continue;
		HudAttachLabel label;
		label.screen_x = point.x;
		label.screen_y = point.y;
		label.text = attach_label_text(gametext, l.type, l.attach_text_key, l.armory_delay_seconds);
		label.nearest = l.nearest;
		out.push_back(std::move(label));
	}
}

void fill_attach_labels(world::LocalPlayer &local, const world::VehicleOccupancySource *occupancy,
		const HudViewport &viewport, const GameTextLookup &gametext,
		std::vector<HudAttachLabel> &out) {
	out.clear();
	std::vector<world::AttachLabel> labels;
	local.collect_attach_labels(labels, occupancy);
	project_attach_labels(labels, viewport, gametext, out);
}

void project_friendly_tags(const std::vector<world::FriendlyTagSource> &tags,
		const HudViewport &viewport, std::vector<HudFriendlyTag> &out) {
	out.reserve(out.size() + tags.size());
	for (const world::FriendlyTagSource &t : tags) {
		const std::array<int32_t, 3> anchor =
				friendly_tag_anchor(fixed_point(t.position), t.eye_offset_z);
		const HudProjectedPoint point = hud_project_world_point(viewport, anchor);
		HudFriendlyTag tag;
		// A clipped anchor bails in the compiler, unless the death screen pins it
		// (HudFriendlyTag::projected) [orig: HUD_DrawEntityLabel @0x5a3b7e..0x5a3b80].
		tag.projected = point.valid && point.clip == 0;
		tag.screen_x = point.x;
		tag.screen_y = point.y;
		tag.dist_q16 = label_distance_q16(anchor, viewport.view_pos_q16);
		tag.name = t.name;
		tag.entity_id = t.fallback_name_id;
		tag.handle = static_cast<uint16_t>(t.entity.packed);
		tag.team = t.team;
		tag.health_ratio_fp16 = t.health_ratio_fp16;
		tag.medic = t.medic;
		tag.speaking = t.speaking;
		tag.player = t.player;
		// The downed legs (D-HUD-20 residue a): the compiler's recolor / count.
		tag.dead = t.dead;
		tag.has_slot = t.has_slot;
		tag.medic_request = t.medic_request;
		tag.revive_seconds = t.revive_seconds;
		// The radio-request icon's per-tag fold (the gather carries the witness); the viewer
		// gate is HudFrameState::radio_request_icon_viewer.
		tag.radio_request = t.radio_request;
		tag.squad_color_index = t.squad_color_index;
		// DFX2-87's reach facts: the HUD target cursor's entity and the slot +0x3C word's match
		// (the gather carries the witness).
		tag.cursor_target = t.cursor_target;
		tag.same_account = t.same_account;
		out.push_back(std::move(tag));
	}
}

} // namespace opennova::hud
