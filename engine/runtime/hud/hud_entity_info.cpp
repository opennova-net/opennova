#include <runtime/hud/hud_entity_info.h>

#include <base/io/fixed.h>
#include <runtime/hud/hud_math.h>
#include <runtime/world/local_player_view.h>
#include <runtime/world/player_view.h>
#include <runtime/world/player_weapon.h>

#include <algorithm>

namespace opennova::hud {

HudEntityInfo local_player_entity_info(int32_t health, int32_t max_health,
		const world::LocalPlayerViewFrame *view, const world::LocalPlayerWeaponView *weapon_view,
		const HudEntityWeaponDef *weapon) {
	HudEntityInfo out;
	// The bar's fraction of the health pair, clamped to [0, 1] and empty
	// without a ceiling. Retail's record word is 16.16, (health << 16) /
	// ceiling by idiv with the ceiling 1 when it is zero or the body has no
	// def, capped UNSIGNED at 0x10000 (`jbe`), so a negative health, and any
	// health over a zero ceiling, draws a full-width bar there (D-HUD-X1)
	// [orig: HUD_BuildEntityInfo @0x4b8791..0x4b87a4 (the ceiling),
	//  @0x4b87ac..0x4b87d3 (hudInfo+92)].
	const double fraction = max_health > 0
			? static_cast<double>(health) / static_cast<double>(max_health)
			: 0.0;
	out.health_fraction = static_cast<float>(std::clamp(fraction, 0.0, 1.0));
	// The stance icon row the view frame carries, its seat and weapon
	// overrides applied [orig: HUD_BuildEntityInfo @0x4b860c..0x4b8786]; the
	// camera's horizontal fov, the default 80 without a composed view
	// (world/player_view.h kPlayerCameraFovHDeg carries the witness).
	out.stance = view != nullptr ? view->hud_stance : 0;
	out.fov_h_deg = view != nullptr ? view->fov_h_deg : world::kPlayerCameraFovHDeg;
	// The weapon cluster's ammo pair while a weapon FSM runs: the displayed
	// clip (-1 for the clipsize -1 weapon) and the reserve with a capacity-1
	// weapon's chambered round folded in (hud_math.h carries both witnesses)
	// [orig: HUD_BuildEntityInfo @0x4b8573..0x4b85fa, hudInfo+52 / +56].
	const bool live = weapon_view != nullptr && weapon_view->active;
	if (live) {
		out.clip = weapon != nullptr ? displayed_clip(weapon_view->clip, weapon->clipsize)
				: weapon_view->clip;
		out.reserve = folded_reserve(out.clip, weapon_view->reserve,
				weapon != nullptr ? weapon->clipsize : -1);
	}
	// The cluster draws with a live FSM and the equipped def (hudInfo+552's
	// pointer [orig: HUD_BuildEntityInfo @0x4b8561]); its heat word is 0 on foot,
	// the clamped accumulator otherwise [orig: hudInfo+60 =
	// WeaponSlot_CalcAccumulatedHeat @0x53f780, @0x4b8533..0x4b8554]; the
	// dispersion, the aimed-shot verdict and the PowerThrow charge are the
	// weapon frame's [orig: HUD_DrawCrosshair @0x592b07..0x592bf5; the @0x4de4f7
	// promoter; g_FireChargeStartTick @0xB76800 read by
	// HUD_DrawPowerThrowChargeBar @0x599830], and the Inset keep-up predicate
	// the view frame's.
	out.weapon_drawn = live && weapon != nullptr;
	out.heat = live ? weapon_view->heat : 0;
	out.spread_fp16 = live ? weapon_view->hud_spread_fp16 : 0;
	out.aimed_shot_available = live && weapon_view->aimed_shot_available;
	out.keep_crosshair_while_aimed = view != nullptr && view->hud_keep_crosshair_while_aimed;
	out.windup_active = live && weapon_view->windup_active;
	out.windup_held_ticks = live ? weapon_view->windup_held_ticks : 0;
	out.ammo_bucket = live ? weapon_view->ammo_bucket : 0;
	out.ammo_class_id = live ? weapon_view->ammo_class_id : 0;
	// The view effects and the damage-feedback quads the frame carries; a
	// frame not yet composed reads the rangefinder's 1 and the revive tint's
	// channel 255 (world/player_view.h carries the witnesses).
	const bool scope_card = view != nullptr && view->scope_card_active;
	if (view != nullptr) {
		out.binoculars_view_active = view->binoculars_view_active;
		out.binocular_range = view->aim_range_q16 / io::kFp16OneInt;
		out.nvg_mask = view->nvg_mask_visible;
		out.scope_mask_nvg_lens = view->nvg_lens_active;
		out.nvg_sights_in_scene = view->nvg_sights_in_scene;
		out.nvg_gain = view->nvg_gain;
		out.flash_white = view->screen_flash_white_alpha;
		out.flash_red = view->screen_flash_red_alpha;
		out.flash_revive = view->screen_flash_revive;
		out.flash_revive_channel = view->screen_flash_revive_channel;
		out.overlays_suppressed = view->hud_overlays_suppressed;
	}
	// The SIGHTS card over the frame: the card switch, under the binocular
	// arm the fork tests first, and not over the NVG composite, whose Sighted
	// arm draws the card into the scene instead [orig:
	// Player_IsEquippedWeaponScoped @0x4dcc80; Render_ProcessMainSceneFrame
	// @0x5caae3..0x5caaec (the binocular mask ahead of the card arms
	// @0x5caaf3 / @0x5cab01); NVG_RenderSceneToTarget @0x5d08cb..0x5d0952].
	out.sights_card_up = scope_card && !out.binoculars_view_active && !out.nvg_sights_in_scene;
	// The fork over the two selector bytes, which the vehicle-attack context
	// clears before it; only the Scoped arm chains the circle mask, its one
	// argument that the card drew no SIGHTS row; under the NVG composite's
	// Scoped arm the lens draws its own ring and the reticle rides at unit
	// scale [orig: Render_ProcessMainSceneFrame @0x5ca2ff..0x5ca304 (the
	// clear), @0x5cab08..0x5cab15 (the mask call); NVG_DrawScopedLens
	// @0x5d2798..0x5d27bc].
	const bool card_selectors = scope_card && !(view != nullptr && view->vehicle_attack_context) &&
			weapon != nullptr;
	out.overlay = scoped_view_overlay(out.binoculars_view_active,
			card_selectors && weapon->sighted_selector, card_selectors && weapon->scoped_selector);
	out.scope_mask_up = out.overlay == ScopedViewOverlay::kScopedCardWithCircleMask ||
			out.scope_mask_nvg_lens;
	out.scope_mask_draw_crosshair = weapon == nullptr || !weapon->has_sights;
	return out;
}

} // namespace opennova::hud
