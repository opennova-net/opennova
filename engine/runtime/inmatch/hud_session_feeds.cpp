#include "hud_session_feeds.h"

#include "minimap_overlays.h"

#include <base/io/cp1252.h>
#include <runtime/mission/mission_kernel.h>
#include <runtime/world/world.h>

#include <vector>

namespace opennova::inmatch {

HudSessionRole hud_session_role(RoleKind kind, const RoleView &view) {
	HudSessionRole role;
	role.in_session = view.kernel != nullptr && view.kernel->world.rules.mp_session;
	role.authority = kind != RoleKind::Joiner;
	role.mp_session_peer = kind != RoleKind::DedicatedHost;
	role.round_over = view.kernel != nullptr && view.kernel->world.match.outcome().ended;
	return role;
}

hud::HudMissionEffectRecord hud_mission_effect_record(const world::Effect &effect) {
	hud::HudMissionEffectRecord record;
	record.kind = hud::hud_mission_effect(effect.kind);
	record.a = effect.a;
	record.b = effect.b;
	record.c = effect.c;
	// The effect's text as the HUD's rings store it: retail's single-byte text (a UTF-8 string
	// encoded, any other bytes kept as they are).
	record.text = utf8_to_retail_text(effect.str);
	return record;
}

uint32_t hud_session_escape(hud::HudSession &session, const HudSessionRole &role) {
	hud::HudEscapeInput input;
	input.in_session = role.in_session;
	input.round_over = role.round_over;
	input.authority = role.authority;
	return hud::hud_toggles_escape(session.toggles(), input);
}

// The type-2043 marker the overlay init's pool scan finds [orig: HUD_InitOverlaySystem @0x5a4999].
void hud_session_grid_origin_feed(hud::HudSession &session, const RoleView &view,
		int64_t now_ticks, bool force, hud::HudFrameState &state) {
	if (!force && !session.map_grid_origin_due(now_ticks)) return;
	const hud::HudMapGridOrigin origin = hud_map_grid_origin(view);
	// A forced query stamps the block whatever it finds; a retry only the origin it found.
	if (!force && !origin.present) return;
	session.set_map_grid_origin_present(origin.present);
	state.minimap.grid_origin_present = origin.present;
	state.minimap.grid_origin_x = origin.present ? origin.x_q16 : 0;
	state.minimap.grid_origin_y = origin.present ? origin.y_q16 : 0;
}

void hud_session_footprints_feed(hud::HudSession &session, const RoleView &view,
		int64_t now_ticks, hud::HudFrameState &state) {
	if (!session.map_footprints_due(now_ticks) || view.kernel == nullptr) return;
	std::vector<hud::HudMinimapFootprint> footprints;
	build_minimap_footprints(view.kernel->world, view.kernel->occlusion, footprints);
	if (footprints.empty()) return;
	session.set_map_footprints_fed();
	state.map_footprints = std::move(footprints);
}

} // namespace opennova::inmatch
