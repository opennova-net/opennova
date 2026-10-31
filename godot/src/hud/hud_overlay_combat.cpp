#include "hud/hud_overlay.h"
#include "rtxt/rtxt_string_file.h"
#include "simulation/player_local_view.h"
#include "util/string_convert.h"
#include <algorithm>
#include <runtime/hud/hud_bay_logos.h>
#include <runtime/hud/hud_capture_labels.h>
#include <runtime/hud/hud_game_text.h>
#include <runtime/hud/hud_layout_from_hudpos.h>
#include <runtime/hud/hud_viewport.h>

namespace godot {
void HudOverlay::combat_texture_(int slot, const String &name, ResourceRoot::TextureLoader loader,
		opennova::hud::HudSprite &sprite) {
	// A slot reloads when its name or its loader changes (the cargo slot's mode
	// follows its art).
	if (combat_texture_names_[slot] != name || combat_texture_loaders_[slot] != static_cast<int>(loader)) {
		combat_texture_names_[slot] = name;
		combat_texture_loaders_[slot] = static_cast<int>(loader);
		textures_[slot] = load_hud_texture_(name, loader);
	}
	const auto &texture = textures_[slot];
	sprite = { texture.is_valid() ? texture->get_width() : 0,
		texture.is_valid() ? texture->get_height() : 0, texture.is_valid() };
}
void HudOverlay::set_weapon_icon(const String &p_weapon_name, const String &p_hudicon) {
	// The combat feed's two weapon fields (world::fill_hud_combat_view): the identity the silhouette's
	// flash stamps on, the hudicon art in alpha mode.
	state_.combat.weapon_identity = opennova::to_std(p_weapon_name);
	combat_texture_(opennova::hud::kHudTexWeaponSilhouette, p_hudicon, ResourceRoot::TEXTURE_LOADER_HUD_ALPHA,
			layout_.combat.weapon);
	compiler_.update_layout(layout_);
	queue_redraw();
}

void HudOverlay::set_combat_state(const Ref<PlayerLocalView> &view,
		const Ref<RtxtStringFile> &gametext, const String &use_key) {
	using namespace opennova::hud;
	state_.combat = {};
	opennova::world::HudCombatView empty;
	const auto &v = view.is_valid() ? view->native_frame().hud_combat : empty;
	state_.combat = v.state;
	auto &s = state_.combat;
	// The inset's friendly name is the role's lookup (set_role_facts).
	s.target_name = inset_target_name_;
	s.inset_reticle_quad = inset_reticle_quad_;
	auto &l = layout_.combat;
	// The weapon def's crosshair, commandersX and hudicon art and the item
	// defs' HUD images load in alpha mode; the cargo's stock flag and document
	// art in colour mode (world::HudCombatView::cargo_texture_alpha).
	constexpr ResourceRoot::TextureLoader alpha = ResourceRoot::TEXTURE_LOADER_HUD_ALPHA;
	combat_texture_(kHudTexCustomAim, String(v.custom_texture.c_str()), alpha, l.custom_aim);
	combat_texture_(kHudTexCommander, String(v.commander_texture.c_str()), alpha, l.commander);
	combat_texture_(kHudTexWeaponSilhouette, String(v.weapon_texture.c_str()), alpha, l.weapon);
	combat_texture_(kHudTexVehicleStatus, String(v.vehicle_texture.c_str()), alpha, l.vehicle);
	combat_texture_(kHudTexCargo, String(v.cargo_texture.c_str()),
			v.cargo_texture_alpha ? alpha : ResourceRoot::TEXTURE_LOADER_HUD_COLOR, l.cargo);
	// Each point through the frame's viewport (set_view_frame; engine hud/hud_viewport.h).
	const auto project = [this](const std::array<int32_t, 3> &point, bool valid) {
		return valid ? hud_project_world_point(viewport_, point) : HudProjectedPoint{};
	};
	s.vehicle_fixed_point = project(v.vehicle_fixed, v.vehicle_fixed_valid);
	s.vehicle_lag_point = project(v.vehicle_lag, v.vehicle_lag_valid);
	s.impact_point = project(v.impact, v.impact_valid);
	s.target_point = project(v.target, v.target_valid);
	s.aim_point = project(v.aim, v.aim_valid);
	s.commander_point = project(v.commander, v.commander_valid);
	// The vehicle-bay logos: the engine's walk over this frame's marker rows
	// (set_minimap_state fed them), each admitted point projected here.
	s.bay_logos.clear();
	if (v.local_valid) {
		vehicle_bay_logo_walk(state_.minimap.markers, v.local_position, v.local_team,
				s.bay_logos);
		for (HudBayLogo &logo : s.bay_logos)
			logo.point = project(logo.position, true);
	}
	// The capture-point labels: the engine's walk over the same rows (each
	// carrying its zone-timer entry) with the gametext strings, each admitted
	// point projected here.
	s.capture_labels.clear();
	if (v.local_valid) {
		capture_point_label_walk(state_.minimap.markers, v.local_team, game_text_lookup(gametext),
				s.capture_labels);
		for (HudCaptureLabel &label : s.capture_labels)
			label.point = project(label.position, true);
	}
	if (s.service_prompt) {
		// The templates, their miss rules and the sprintf are the engine's
		// (hud/hud_game_text.h service_prompt_text).
		s.service_text = service_prompt_text(s.service_prompt, use_key.utf8().get_data(),
				s.service_wait_seconds, game_text_lookup(gametext));
	}
	if (gametext.is_valid()) {
		// The override table first (game_text_lookup, D-NET-675).
		const opennova::hud::GameTextLookup lookup = game_text_lookup(gametext);
		const std::string impact = lookup(opennova::hud::kGameTextOverlays, "STROVER_DIST", "");
		if (!impact.empty())
			s.impact_format = impact;
		const char *keys[] = { "STROVER_MEDGEAR", "STROVER_LOWGEAR", "STROVER_HIGEAR" };
		for (int i = 0; i < 3; ++i) {
			const std::string text = lookup(opennova::hud::kGameTextOverlays, keys[i], "");
			if (!text.empty())
				s.gear_text[i] = text;
		}
		const std::string text = lookup("hud", "altitude", "");
		if (!text.empty())
			s.altitude_text = text;
	}
	compiler_.update_layout(layout_);
	queue_redraw();
}
} // namespace godot
