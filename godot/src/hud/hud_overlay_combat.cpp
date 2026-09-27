#include "hud/hud_overlay.h"
#include "rtxt/rtxt_string_file.h"
#include "simulation/player_local_view.h"
#include "util/axes.h"
#include "util/string_convert.h"
#include <algorithm>
#include <cmath>
#include <godot_cpp/variant/plane.hpp>
#include <runtime/hud/hud_game_text.h>
#include <runtime/hud/hud_layout_from_hudpos.h>

namespace godot {
void HudOverlay::combat_texture_(int slot, const String &name, opennova::hud::HudSprite &sprite) {
	if (combat_texture_names_[slot] != name) {
		combat_texture_names_[slot] = name;
		textures_[slot] = load_hud_texture_(name);
	}
	const auto &texture = textures_[slot];
	sprite = { texture.is_valid() ? texture->get_width() : 0,
		texture.is_valid() ? texture->get_height() : 0, texture.is_valid() };
}
void HudOverlay::configure_combat_(const opennova::hud::HudLayoutAssets &assets) {
	using namespace opennova::hud;
	auto &l = layout_.combat;
	combat_texture_(kHudTexVehicleFixed, "rockpip.tga", l.vehicle_fixed);
	combat_texture_(kHudTexVehicleLag, "turrpip.tga", l.vehicle_lag);
	combat_texture_(kHudTexDriverCrosshair, "dirguide.tga", l.driver_crosshair);
	combat_texture_(kHudTexTarget, "comalck2.tga", l.target);
	combat_texture_(kHudTexTargetFriendly, "comlck2x.tga", l.target_friendly);
	combat_texture_(kHudTexParachute, opennova::to_gd(assets.parachute_icon), l.parachute);
	combat_texture_(kHudTexArmor, opennova::to_gd(assets.armor_icon), l.armor);
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
	combat_texture_(kHudTexCustomAim, String(v.custom_texture.c_str()), l.custom_aim);
	combat_texture_(kHudTexCommander, String(v.commander_texture.c_str()), l.commander);
	combat_texture_(kHudTexWeaponSilhouette, String(v.weapon_texture.c_str()), l.weapon);
	combat_texture_(kHudTexVehicleStatus, String(v.vehicle_texture.c_str()), l.vehicle);
	combat_texture_(kHudTexCargo, String(v.cargo_texture.c_str()), l.cargo);
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
	if (s.service_prompt) {
		// The templates, their miss rules and the sprintf are the engine's
		// (hud/hud_game_text.h service_prompt_text).
		s.service_text = service_prompt_text(s.service_prompt, use_key.utf8().get_data(),
				s.service_wait_seconds, game_text_lookup(gametext));
	}
	if (gametext.is_valid()) {
		const String impact = gametext->get_string_in_section("Overlays", "STROVER_DIST");
		if (!impact.is_empty())
			s.impact_format = opennova::to_std(impact);
		const char *keys[] = { "STROVER_MEDGEAR", "STROVER_LOWGEAR", "STROVER_HIGEAR" };
		for (int i = 0; i < 3; ++i) {
			const String text = gametext->get_string_in_section("Overlays", keys[i]);
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
