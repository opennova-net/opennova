#include "hud/hud_overlay.h"
#include "rtxt/rtxt_string_file.h"
#include "simulation/player_local_view.h"
#include "util/axes.h"
#include "util/string_convert.h"
#include <algorithm>
#include <cmath>
#include <godot_cpp/variant/plane.hpp>
#include <runtime/hud/hud_bay_logos.h>
#include <runtime/hud/hud_capture_labels.h>
#include <runtime/hud/hud_game_text.h>
#include <runtime/hud/hud_layout_from_hudpos.h>
#include <runtime/hud/hud_texture_names.h>

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
void HudOverlay::configure_combat_(const opennova::hud::HudLayoutAssets &assets) {
	using namespace opennova::hud;
	auto &l = layout_.combat;
	// HUD_LoadAllTextures loads the fixed art in colour mode and the hudpos
	// parachute and armor icons in alpha mode (docs/interface/hud-re.md
	// "The HUD texture loader"); the names are the engine's (hud_texture_names.h).
	constexpr ResourceRoot::TextureLoader colour = ResourceRoot::TEXTURE_LOADER_HUD_COLOR;
	constexpr ResourceRoot::TextureLoader alpha = ResourceRoot::TEXTURE_LOADER_HUD_ALPHA;
	combat_texture_(kHudTexVehicleFixed, hud_fixed_texture_name(kHudTexVehicleFixed), colour, l.vehicle_fixed);
	combat_texture_(kHudTexVehicleLag, hud_fixed_texture_name(kHudTexVehicleLag), colour, l.vehicle_lag);
	combat_texture_(kHudTexDriverCrosshair, hud_fixed_texture_name(kHudTexDriverCrosshair), colour, l.driver_crosshair);
	combat_texture_(kHudTexTarget, hud_fixed_texture_name(kHudTexTarget), colour, l.target);
	combat_texture_(kHudTexTargetFriendly, hud_fixed_texture_name(kHudTexTargetFriendly), colour, l.target_friendly);
	combat_texture_(kHudTexParachute, opennova::to_gd(assets.parachute_icon), alpha, l.parachute);
	combat_texture_(kHudTexArmor, opennova::to_gd(assets.armor_icon), alpha, l.armor);
	combat_texture_(kHudTexLogoHelo, hud_fixed_texture_name(kHudTexLogoHelo), colour, l.logo_helo);
	combat_texture_(kHudTexLogoHumm, hud_fixed_texture_name(kHudTexLogoHumm), colour, l.logo_humm);
	combat_texture_(kHudTexLogoBoat, hud_fixed_texture_name(kHudTexLogoBoat), colour, l.logo_boat);
}
void HudOverlay::set_combat_state(const Ref<PlayerLocalView> &view, const Transform3D &camera,
		const Projection &projection, bool has_camera, const Ref<RtxtStringFile> &gametext,
		const String &use_key) {
	using namespace opennova::hud;
	state_.combat = {};
	opennova::world::HudCombatView empty;
	const auto &v = view.is_valid() ? view->native_frame().hud_combat : empty;
	state_.combat = v.state;
	auto &s = state_.combat;
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
	const Vector2 surface = draw_surface_();
	const auto project = [&](const std::array<int32_t, 3> &point, bool valid) {
		HudProjectedPoint out;
		if (!has_camera || !valid)
			return out;
		const Vector3 position = mission_to_godot(opennova::world::Vec3{
				point[0] / 65536.0f, point[1] / 65536.0f, point[2] / 65536.0f });
		const Vector3 local = camera.xform_inv(position);
		const Plane clip = projection.xform4(Plane(local, 1));
		out.clip = -local.z < projection.get_z_near() ? 16 : 0;
		const double depth = -double(local.z) * 65536.0;
		out.depth_q16 = static_cast<int32_t>(std::clamp(depth, -2147483647.0, 2147483647.0));
		if (clip.d == 0 || !std::isfinite(clip.d))
			return out;
		const float x = clip.normal.x / clip.d, y = clip.normal.y / clip.d;
		if (x < -1)
			out.clip |= 1;
		if (x > 1)
			out.clip |= 2;
		if (y > 1)
			out.clip |= 4;
		if (y < -1)
			out.clip |= 8;
		out.x = (std::clamp(x, -1.0f, 1.0f) * 0.5f + 0.5f) * surface.x;
		out.y = (-std::clamp(y, -1.0f, 1.0f) * 0.5f + 0.5f) * surface.y;
		out.valid = true;
		return out;
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
		const String impact = gametext->get_string_in_section(opennova::hud::kGameTextOverlays, "STROVER_DIST");
		if (!impact.is_empty())
			s.impact_format = opennova::to_std(impact);
		const char *keys[] = { "STROVER_MEDGEAR", "STROVER_LOWGEAR", "STROVER_HIGEAR" };
		for (int i = 0; i < 3; ++i) {
			const String text = gametext->get_string_in_section(opennova::hud::kGameTextOverlays, keys[i]);
			if (!text.is_empty())
				s.gear_text[i] = opennova::to_std(text);
		}
		const String text = gametext->get_string_in_section("hud", "altitude");
		if (!text.is_empty())
			s.altitude_text = opennova::to_std(text);
	}
	compiler_.update_layout(layout_);
	queue_redraw();
}
} // namespace godot
