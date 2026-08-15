#include "world/minimap_overlay.h"

#include "world/entity.h"

#include <algorithm>

namespace opennova::world {

namespace {

// The cell-9 x1.2 scale rides the tail of every icon branch.
// [orig: draw_minimap_blip @0x597d5a..0x597d64]
MinimapBlipDrawPolicy finish_blip_policy(MinimapBlipDrawPolicy policy,
		uint8_t icon) {
	if (icon == 9) {
		policy.half_x_q16 = static_cast<int32_t>(
				static_cast<int64_t>(policy.half_x_q16) * 6 / 5);
		policy.half_y_q16 = static_cast<int32_t>(
				static_cast<int64_t>(policy.half_y_q16) * 6 / 5);
	}
	return policy;
}

} // namespace

uint8_t minimap_team_color(uint8_t team) {
	// Blue / Red / neutral color-table indices.
	// [orig: Entity_ClassifyForMinimap @0x50FA70]
	if (team == 1) return 0x0A;
	return team == 2 ? 0x09 : 0x0C;
}

MinimapOverlayClassification classify_minimap_overlay(const Entity &entity) {
	MinimapOverlayClassification out;
	out.handle = entity.handle.packed;
	out.flags = ((entity.flags | entity.engine_flags) & kEntityFlagDead) != 0
			? 1 : 0;
	out.source = entity.zone_number; // entity+538 / BMS lfp_group
	if (!entity.has_item_def || (entity.item_attrib & kItemAttribNoHud) != 0)
		return out;

	out.color = minimap_team_color(entity.team);
	const bool dead = out.flags != 0;
	if ((entity.item_attrib & kItemAttribChangeTeam) != 0) {
		out.visible = true;
		return out;
	}
	// ItemDefAttrib2 FARP. VehicleBay's groupFlags-dependent 19..22 selector
	// remains definition-data-gated and is deliberately not approximated.
	if ((entity.item_attrib2 & 0x00002000u) != 0) {
		out.icon = 5;
		out.visible = true;
		return out;
	}
	if (entity.item_unit_type == 11 && !dead) {
		out.icon = 9;
		out.visible = true;
		return out;
	}
	if ((entity.item_attrib & kItemAttribArmory) != 0) {
		out.icon = 13;
		out.visible = true;
		return out;
	}
	if (entity.item_type == 5) { // Building
		if (!entity.has_minimap_model_marker) return out;
		if (entity.team != 1 && entity.team != 2) out.color = 0;
		out.visible = true;
		return out;
	}
	if (entity.item_type == 1 && !dead) { // Vehicle
		out.icon = 10;
		if (entity.item_unit_type >= 5 && entity.item_unit_type <= 8)
			out.icon = 15;
		else if (entity.item_unit_type == 3 || entity.item_unit_type == 4)
			out.icon = 11;
		else if (entity.item_unit_type == 12)
			out.icon = 25;
		out.visible = true;
		return out;
	}
	// Raw attrib 0x8000 is a witnessed generic live-marker branch; its token
	// name is not yet recovered.
	if ((entity.item_attrib & 0x00008000u) != 0 && !dead) {
		out.visible = true;
		return out;
	}
	if ((entity.item_attrib & kItemAttribEweap) != 0 && !dead) {
		out.icon = (entity.item_id == 1869 || entity.item_id == 1886) ? 12 : 4;
		out.color = 8;
		out.visible = true;
		return out;
	}
	if (entity.item_type == 3) { // Person
		out.icon = dead ? 8 : 3;
		out.visible = true;
		return out;
	}
	if ((entity.item_attrib & kItemAttribSpawnPoint) != 0)
		out.visible = true;
	return out;
}

bool minimap_overlay_entity_enabled(const Entity &entity) {
	// entity+36 bit 0 is hidden/disabled. Dead remains classifier-visible.
	return entity.has_item_def && ((entity.flags | entity.engine_flags) & 1u) == 0;
}

MinimapBlipDrawPolicy minimap_blip_draw_policy(const Entity &entity,
		uint8_t icon) {
	MinimapBlipDrawPolicy out;
	// Model halves when stamped, the 10-wu class fallback otherwise.
	// [orig: @0x5979a2..0x5979cb]
	const bool has_model_halves =
			entity.minimap_half_x_q16 > 0 || entity.minimap_half_y_q16 > 0;
	if (has_model_halves) {
		out.half_x_q16 = entity.minimap_half_x_q16;
		out.half_y_q16 = entity.minimap_half_y_q16;
	}
	const bool dead =
			((entity.flags | entity.engine_flags) & kEntityFlagDead) != 0;
	if (!entity.has_item_def) {
		// No def: the default rotated form on the fallback size.
		return finish_blip_policy(out, icon);
	}
	if ((entity.item_attrib & kItemAttribArmory) != 0) {
		// The armory badge draws UPRIGHT at 4 wu. [orig: @0x5979e4..0x597a0d]
		out.half_x_q16 = out.half_y_q16 = 0x40000;
		out.rotate = false;
		return finish_blip_policy(out, icon);
	}
	if ((entity.item_attrib2 & 0x00002000u) != 0) {
		// The attrib2-0x2000 (FARP) class: 4 wu, rotated.
		// [orig: @0x597a26..0x597a5d; the zone-visibility mask gate
		//  (1<<zone) & dword_A85BBC is an MP-slot residual]
		out.half_x_q16 = out.half_y_q16 = 0x40000;
		return finish_blip_policy(out, icon);
	}
	if (entity.item_type == 5 && (entity.item_attrib & kItemAttribSpawnPoint) == 0 &&
			(entity.item_attrib2 & 1u) == 0) {
		// Building with the marker model: the icon path is skipped entirely —
		// the OOBJ occlusion ground-slice footprint draws instead.
		// [orig: @0x597a84..0x597b3f -> render_collision_wireframe @0x596800]
		if (entity.has_minimap_model_marker) {
			out.footprint = true;
			out.rotate = false;
			return out;
		}
		return finish_blip_policy(out, icon);
	}
	if (entity.item_type == 3) {
		// Person: 2 wu; the dead form is the UPRIGHT cell-8 body.
		// [orig: @0x597b43..0x597b72]
		out.half_x_q16 = out.half_y_q16 = 0x20000;
		if (dead) out.rotate = false;
		return finish_blip_policy(out, icon);
	}
	if ((entity.item_attrib & kItemAttribEweap) != 0 && entity.item_type != 1) {
		// Non-vehicle emplacements: 4 wu, floor 4, UPRIGHT.
		// [orig: @0x597b87..0x597ba2]
		out.half_x_q16 = out.half_y_q16 = 0x40000;
		out.floor_px = 4;
		out.rotate = false;
		return finish_blip_policy(out, icon);
	}
	if ((entity.item_attrib & 2u) != 0) {
		// The attrib-bit1 class: 2 wu, floor 4, rotated (the entity+692
		// availability gate and the sub_5977F0 zone color are residuals).
		// [orig: @0x597bac..0x597bf9]
		out.half_x_q16 = out.half_y_q16 = 0x20000;
		out.floor_px = 4;
		return finish_blip_policy(out, icon);
	}
	if (icon == 6 || icon == 2) {
		// Cells 6/2 draw UPRIGHT at 4 wu; cell 6 floors at 12 px.
		// [orig: @0x597c06..0x597c9b]
		out.half_x_q16 = out.half_y_q16 = 0x40000;
		out.floor_px = icon == 6 ? 12 : 6;
		out.rotate = false;
		return finish_blip_policy(out, icon);
	}
	if ((entity.item_attrib & kItemAttribSpawnPoint) != 0 &&
			entity.item_type != 1) {
		// Spawn points: min(model half, 4 wu) uniform, floor 16, rotated.
		// [orig: @0x597c13..0x597c36]
		const int32_t model_min =
				std::min(out.half_x_q16, out.half_y_q16);
		const int32_t half = std::min<int32_t>(model_min, 0x40000);
		out.half_x_q16 = out.half_y_q16 = half > 0 ? half : 0x40000;
		out.floor_px = 16;
		return finish_blip_policy(out, icon);
	}
	if ((entity.item_attrib2 & 1u) != 0) {
		// The attrib2-bit0 class: 8 wu, floor 8, UPRIGHT.
		// [orig: @0x597c5a..0x597c75]
		out.half_x_q16 = out.half_y_q16 = 0x80000;
		out.floor_px = 8;
		out.rotate = false;
		return finish_blip_policy(out, icon);
	}
	// Default: model halves (or the 10-wu fallback), floor 6, rotated.
	return finish_blip_policy(out, icon);
}

} // namespace opennova::world
