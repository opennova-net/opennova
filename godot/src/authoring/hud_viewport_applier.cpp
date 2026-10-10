#include "authoring/hud_viewport_applier.h"

#include <godot_cpp/classes/color_rect.hpp>
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/vector2.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

#include <editor/assets/project_asset_source.h>
#include <editor/preview/viewport_device.h>
#include <editor/preview/viewport_follow.h>
#include <editor/project/project_files.h>
#include <editor/session/view/session_view.h>
#include <runtime/hud/hud_elements.h>
#include <runtime/world/player_view.h>

#include "authoring/preview_backdrop.h"
#include "hud/hud_pos.h"
#include "hud/player_hud_weapon_def.h"
#include "object/weapon_def.h"
#include "simulation/player_local_view.h"
#include "util/string_convert.h"

#include <runtime/renderer/texture_compression.h>

namespace godot {

namespace {

// The game's own first-person view effects, mounted under the overlay as its HUD presenter mounts them
// (godot/game/world/game_hud_presenter.gd ensure_game_hud).
constexpr const char *kViewEffectsPath = "res://game/world/player_view_effects.gd";
// The game's SIGHTS card and scoped-view circle mask, mounted after the view effects as the presenter mounts them
// (game_hud_presenter.gd ensure_game_hud).
constexpr const char *kSightsCardPath = "res://game/world/hud_sights_card.gd";
constexpr const char *kScopeMaskPath = "res://game/world/hud_scope_circle_mask.gd";
// What the preview's backdrop is where the game has its 3D view on the Dark preview background (each
// picture's own): a plain mid grey, which the HUD's light and dark art both read over; the editor's preview
// background otherwise (authoring/preview_backdrop).
const Color kBackdrop(0.27f, 0.29f, 0.31f, 1.0f);
// The rangefinder's reading and the goggles' gain the preview's view effects show.
constexpr int kBinocularRange = 250;
constexpr int kNightVisionGain = 2;
// The field of view the HUD's crosshair spread is drawn at: the infantry view's.
constexpr float kFovDegrees = opennova::world::kPlayerCameraFovHDeg;

template <typename T>
T *node(uint64_t id) {
	return id ? Object::cast_to<T>(ObjectDB::get_instance(id)) : nullptr;
}

// A Control of the game's script at `path` (null where the build lacks it).
Control *script_control(const char *path) {
	ResourceLoader *loader = ResourceLoader::get_singleton();
	if (!loader->exists(path)) return nullptr;
	const Ref<Script> script = loader->load(path);
	return script.is_valid() ? Object::cast_to<Control>(script->call("new")) : nullptr;
}

} // namespace

HudViewportApplier::HudViewportApplier(SubViewport &viewport) {
	viewport_id_ = viewport.get_instance_id();
	ColorRect *backdrop = memnew(ColorRect);
	backdrop->set_name("Backdrop");
	backdrop->set_color(kBackdrop);
	backdrop_material_ = make_preview_backdrop_canvas(kBackdrop);
	backdrop->set_material(backdrop_material_);
	backdrop->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	viewport.add_child(backdrop);
	backdrop_id_ = backdrop->get_instance_id();
	HudOverlay *overlay = memnew(HudOverlay);
	overlay->set_name("Hud");
	overlay->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	viewport.add_child(overlay);
	overlay_id_ = overlay->get_instance_id();
	// The view effects behind the overlay's own draw list, a stable layer under it.
	if (Control *effects = script_control(kViewEffectsPath)) {
		effects->set_name("PlayerViewEffects");
		effects->set_draw_behind_parent(true);
		effects->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
		overlay->add_child(effects, false, Node::INTERNAL_MODE_BACK);
		effects->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
		effects_id_ = effects->get_instance_id();
	}
	// The SIGHTS card, then the circle mask over it, behind the overlay's draw list.
	if (Control *card = script_control(kSightsCardPath)) {
		card->set_name("SightsCard");
		card->set_draw_behind_parent(true);
		card->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
		overlay->add_child(card);
		card->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
		card_id_ = card->get_instance_id();
	}
	if (Control *mask = script_control(kScopeMaskPath)) {
		mask->set_name("ScopeCircleMask");
		mask->set_draw_behind_parent(true);
		mask->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
		mask->set_visible(false);
		overlay->add_child(mask);
		mask->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
		mask_id_ = mask->get_instance_id();
	}
	root_.instantiate();
}

HudOverlay *HudViewportApplier::overlay() const {
	return node<HudOverlay>(overlay_id_);
}

Control *HudViewportApplier::view_effects() const {
	return node<Control>(effects_id_);
}

Control *HudViewportApplier::sights_card() const {
	return node<Control>(card_id_);
}

Control *HudViewportApplier::scope_mask() const {
	return node<Control>(mask_id_);
}

void HudViewportApplier::rebuild(const opennova::editor::ViewportModel &model, const opennova::editor::SessionView &view,
		const opennova::editor::PreviewClock &) {
	clear();
	HudOverlay *hud = overlay();
	if (!hud || !view.findings.assets) return;
	// The project's files, the open documents standing in for theirs, each read noted with its stamp.
	stamped_ = std::make_shared<opennova::StampedFiles>(view.findings.assets);
	root_->mount_files(stamped_);
	Ref<HudPos> layout;
	layout.instantiate();
	if (layout->load_from_resource_root(root_, String(opennova::editor::basename_of(model.path()).c_str())) != OK) return;
	hud->configure(layout, root_);
	configured_ = hud->is_configured();
	// The weapons and the game text the weapon cluster reads (a project may lack either).
	weapons_.instantiate();
	if (root_->has_file("weapon.def") && weapons_->load_from_resource_root(root_, "weapon.def") != OK) weapons_.unref();
	gametext_.instantiate();
	const PackedByteArray text = root_->has_file("gametext.bin") ? root_->read_file("gametext.bin") : PackedByteArray();
	if (text.is_empty() || gametext_->load_from_byte_array(text) != OK) gametext_.unref();
	if (Control *effects = view_effects()) effects->call("set_resource_root", root_);
	applied_ = false;
}

void HudViewportApplier::update(const opennova::editor::ViewportModel &, const opennova::editor::PreviewClock &) {}

void HudViewportApplier::clear() {
	if (HudOverlay *hud = overlay()) hud->configure(Ref<HudPos>(), Ref<ResourceRoot>());
	configured_ = false;
	applied_ = false;
	weapon_.clear();
	weapon_capacity_ = 0;
	armed_ = false;
	weapons_.unref();
	gametext_.unref();
	slice_.unref();
	if (Control *card = sights_card()) card->call("set_weapon_sights", TypedArray<WeaponSightRow>(), Ref<ResourceRoot>());
	board_shown_ = false;
	stamped_.reset();
}

void HudViewportApplier::apply_options_(const opennova::editor::HudViewport &model) {
	HudOverlay *hud = overlay();
	if (!hud) return;
	const opennova::editor::HudViewportOptions &options = model.options();
	if (!applied_ || options.crosshair != options_.crosshair) hud->set_crosshair_style(options.crosshair);
	if (!applied_ || options.detail != options_.detail) hud->set_hud_detail_level(options.detail);
	// The weapon's HUD slice, as the presenter installs it on a weapon change: its name in the player's
	// words (gametext's WepDes), its clip and round art, and its silhouette.
	const opennova::editor::HudPreviewWeapon *weapon = model.weapon_shown();
	const std::string name = weapon ? weapon->name : std::string();
	if (!applied_ || name != weapon_) {
		weapon_ = name;
		weapon_capacity_ = 0;
		armed_ = false;
		const int index = weapon && weapons_.is_valid() ? weapons_->find_weapon(String(name.c_str())) : -1;
		const Ref<PlayerHudWeaponDef> slice =
				index >= 0 ? PlayerHudWeaponDef::from_weapon_def(weapons_->get_weapon(index)) : Ref<PlayerHudWeaponDef>();
		hud->install_weapon(slice, gametext_);
		// The card's rows for the weapon, their pictures through the stage loader at the game's fresh word.
		slice_ = slice;
		if (Control *card = sights_card())
			card->call("set_weapon_sights", slice.is_valid() ? slice->get_sights() : TypedArray<WeaponSightRow>(), root_,
					opennova::renderer::kTexCompressionLevelFreshProfile);
		if (slice.is_valid()) {
			armed_ = true;
			weapon_capacity_ = slice->get_clipsize();
			hud->set_weapon_icon(slice->get_weapon_name(), String(weapon->hudicon.c_str()));
		} else {
			hud->set_weapon_icon(String(), String());
		}
	}
	options_ = options;
	applied_ = true;
}

void HudViewportApplier::apply_sights_(const opennova::editor::HudViewport &model) {
	HudOverlay *hud = overlay();
	if (!hud) return;
	// The frame as the presenter reads it: the scope readouts, the player's mount and weapon category, the card's
	// scale and slide, the card up where the frame says (not into a goggle scene), the mask on the scene frame's
	// overlay fork (the Scoped arm), its cross and grid where the weapon authors no SIGHTS row.
	const opennova::world::LocalPlayerViewFrame *frame = model.scope_frame();
	Ref<PlayerLocalView> view;
	if (frame) {
		view.instantiate();
		view->assign(*frame);
	}
	hud->set_scope_state(view, gametext_);
	hud->set_player_context(view);
	const bool sights = slice_.is_valid() && !slice_->get_sights().is_empty();
	if (Control *card = sights_card()) {
		card->call("set_sight_state", hud->get_sight_scale_index(), view.is_valid() ? view->get_sight_slide_multiplier() : 0);
		card->call("set_card_up", frame && frame->scope_card_active && !frame->nvg_sights_in_scene);
	}
	if (Control *mask = scope_mask()) {
		const bool selectors = frame && frame->scope_card_active && !frame->vehicle_attack_context && slice_.is_valid();
		const int branch = HudPos::scoped_view_overlay(false, selectors && slice_->get_sighted_selector(),
				selectors && slice_->get_scoped_selector());
		const bool lens = frame && frame->nvg_lens_active;
		mask->call("set_mask_state", branch == 3 || lens, !sights, lens);
	}
}

void HudViewportApplier::apply_board_(const opennova::editor::HudViewport &model, int64_t ticks) {
	HudOverlay *hud = overlay();
	if (!hud) return;
	const opennova::editor::HudViewportOptions &options = model.options();
	if (!options.board) {
		if (board_shown_) hud->set_scoreboard_board(false, 0, gametext_, opennova::hud::HudScoreboardState());
		board_shown_ = false;
		return;
	}
	// The model's board: its stand-in rows, its strings composed over the project's tables.
	hud->set_scoreboard_board(true, int(ticks), gametext_, model.board());
	board_shown_ = true;
}

void HudViewportApplier::apply(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock,
		opennova::editor::ViewportDeviceReport &report) {
	const auto &hud_model = static_cast<const opennova::editor::HudViewport &>(model);
	HudOverlay *hud = overlay();
	const opennova::editor::HudViewportOptions &options = hud_model.options();
	// The HUD laid out at the screen the options name: the SubViewport's 2D space that screen, stretched onto the
	// picture, so what reads the screen off the viewport (the SIGHTS card's rows) reads the game's screen too.
	if (SubViewport *viewport = node<SubViewport>(viewport_id_)) {
		const Vector2i screen(options.width, options.height);
		if (viewport->get_size_2d_override() != screen) viewport->set_size_2d_override(screen);
		if (!viewport->is_size_2d_override_stretch_enabled()) viewport->set_size_2d_override_stretch(true);
	}
	if (ColorRect *backdrop = node<ColorRect>(backdrop_id_)) {
		backdrop->set_position(Vector2(0.0f, 0.0f));
		backdrop->set_size(Vector2(float(options.width), float(options.height)));
	}
	if (hud) {
		hud->set_position(Vector2(0.0f, 0.0f));
		hud->set_size(Vector2(float(options.width), float(options.height)));
		hud->set_scale(Vector2(1.0f, 1.0f));
	}
	if (stamped_) report.files = stamped_->stamps();
	if (!hud || !configured_) return;
	apply_options_(hud_model);
	// The weapon's state as the presenter feeds it each frame: a clip of -1 is a full one, an infinite
	// weapon's clip reads -1 (hud_math displayed_clip), and a capacity-1 weapon folds the chambered round
	// into the reserve (hud_math folded_reserve).
	const int clip = HudPos::displayed_clip(options.clip < 0 ? std::max(weapon_capacity_, 0) : options.clip,
			weapon_capacity_);
	const int reserve = HudPos::folded_reserve(clip, options.reserve, weapon_capacity_);
	// With the sights up, the aim and the spread as the run's weapon holds them, the keep-up the frame says.
	const opennova::world::LocalPlayerWeaponView *sighted = hud_model.scope_weapon();
	const opennova::world::LocalPlayerViewFrame *frame = hud_model.scope_frame();
	hud->set_weapon_state(armed_, clip, reserve, 0, sighted ? sighted->hud_spread_fp16 : 0,
			sighted && sighted->aimed_shot_available, frame && frame->hud_keep_crosshair_while_aimed, false, 0);
	// The view: the binoculars' state the HUD reads, the aim the first-person pin to the screen's middle.
	const bool binoculars = options.view == opennova::editor::HudPreviewView::Binoculars;
	const bool night_vision = options.view == opennova::editor::HudPreviewView::NightVision;
	const float inf = std::numeric_limits<float>::infinity();
	hud->set_view_state(binoculars, Vector2(inf, inf));
	if (Control *effects = view_effects()) {
		effects->call("update_view", binoculars, kBinocularRange, night_vision, kNightVisionGain);
		effects->call("update_damage_feedback", 0, options.damage, 0, 255);
	}
	apply_sights_(hud_model);
	apply_board_(hud_model, int64_t(clock.ticks()));
	// Where each element of the HUD's walk drew, in the screen's pixels: the boxes of its compile now.
	const std::vector<opennova::hud::HudElementBox> boxes = opennova::hud::hud_element_boxes(hud->compile_draw_list());
	report.rects.assign(opennova::hud::kHudElementCount, opennova::editor::ViewportDeviceReport::Rect());
	for (const opennova::hud::HudElementBox &box : boxes) {
		opennova::editor::ViewportDeviceReport::Rect &rect = report.rects[static_cast<size_t>(box.element)];
		const int left = int(std::floor(box.x0)), top = int(std::floor(box.y0));
		const int right = int(std::ceil(box.x1)), bottom = int(std::ceil(box.y1));
		if (!rect.placed) {
			rect = { true, left, top, right, bottom };
			continue;
		}
		rect.left = std::min(rect.left, left);
		rect.top = std::min(rect.top, top);
		rect.right = std::max(rect.right, right);
		rect.bottom = std::max(rect.bottom, bottom);
	}
}

void HudViewportApplier::tick(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock) {
	// The view effects' sun veil is the world's (the Celestial pushes its alpha as a process-wide global): the HUD
	// preview has no world and no sun, so none, whatever another device's Celestial (an environment view looking at
	// the sun) last pushed. The effects make the veil as they enter the tree.
	if (Control *effects = view_effects())
		if (CanvasItem *veil = Object::cast_to<CanvasItem>(effects->get_node_or_null(NodePath("SunVeil"))))
			if (veil->is_visible()) veil->set_visible(false);
	HudOverlay *hud = overlay();
	if (!hud || !configured_) return;
	// The player's state on the preview clock's ticks, the HUD's clock (its fades, its flashes).
	const auto &hud_model = static_cast<const opennova::editor::HudViewport &>(model);
	const opennova::editor::HudViewportOptions &options = hud_model.options();
	// The field of view the crosshair's spread is drawn at: the sights' zoomed one while they are up.
	const opennova::world::LocalPlayerViewFrame *frame = hud_model.scope_frame();
	hud->set_player_state(int(clock.ticks()), float(options.health) / 100.0f, options.stance,
			frame ? frame->fov_h_deg : kFovDegrees);
	// The 4-team page alternates on the HUD's clock.
	if (options.board) apply_board_(hud_model, int64_t(clock.ticks()));
}

void HudViewportApplier::background(opennova::editor::PreviewBackground background) {
	set_preview_backdrop(**backdrop_material_, background);
}

void HudViewportApplier::resize(int, int) {
	// The picture's size is the SubViewport's own; its 2D space is the options' screen stretched onto it (apply).
}

} // namespace godot
