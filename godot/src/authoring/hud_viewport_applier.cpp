#include "authoring/hud_viewport_applier.h"

#include <godot_cpp/classes/color_rect.hpp>
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/script.hpp>
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

#include "authoring/preview_backdrop.h"
#include "hud/hud_pos.h"
#include "hud/player_hud_weapon_def.h"
#include "object/weapon_def.h"
#include "util/string_convert.h"

namespace godot {

namespace {

// The game's own first-person view effects, mounted under the overlay as its HUD presenter mounts them
// (godot/game/world/game_hud_presenter.gd ensure_game_hud).
constexpr const char *kViewEffectsPath = "res://game/world/player_view_effects.gd";
// What the preview's backdrop is where the game has its 3D view on the Dark preview background (each
// picture's own): a plain mid grey, which the HUD's light and dark art both read over; the editor's preview
// background otherwise (authoring/preview_backdrop).
const Color kBackdrop(0.27f, 0.29f, 0.31f, 1.0f);
// The rangefinder's reading and the goggles' gain the preview's view effects show.
constexpr int kBinocularRange = 250;
constexpr int kNightVisionGain = 2;
// The field of view the HUD's crosshair spread is drawn at: the infantry view's.
constexpr float kFovDegrees = 80.0f;

template <typename T>
T *node(uint64_t id) {
	return id ? Object::cast_to<T>(ObjectDB::get_instance(id)) : nullptr;
}

} // namespace

HudViewportApplier::HudViewportApplier(SubViewport &viewport) {
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
	ResourceLoader *loader = ResourceLoader::get_singleton();
	if (loader->exists(kViewEffectsPath)) {
		const Ref<Script> script = loader->load(kViewEffectsPath);
		Control *effects = script.is_valid() ? Object::cast_to<Control>(script->call("new")) : nullptr;
		if (effects) {
			effects->set_name("PlayerViewEffects");
			effects->set_draw_behind_parent(true);
			effects->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
			overlay->add_child(effects, false, Node::INTERNAL_MODE_BACK);
			effects->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
			effects_id_ = effects->get_instance_id();
		}
	}
	root_.instantiate();
}

HudOverlay *HudViewportApplier::overlay() const {
	return node<HudOverlay>(overlay_id_);
}

Control *HudViewportApplier::view_effects() const {
	return node<Control>(effects_id_);
}

void HudViewportApplier::rebuild(const opennova::editor::ViewportModel &model, const opennova::editor::SessionView &view,
		const opennova::editor::PreviewClock &) {
	clear();
	HudOverlay *hud = overlay();
	if (!hud || !view.findings.assets) return;
	// The project's files, the open documents standing in for theirs, each read noted with its stamp.
	stamped_ = std::make_shared<opennova::editor::StampedFiles>(view.findings.assets);
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
		if (slice.is_valid()) {
			armed_ = true;
			weapon_capacity_ = slice->get_clipsize();
			hud->set_weapon(slice->get_weapon_name(), HudPos::weapon_display_name(gametext_, slice->get_weapon_name()),
					slice->get_clipsize(), slice->get_rounds_per_icon(), slice->get_clipgfx_texture(),
					slice->get_clipgfx_offset(), slice->get_rndgfx_texture(), slice->get_rndgfx_offset(),
					slice->get_rndgfx_step());
			hud->set_weapon_icon(slice->get_weapon_name(), String(weapon->hudicon.c_str()));
		} else {
			hud->clear_weapon();
			hud->set_weapon_icon(String(), String());
		}
	}
	options_ = options;
	applied_ = true;
}

void HudViewportApplier::apply(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &,
		opennova::editor::ViewportDeviceReport &report) {
	const auto &hud_model = static_cast<const opennova::editor::HudViewport &>(model);
	HudOverlay *hud = overlay();
	const opennova::editor::HudViewportOptions &options = hud_model.options();
	// The HUD laid out at the screen the options name, scaled onto the picture.
	if (hud) {
		hud->set_position(Vector2(0.0f, 0.0f));
		hud->set_size(Vector2(float(options.width), float(options.height)));
		hud->set_scale(Vector2(float(width_) / float(options.width), float(height_) / float(options.height)));
	}
	if (stamped_) report.files = stamped_->stamps();
	if (!hud || !configured_) return;
	apply_options_(hud_model);
	// The weapon's state as the presenter feeds it each frame: a clip of -1 is a full one, and a
	// capacity-1 weapon folds the chambered round into the reserve (hud_math folded_reserve).
	// An infinite weapon's clip reads -1, as its clipsize does (the presenter's rule).
	const int clip = weapon_capacity_ == -1 ? -1 : options.clip < 0 ? std::max(weapon_capacity_, 0) : options.clip;
	const int reserve = HudPos::folded_reserve(clip, options.reserve, weapon_capacity_);
	hud->set_weapon_state(armed_, clip, reserve, 0, 0, false, false, false, 0);
	// The view: the binoculars' state the HUD reads, the aim the first-person pin to the screen's middle.
	const bool binoculars = options.view == opennova::editor::HudPreviewView::Binoculars;
	const bool night_vision = options.view == opennova::editor::HudPreviewView::NightVision;
	const float inf = std::numeric_limits<float>::infinity();
	hud->set_view_state(binoculars, Vector2(inf, inf));
	if (Control *effects = view_effects()) {
		effects->call("update_view", binoculars, kBinocularRange, night_vision, kNightVisionGain);
		effects->call("update_damage_feedback", 0, options.damage, 0, 255);
	}
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
	HudOverlay *hud = overlay();
	if (!hud || !configured_) return;
	// The player's state on the preview clock's ticks, the HUD's clock (its fades, its flashes).
	const opennova::editor::HudViewportOptions &options = static_cast<const opennova::editor::HudViewport &>(model).options();
	hud->set_player_state(int(clock.ticks()), float(options.health) / 100.0f, options.stance, kFovDegrees);
}

void HudViewportApplier::background(opennova::editor::PreviewBackground background) {
	set_preview_backdrop(**backdrop_material_, background);
}

void HudViewportApplier::resize(int width, int height) {
	width_ = std::max(width, 1);
	height_ = std::max(height, 1);
	if (ColorRect *backdrop = node<ColorRect>(backdrop_id_)) {
		backdrop->set_position(Vector2(0.0f, 0.0f));
		backdrop->set_size(Vector2(float(width_), float(height_)));
	}
}

} // namespace godot
