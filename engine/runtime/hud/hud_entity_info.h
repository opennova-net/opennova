#pragma once

#include <runtime/hud/scope_circle_mask.h>

#include <cstdint>

// The local player's per-frame HUD info record: the fields the shell's HUD
// presenter writes into the overlay, the SIGHTS card, the scoped-view circle
// mask and the view effects each frame, filled from the engine's view and
// weapon frames as retail rebuilds its 576-byte info struct at the head of
// the overlay pass.
// [orig: HUD_BuildEntityInfo @0x4b8440; HUD_RenderAllOverlays @0x5a8070, its
//  call @0x5a80bc after the struct's memset; the scene frame's card and mask
//  fork Render_ProcessMainSceneFrame @0x5ca0f0]
// Witness record: docs/interface/hud-re.md.

namespace opennova::world {
struct LocalPlayerViewFrame;
struct LocalPlayerWeaponView;
} // namespace opennova::world

namespace opennova::hud {

// The equipped weapon-def slice the fill reads: hudInfo+552's def pointer
// [orig: HUD_BuildEntityInfo @0x4b8561], here the def's clipsize, its two
// card selector halves (scoped_selector_from_def / sighted_selector_from_def)
// and whether it authors any SIGHTS row.
struct HudEntityWeaponDef {
	int32_t clipsize = 0;
	bool scoped_selector = false;
	bool sighted_selector = false;
	bool has_sights = false;
};

struct HudEntityInfo {
	// The health bar's fraction, 0..1: hudInfo+92's 16.16 ratio over 65536.
	float health_fraction = 0.0f;
	// The stance icon row (the view frame's seat and weapon overrides applied).
	int32_t stance = 0;
	// The main camera's horizontal fov, degrees (the crosshair spread's divisor).
	float fov_h_deg = 0.0f;
	// The weapon cluster: drawn at all, the displayed clip and reserve, the
	// heat word, the crosshair dispersion, the aimed-shot verdict, the Inset
	// keep-up predicate, the PowerThrow charge, and the clip-flash key's halves.
	bool weapon_drawn = false;
	int32_t clip = -1;
	int32_t reserve = -1;
	int32_t heat = 0;
	int32_t spread_fp16 = 0;
	bool aimed_shot_available = false;
	bool keep_crosshair_while_aimed = false;
	bool windup_active = false;
	int32_t windup_held_ticks = 0;
	int32_t ammo_bucket = 0;
	int32_t ammo_class_id = 0;
	// The SIGHTS card over the frame (not under the binocular mask, not drawn
	// into the NVG scene instead).
	bool sights_card_up = false;
	// The NVG Sighted arm: the card draws into the NVG scene.
	bool nvg_sights_in_scene = false;
	// The scene frame's overlay fork after the selector clear, and the circle
	// mask it chains: up, its one argument (no SIGHTS row), and the NVG lens's
	// unit-scale reticle in its place.
	ScopedViewOverlay overlay = ScopedViewOverlay::kEntityMarkers;
	bool scope_mask_up = false;
	bool scope_mask_draw_crosshair = true;
	bool scope_mask_nvg_lens = false;
	// The view effects: the binocular view and its rangefinder, the NVG mask
	// and gain, the three damage-feedback quads, and the HUD overlay pass's
	// early return under the white flash.
	bool binoculars_view_active = false;
	int32_t binocular_range = 1;
	bool nvg_mask = false;
	int32_t nvg_gain = 0;
	int32_t flash_white = 0;
	int32_t flash_red = 0;
	int32_t flash_revive = 0;
	int32_t flash_revive_channel = 255;
	bool overlays_suppressed = false;
};

// The record for one frame. `view` is the frame the HUD presents (null: no
// composed view yet), `weapon_view` the equipped weapon's FSM view (null or
// inactive: no weapon FSM), `weapon` the equipped def's slice (null: no def),
// and the health pair the HUD bar reads (world::local_player_health /
// local_player_max_health).
HudEntityInfo local_player_entity_info(int32_t health, int32_t max_health,
		const world::LocalPlayerViewFrame *view, const world::LocalPlayerWeaponView *weapon_view,
		const HudEntityWeaponDef *weapon);

} // namespace opennova::hud
