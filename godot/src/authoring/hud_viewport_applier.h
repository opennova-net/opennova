#pragma once

#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>

#include <cstdint>
#include <memory>
#include <string>

#include <editor/preview/hud_viewport.h>

#include "authoring/viewport_applier.h"
#include "hud/hud_overlay.h"
#include "hud/player_hud_weapon_def.h"
#include "object/weapon_database.h"
#include "resource_index/resource_root.h"
#include "rtxt/rtxt_string_file.h"

namespace opennova {
class StampedFiles;
}

namespace godot {

class ColorRect;
class Control;

// A HUD viewport's device work (the plan's DI-20): the runtime's own HudOverlay, which compiles the HUD
// through the engine's layout fill and frame compiler (runtime/hud) and draws its list, sized to the
// screen the viewport's options name and scaled onto the device's SubViewport, so the HUD is laid out
// at that screen exactly (its fonts, its scale) whatever room the canvas gives it; the game's own
// first-person view effects (godot/game/world/player_view_effects.gd, its binocular and goggle masks
// and the damage vignette, loaded by path as the editor loads its MCP transport) as the overlay's
// behind-parent child, as the game's HUD presenter mounts them, and after them the game's SIGHTS card and
// scoped-view circle mask (godot/game/world/hud_sights_card.gd, hud_scope_circle_mask.gd), the card first so the
// mask covers its corners; a backdrop under both where the game has its 3D view. A Rebuild mounts a root over the
// project's files (the open documents standing in for theirs: the HUD layout as Save would write it) and
// configures the overlay from hudpos.def through it, with weapon.def and gametext.bin; each pump
// applies the player's state the options choose, as the game's presenter feeds it from the world (the weapon's HUD
// slice and silhouette, the clip and the reserve, the stance and the health on the preview clock's ticks, the
// view; the sights' frame the model's run left through the overlay's scope readouts, the card and the mask; the
// Tab board the model composes, its stand-in rows and its strings over the project's tables, through the
// overlay's board entry), and reports the files it
// read and where each element of the HUD's walk drew (ViewportDeviceReport::rects, one per HudElement,
// in the screen's pixels: runtime/hud/hud_elements.h). It reads none of the process-wide render state a
// mission publishes (reads_scene_state false).
class HudViewportApplier final : public ViewportApplier {
public:
	explicit HudViewportApplier(SubViewport &viewport);

	void rebuild(const opennova::editor::ViewportModel &model, const opennova::editor::SessionView &view,
			const opennova::editor::PreviewClock &clock) override;
	void update(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock) override;
	void clear() override;
	void apply(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock,
			opennova::editor::ViewportDeviceReport &report) override;
	void tick(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock) override;
	void resize(int width, int height) override;
	bool reads_scene_state() const override { return false; }
	// The editor's preview background behind the HUD (its own mid grey on Dark).
	void background(opennova::editor::PreviewBackground background) override;

	// What it holds (a GUT device test reads it): the overlay, the view effects (null where the game's
	// script is not in the build), whether a layout is configured, the weapon applied.
	HudOverlay *overlay() const;
	Control *view_effects() const;
	Control *sights_card() const;
	Control *scope_mask() const;
	bool configured() const { return configured_; }
	const std::string &weapon_applied() const { return weapon_; }

private:
	// The options applied to the overlay as it last applied them (a configure applies them all again).
	void apply_options_(const opennova::editor::HudViewport &hud);
	// The sights' frame (the scope readouts, the card, the mask), and the Tab board on the clock's tick.
	void apply_sights_(const opennova::editor::HudViewport &hud);
	void apply_board_(const opennova::editor::HudViewport &hud, int64_t ticks);

	uint64_t backdrop_id_ = 0;
	Ref<ShaderMaterial> backdrop_material_; // the backdrop's: the editor's preview background over it
	uint64_t overlay_id_ = 0;
	uint64_t effects_id_ = 0;
	uint64_t card_id_ = 0, mask_id_ = 0;
	Ref<ResourceRoot> root_;
	std::shared_ptr<opennova::StampedFiles> stamped_; // what the root was asked for
	Ref<WeaponDatabase> weapons_;
	Ref<RtxtStringFile> gametext_;
	Ref<PlayerHudWeaponDef> slice_; // the weapon's HUD slice: its SIGHTS rows and selectors
	bool board_shown_ = false;
	bool configured_ = false;
	bool applied_ = false; // the options applied since the last configure
	std::string weapon_;
	bool armed_ = false; // the weapon the options name found in weapon.def and installed
	int weapon_capacity_ = 0;
	opennova::editor::HudViewportOptions options_;
	int width_ = 1, height_ = 1;
};

} // namespace godot
