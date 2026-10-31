#include <runtime/inmatch/hud_frame_feeds.h>

#include <cstdint>
#include <vector>

#include <base/io/fixed.h> // float_to_fp16_16_sat
#include <runtime/hud/hud_exp_points.h> // hud_exp_points_text
#include <runtime/hud/hud_game_text.h> // hud_session_text / hud_vehicle_rider_text
#include <runtime/hud/hud_lfp_panel.h> // lfp_under_attack_text / lfp_ready_text
#include <runtime/hud/hud_math.h> // FriendlyTagMode
#include <runtime/hud/hud_takeover_status.h> // compose_takeover_status
#include <runtime/hud/hud_world_labels.h> // project_friendly_tags
#include <runtime/inmatch/takeover_status_feed.h> // collect_takeover_status
#include <runtime/mission/mission_kernel.h>
#include <runtime/world/friendly_tags.h>

namespace opennova::inmatch {

void hud_lfp_panel_feed(const HudFeedSource &source, bool shown, uint32_t game_type,
		int local_team, const hud::GameTextLookup &gametext, hud::HudFrameState &state) {
	hud::HudLfpPanelState &panel = state.lfp_panel;
	panel.local_team = local_team;
	// The compiler's element branches on it (the conquest arm, D-HUD-61).
	panel.game_type = game_type;
	panel.under_attack_text = hud::lfp_under_attack_text(gametext);
	panel.ready_text = hud::lfp_ready_text(gametext);
	// The takeover dialog rides the same zone push: its nearest entry, composed with the
	// gametext and the viewer's team; the frame draws it off the death screen.
	hud::HudTakeoverFacts takeover;
	if (shown && source.zones != nullptr) {
		panel.shown = collect_lfp_zones(source.view, *source.zones, local_team, panel.zones);
		collect_takeover_status(source.view, *source.zones, takeover);
	} else {
		panel.shown = false;
		panel.zones.clear();
	}
	state.takeover_status = hud::compose_takeover_status(takeover, local_team, gametext);
}

void hud_friendly_tags_feed(const HudFeedSource &source, bool shown,
		const hud::HudViewport &viewport, float fog_distance_units, hud::HudFrameState &state) {
	state.friendly_tags.clear();
	// The fog cull's distance, the environment's live word [orig: HUD_DrawEntityLabel
	//  `cmp esi, g_EnvFogDistCurrent` @0x5A3B28].
	const int32_t fog_q16 = io::float_to_fp16_16_sat(fog_distance_units);
	state.fog_dist_q16 = fog_q16 > 0 ? fog_q16 : INT32_MAX;
	state.speaking_level =
			source.view.kernel != nullptr ? source.view.kernel->world.audio_out_meter.stage1() : 0;
	if (!shown || !viewport.valid ||
			state.friendly_tag_mode == static_cast<int>(hud::FriendlyTagMode::kOff))
		return;
	std::vector<world::FriendlyTagSource> tags;
	if (!collect_friendly_tags(source.view, tags) || tags.empty()) return;
	hud::project_friendly_tags(tags, viewport, state.friendly_tags);
}

void hud_role_facts_feed(const HudFeedSource &source, uint32_t voice_menus,
		const hud::GameTextLookup &gametext, uint32_t hudls_system,
		const std::array<std::string, 12> &slot_bar_key_labels, hud::HudFrameState &state,
		HudRoleFeed &out) {
	const HudRoleFacts facts = hud_role_facts(source.view, voice_menus);
	const auto overlays = [&gametext](const char *key) {
		return gametext(hud::kGameTextOverlays, key, "");
	};
	state.emotes_menu = facts.emotes_menu;
	state.radio_menu = facts.radio_menu;
	state.paused_text = overlays("STROVER7");
	state.breath_samples = facts.breath.samples;
	state.breath_time = facts.breath.breath_time;
	state.round_over = facts.breath.round_over;
	state.breath_label = overlays("STROVER91");
	state.session = facts.session;
	state.squad_orders = facts.squad_orders;
	state.net_quality = facts.net_quality;
	state.frame_rate = facts.frame_rate;
	// The connection status line and the start timer with their Overlays texts
	// (hud::HudConnectStatus / hud::HudStartTimer carry the witness).
	state.connect_status = facts.connect_status;
	state.connect_status.label = overlays("STROVER54");
	state.start_timer.seconds = facts.start_timer_seconds;
	state.start_timer.label = overlays("STROVER59");
	state.start_timer.footer = overlays("STROVER63");
	// The EXPPOINTS element's words and its Overlays strings (hud/hud_exp_points.h).
	state.exp_points = facts.exp_points;
	hud::hud_exp_points_text(gametext, state.exp_points.text);
	state.title = facts.title;
	state.lone_gunner = facts.lone_gunner;
	// The vehicle panel's rider lines and their weapon texts (hud::HudVehicleRider carries the
	// witness).
	state.vehicle_riders = facts.vehicle_riders;
	hud::hud_vehicle_rider_text(gametext, state.vehicle_riders);
	out.inset_target_name = facts.inset_target_name;
	out.inset_reticle_quad = facts.inset_reticle_quad;
	hud::hud_session_text(gametext, state.session.game_type, state.session.text);
	out.slot_bar_loads = hudls_system != 0;
	for (size_t c = 0; c < state.slot_bar.size(); ++c) {
		hud::HudSlotBarSlot &slot = state.slot_bar[c];
		slot = hud::HudSlotBarSlot{};
		out.slot_bar_icons[c].clear();
		if (!out.slot_bar_loads) continue;
		slot.present = facts.slot_bar[c].adm_index >= 0;
		slot.count = facts.slot_bar[c].count;
		if (slot.present) out.slot_bar_icons[c] = facts.slot_bar_icons[c];
		// The key label: binding record 200 + the recorded def's category
		// [orig: HUD_DrawWeaponSlotBar @0x599e8c..0x599e9f].
		const int32_t def_category = facts.slot_bar[c].def_category;
		if (def_category >= 0 && def_category < static_cast<int32_t>(slot_bar_key_labels.size()))
			slot.key_label = slot_bar_key_labels[static_cast<size_t>(def_category)];
	}
}

} // namespace opennova::inmatch
