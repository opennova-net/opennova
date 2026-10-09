#include "hud/hud_pos.h"
#include "fnt/fnt_resource.h"
#include "hud/font_page_glyphs.h"
#include "render/d3d9_raster_device.h"
#include "util/color_convert.h"
#include "util/string_convert.h"
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <runtime/hud/game_font.h>
#include "hud/vehicle_hud_block.h"

#include <godot_cpp/classes/canvas_item.hpp>

#include "resource_index/resource_root.h"
#include "rtxt/rtxt_string_file.h"
#include "simulation/hud_view_records.h"
#include "util/data_format.h"

#include <formats/def/def.h>
#include <runtime/hud/hud_game_text.h>
#include <runtime/hud/hud_math.h>
#include <runtime/hud/scope_circle_mask.h>
#include <runtime/hud/sight_overlay.h>
#include <runtime/hud/loading_screen.h>
#include <runtime/hud/view_effects.h>
#include <runtime/renderer/aspect_ratio.h>
#include <runtime/renderer/frame_fx_effects.h>

using namespace opennova::def;

// The GDScript-facing mirrors are pinned to the engine's witnessed values —
// a drifted copy here would silently split the native expiry policy from the
// shell's wrap/trim consumers.
static_assert(godot::HudPos::MESSAGE_LIFE_TICKS ==
		opennova::hud::kMessageLifeTicks);
static_assert(godot::HudPos::MESSAGE_EXPIRY_STAGGER ==
		opennova::hud::kMessageExpiryStagger);
static_assert(godot::HudPos::MESSAGE_TEXT_MAX ==
		opennova::hud::kMessageTextMax);
static_assert(godot::HudPos::MESSAGE_SLOT_COUNT ==
		opennova::hud::kMessageSlotCount);
static_assert(godot::HudPos::LOADING_BAND_LEFT ==
		opennova::hud::kLoadingBandLeft);
static_assert(godot::HudPos::LOADING_BAND_TOP ==
		opennova::hud::kLoadingBandTop);
static_assert(godot::HudPos::LOADING_BAND_RIGHT_CUSTOM ==
		opennova::hud::kLoadingBandRightCustom);
static_assert(godot::HudPos::LOADING_BAND_RIGHT_STOCK ==
		opennova::hud::kLoadingBandRightStock);
static_assert(godot::HudPos::LOADING_BAND_BOTTOM ==
		opennova::hud::kLoadingBandBottom);
static_assert(godot::HudPos::LOADING_PRESENT_INTERVAL_MS ==
		opennova::hud::kLoadingPresentIntervalMs);
static_assert(godot::HudPos::SPLASH_CONTINUE_X ==
		opennova::hud::kSplashContinueX);
static_assert(godot::HudPos::SPLASH_CONTINUE_Y ==
		opennova::hud::kSplashContinueY);
static_assert(godot::HudPos::SPLASH_BLINK_MASK_MS ==
		opennova::hud::kSplashBlinkMaskMs);
static_assert(godot::HudPos::SPLASH_ARROW_SCALE_BASE_W ==
		opennova::hud::kSplashArrowScaleBaseW);
static_assert(godot::HudPos::SPLASH_ARROW_SCALE_BASE_H ==
		opennova::hud::kSplashArrowScaleBaseH);
static_assert(godot::HudPos::SPLASH_FONT_SCALE_BASE_W ==
		opennova::hud::kSplashContinueFontScaleBaseW);
static_assert(godot::HudPos::BINOCULAR_DIGIT_STEP ==
		opennova::hud::kBinocularDigitStep);
static_assert(godot::HudPos::VIEW_DIGIT_CELL ==
		opennova::hud::kViewDigitCell);

double godot::HudPos::percent_to_alpha() {
	return opennova::hud::kPercentToAlpha;
}

using namespace godot;

namespace {

} // namespace

void HudPos::_bind_methods() {
	ClassDB::bind_static_method("HudPos", D_METHOD("scale_point", "design", "surface"), &HudPos::scale_point);
	ClassDB::bind_static_method("HudPos", D_METHOD("scale_rect", "design", "surface"), &HudPos::scale_rect);
	ClassDB::bind_static_method("HudPos", D_METHOD("d3d9_screen_offset"), &HudPos::d3d9_screen_offset);
	ClassDB::bind_static_method("HudPos", D_METHOD("sight_scale_rect", "design", "surface"), &HudPos::sight_scale_rect);
	ClassDB::bind_static_method("HudPos", D_METHOD("nvg_scene_sight_rect", "design", "surface", "aspect_mode"), &HudPos::nvg_scene_sight_rect);
	ClassDB::bind_static_method("HudPos", D_METHOD("pixel_delta_to_design", "delta", "surface"), &HudPos::pixel_delta_to_design);
	ClassDB::bind_static_method("HudPos", D_METHOD("fade_decay", "elapsed_ticks", "ramp_ticks"), &HudPos::fade_decay);
	ClassDB::bind_static_method("HudPos", D_METHOD("fade_flash_alpha", "elapsed_ticks", "ramp_ticks", "base_alpha", "max_alpha"), &HudPos::fade_flash_alpha);
	ClassDB::bind_static_method("HudPos", D_METHOD("stance_current_alpha", "elapsed_ticks", "ramp_ticks", "base_alpha"), &HudPos::stance_current_alpha);
	ClassDB::bind_static_method("HudPos", D_METHOD("stance_prev_alpha", "elapsed_ticks", "ramp_ticks"), &HudPos::stance_prev_alpha);
	ClassDB::bind_static_method("HudPos", D_METHOD("stance_scale_q16", "frame0_size"), &HudPos::stance_scale_q16);
	ClassDB::bind_static_method("HudPos", D_METHOD("stance_scaled_dim", "dim", "q16"), &HudPos::stance_scaled_dim);
	ClassDB::bind_static_method("HudPos", D_METHOD("stance_center_offset", "frame0_size", "q16"), &HudPos::stance_center_offset);
	ClassDB::bind_static_method("HudPos", D_METHOD("health_color_band", "fraction"), &HudPos::health_color_band);
	ClassDB::bind_static_method("HudPos", D_METHOD("message_expire_tick", "now_ticks", "prev_expire", "has_prev"), &HudPos::message_expire_tick);
	ClassDB::bind_static_method("HudPos", D_METHOD("format_ammo", "clip", "reserve", "capacity"), &HudPos::format_ammo);
	ClassDB::bind_static_method("HudPos", D_METHOD("round_icon_count", "clip", "reserve", "capacity", "divisor"), &HudPos::round_icon_count);
	ClassDB::bind_static_method("HudPos", D_METHOD("folded_reserve", "clip", "reserve", "capacity"), &HudPos::folded_reserve);
	ClassDB::bind_static_method("HudPos", D_METHOD("displayed_clip", "clip", "capacity"), &HudPos::displayed_clip);
	ClassDB::bind_static_method("HudPos", D_METHOD("waypoint_distance_m", "ground_delta"), &HudPos::waypoint_distance_m);
	ClassDB::bind_static_method("HudPos", D_METHOD("loading_bar_fill_span", "x", "w", "displayed"), &HudPos::loading_bar_fill_span);
	ClassDB::bind_static_method("HudPos", D_METHOD("crosshair_spread_px_fp16", "spread_fp16", "fov_deg", "screen_w"), &HudPos::crosshair_spread_px_fp16);
	ClassDB::bind_static_method("HudPos", D_METHOD("crosshair_total_spread_fp16", "error_fp16", "recoil_pitch_bam", "weight_spread_bam"), &HudPos::crosshair_total_spread_fp16);
	ClassDB::bind_static_method("HudPos", D_METHOD("crosshair_error_row", "stance", "scoped"), &HudPos::crosshair_error_row);
	ClassDB::bind_static_method("HudPos", D_METHOD("crosshair_should_draw", "aimed", "keep_while_aimed"), &HudPos::crosshair_should_draw);
	ClassDB::bind_static_method("HudPos", D_METHOD("loading_gametype_text_key", "game_type"), &HudPos::loading_gametype_text_key);
	ClassDB::bind_static_method("HudPos", D_METHOD("waypoint_label", "mission", "gametext", "view", "in_session", "game_type"), &HudPos::waypoint_label);
	ClassDB::bind_static_method("HudPos", D_METHOD("subgoal_message", "mission", "lost", "header_id"), &HudPos::subgoal_message);
	ClassDB::bind_static_method("HudPos", D_METHOD("objective_header", "gametext"), &HudPos::objective_header);
	ClassDB::bind_static_method("HudPos", D_METHOD("objective_directive", "mission", "win", "header_id"), &HudPos::objective_directive);
	ClassDB::bind_static_method("HudPos", D_METHOD("triggered_text", "mission", "text_id"), &HudPos::triggered_text);
	ClassDB::bind_static_method("HudPos", D_METHOD("weapon_display_name", "gametext", "weapon_id"), &HudPos::weapon_display_name);
	ClassDB::bind_static_method("HudPos", D_METHOD("loading_fallback_image"), &HudPos::loading_fallback_image);
	ClassDB::bind_static_method("HudPos", D_METHOD("loading_font_small"), &HudPos::loading_font_small);
	ClassDB::bind_static_method("HudPos", D_METHOD("loading_font_large"), &HudPos::loading_font_large);
	ClassDB::bind_static_method("HudPos", D_METHOD("loading_msg_label_key"), &HudPos::loading_msg_label_key);
	ClassDB::bind_static_method("HudPos", D_METHOD("loading_msg_label_fallback"), &HudPos::loading_msg_label_fallback);
	ClassDB::bind_static_method("HudPos", D_METHOD("loading_sidecar_image_name", "mission_file"), &HudPos::loading_sidecar_image_name);
	ClassDB::bind_static_method("HudPos", D_METHOD("loading_present_due", "elapsed_ms", "reported_changed"), &HudPos::loading_present_due);
	ClassDB::bind_static_method("HudPos", D_METHOD("loading_msg_x_frac"), &HudPos::loading_msg_x_frac);
	ClassDB::bind_static_method("HudPos", D_METHOD("loading_msg_right_frac"), &HudPos::loading_msg_right_frac);
	ClassDB::bind_static_method("HudPos", D_METHOD("loading_msg_label_y_frac"), &HudPos::loading_msg_label_y_frac);
	ClassDB::bind_static_method("HudPos", D_METHOD("loading_msg_body_y_frac"), &HudPos::loading_msg_body_y_frac);
	ClassDB::bind_static_method("HudPos", D_METHOD("loading_msg_label_color"), &HudPos::loading_msg_label_color);
	ClassDB::bind_static_method("HudPos", D_METHOD("loading_bar_pos"), &HudPos::loading_bar_pos);
	ClassDB::bind_static_method("HudPos", D_METHOD("loading_bar_size"), &HudPos::loading_bar_size);
	ClassDB::bind_static_method("HudPos", D_METHOD("loading_bar_border_gray"), &HudPos::loading_bar_border_gray);
	ClassDB::bind_static_method("HudPos", D_METHOD("loading_bar_fill_color"), &HudPos::loading_bar_fill_color);
	ClassDB::bind_static_method("HudPos", D_METHOD("loading_splash_arrow_image"), &HudPos::loading_splash_arrow_image);
	ClassDB::bind_static_method("HudPos", D_METHOD("loading_splash_sound_set"), &HudPos::loading_splash_sound_set);
	ClassDB::bind_static_method("HudPos", D_METHOD("loading_splash_continue_key"), &HudPos::loading_splash_continue_key);
	ClassDB::bind_static_method("HudPos", D_METHOD("loading_splash_continue_font"), &HudPos::loading_splash_continue_font);
	ClassDB::bind_static_method("HudPos", D_METHOD("draw_splash_continue", "item", "font", "pages", "text", "size", "phase_on"), &HudPos::draw_splash_continue);
	ClassDB::bind_static_method("HudPos", D_METHOD("font_page_textures", "font"), &HudPos::font_page_textures);
	ClassDB::bind_static_method("HudPos", D_METHOD("glyph_shader_code"), &HudPos::glyph_shader_code);
	ClassDB::bind_static_method("HudPos", D_METHOD("draw_wrapped_text", "item", "font", "font_size", "text", "x", "y", "width", "bottom", "align", "color", "skip_lines"), &HudPos::draw_wrapped_text, DEFVAL(0));
	ClassDB::bind_static_method("HudPos", D_METHOD("binocular_crosshair_rect"), &HudPos::binocular_crosshair_rect);
	ClassDB::bind_static_method("HudPos", D_METHOD("binocular_digit_pos"), &HudPos::binocular_digit_pos);
	ClassDB::bind_static_method("HudPos", D_METHOD("nvg_scale_rect"), &HudPos::nvg_scale_rect);
	ClassDB::bind_static_method("HudPos", D_METHOD("nvg_scale_modulate"), &HudPos::nvg_scale_modulate);
	ClassDB::bind_static_method("HudPos", D_METHOD("binocular_range_step", "current", "target"), &HudPos::binocular_range_step);
	ClassDB::bind_static_method("HudPos", D_METHOD("view_effect_texture", "which"), &HudPos::view_effect_texture);
	ClassDB::bind_static_method("HudPos", D_METHOD("scope_mask_points", "surface", "screen_width", "draw_crosshair", "batch", "aspect_mode", "nvg_lens"), &HudPos::scope_mask_points, DEFVAL(-1), DEFVAL(false));
	ClassDB::bind_static_method("HudPos", D_METHOD("scope_mask_colors", "surface", "screen_width", "draw_crosshair", "batch", "aspect_mode", "nvg_lens"), &HudPos::scope_mask_colors, DEFVAL(-1), DEFVAL(false));
	ClassDB::bind_static_method("HudPos", D_METHOD("scope_mask_indices", "surface", "screen_width", "draw_crosshair", "batch", "aspect_mode", "nvg_lens"), &HudPos::scope_mask_indices, DEFVAL(-1), DEFVAL(false));
	ClassDB::bind_static_method("HudPos", D_METHOD("scope_mask_frame", "surface", "screen_width", "aspect_mode"), &HudPos::scope_mask_frame, DEFVAL(-1));
	ClassDB::bind_static_method("HudPos", D_METHOD("scoped_view_overlay", "binoculars_view_active", "sighted", "scoped"), &HudPos::scoped_view_overlay);
	ClassDB::bind_static_method("HudPos", D_METHOD("scoped_selector_from_def", "weapon_flags", "weapon_flags2"), &HudPos::scoped_selector_from_def);
	ClassDB::bind_static_method("HudPos", D_METHOD("sighted_selector_from_def", "weapon_flags", "slot_switching_from"), &HudPos::sighted_selector_from_def);
	ClassDB::bind_method(D_METHOD("load", "path"), &HudPos::load);
	ClassDB::bind_method(D_METHOD("load_from_resource_root", "resource_root", "name"), &HudPos::load_from_resource_root);
	ClassDB::bind_method(D_METHOD("is_loaded"), &HudPos::is_loaded);
	ClassDB::bind_method(D_METHOD("get_source_path"), &HudPos::get_source_path);
	ClassDB::bind_method(D_METHOD("get_last_error"), &HudPos::get_last_error);
	ClassDB::bind_method(D_METHOD("get_vehicle_hud", "sid"), &HudPos::get_vehicle_hud);

	BIND_CONSTANT(DESIGN_WIDTH);
	BIND_CONSTANT(DESIGN_HEIGHT);
	BIND_CONSTANT(MESSAGE_LIFE_TICKS);
	BIND_CONSTANT(MESSAGE_EXPIRY_STAGGER);
	BIND_CONSTANT(MESSAGE_TEXT_MAX);
	BIND_CONSTANT(MESSAGE_SLOT_COUNT);
	BIND_CONSTANT(LOADING_BAND_LEFT);
	BIND_CONSTANT(LOADING_BAND_TOP);
	BIND_CONSTANT(LOADING_BAND_RIGHT_CUSTOM);
	BIND_CONSTANT(LOADING_BAND_RIGHT_STOCK);
	BIND_CONSTANT(LOADING_BAND_BOTTOM);
	BIND_CONSTANT(LOADING_PRESENT_INTERVAL_MS);
	BIND_CONSTANT(SPLASH_CONTINUE_X);
	BIND_CONSTANT(SPLASH_CONTINUE_Y);
	BIND_CONSTANT(SPLASH_BLINK_MASK_MS);
	BIND_CONSTANT(SPLASH_ARROW_SCALE_BASE_W);
	BIND_CONSTANT(SPLASH_ARROW_SCALE_BASE_H);
	BIND_CONSTANT(SPLASH_FONT_SCALE_BASE_W);
	BIND_CONSTANT(BINOCULAR_DIGIT_STEP);
	BIND_CONSTANT(VIEW_DIGIT_CELL);
	BIND_CONSTANT(VIEW_TEXTURE_BINOCULAR_MASK);
	BIND_CONSTANT(VIEW_TEXTURE_BINOCULAR_CROSSHAIR);
	BIND_CONSTANT(VIEW_TEXTURE_BINOCULAR_DIGITS);
	BIND_CONSTANT(VIEW_TEXTURE_NVG_MASK);
	BIND_CONSTANT(VIEW_TEXTURE_NVG_SCALE);
	BIND_CONSTANT(VIEW_TEXTURE_VIGNETTE);
	BIND_CONSTANT(SCOPE_MASK_RING);
	BIND_CONSTANT(SCOPE_MASK_CROSS);
	BIND_CONSTANT(SCOPE_MASK_GRID);
	ClassDB::bind_static_method("HudPos", D_METHOD("percent_to_alpha"),
			&HudPos::percent_to_alpha);
}

HudPos::HudPos() {}

HudPos::~HudPos() {
	clear_();
}

void HudPos::clear_() {
	if (loaded_) {
		def_free_hudpos(&file_);
	}
	file_ = {};
	loaded_ = false;
}

Error HudPos::parse_bytes_(const PackedByteArray &bytes, const String &src) {
	clear_();
	// An empty file is a file of no line, not an error: the game's line walk
	// runs no callback for it and the HUD keeps every unauthored global
	// (docs/interface/hud-re.md D-HUD-54).
	static const uint8_t no_line = 0;
	const uint8_t *data = bytes.is_empty() ? &no_line : bytes.ptr();
	if (def_parse_hudpos_memory(data, static_cast<size_t>(bytes.size()), &file_) != 0) {
		file_ = {};
		last_error_ = String("def_parse_hudpos_memory failed for ") + src;
		return ERR_PARSE_ERROR;
	}
	loaded_ = true;
	source_path_ = src;
	last_error_ = String();
	return OK;
}

Error HudPos::load(const String &path) {
	PackedByteArray bytes;
	if (!read_nova_payload_file(path, bytes)) {
		clear_();
		last_error_ = String("Cannot open hudpos.def: ") + path;
		return ERR_CANT_OPEN;
	}
	return parse_bytes_(bytes, path);
}

Error HudPos::load_from_resource_root(const Ref<ResourceRoot> &p_resource_root, const String &p_name) {
	if (p_resource_root.is_null() || p_resource_root->get_root_dir().is_empty()) {
		clear_();
		last_error_ = "Resource root is not configured";
		return ERR_INVALID_PARAMETER;
	}
	const String file_name = p_name.get_file();
	if (file_name.is_empty()) {
		clear_();
		last_error_ = "hudpos.def filename is empty";
		return ERR_INVALID_PARAMETER;
	}
	const PackedByteArray bytes = p_resource_root->read_file(file_name);
	if (bytes.is_empty() && !p_resource_root->has_file(file_name)) {
		clear_();
		last_error_ = String("hudpos.def not found in resource root: ") + file_name;
		return ERR_FILE_NOT_FOUND;
	}
	return parse_bytes_(bytes, file_name);
}

bool HudPos::is_loaded() const {
	return loaded_;
}

String HudPos::get_source_path() const {
	return source_path_;
}

String HudPos::get_last_error() const {
	return last_error_;
}

// One VEHICLE_HUD block by items.def sid, case-insensitively -- matching the
// _stricmp the original commits the block with; the LAST matching block, since
// each commit overwrites the item's copy, and an empty sid matches a block
// with none (the commit and its witness live with the parse, def_hudpos.cpp).
// This only hands the block out.
//
// An unknown sid returns NULL rather than a default-filled block: a vehicle
// with no authored panel draws none.
Ref<VehicleHudBlock> HudPos::get_vehicle_hud(const String &p_sid) const {
	if (!loaded_) return Ref<VehicleHudBlock>();
	const String want = p_sid.to_lower();
	for (size_t i = file_.hud.vehicle_huds_count; i-- > 0;) {
		const DefVehicleHudBlock &v = file_.hud.vehicle_huds[i];
		if (String::utf8(v.sid).to_lower() != want) continue;
		Ref<VehicleHudBlock> out;
		out.instantiate();
		out->assign(v);
		return out;
	}
	return Ref<VehicleHudBlock>();
}

Vector2 HudPos::scale_point(const Vector2 &p_design, const Vector2 &p_surface) {
	return Vector2(
			static_cast<float>(opennova::hud::scale_axis(
					p_design.x, p_surface.x, opennova::hud::kDesignWidth)),
			static_cast<float>(opennova::hud::scale_axis(
					p_design.y, p_surface.y, opennova::hud::kDesignHeight)));
}

Vector2 HudPos::d3d9_screen_offset() {
	return d3d9_screen_to_canvas().get_origin();
}

Rect2 HudPos::scale_rect(const Rect2 &p_design, const Vector2 &p_surface) {
	const Vector2 p0 = scale_point(p_design.position, p_surface);
	const Vector2 p1 = scale_point(p_design.position + p_design.size, p_surface);
	return Rect2(p0, p1 - p0);
}

Rect2 HudPos::sight_scale_rect(const Rect2 &p_design, const Vector2 &p_surface) {
	const opennova::hud::SightRect rect{static_cast<int32_t>(p_design.position.x),
			static_cast<int32_t>(p_design.position.y),
			static_cast<int32_t>(p_design.position.x + p_design.size.x),
			static_cast<int32_t>(p_design.position.y + p_design.size.y)};
	const auto screen = opennova::hud::sight_rect_to_viewport(rect, p_surface.x, p_surface.y);
	return Rect2(screen.x1, screen.y1, screen.x2 - screen.x1, screen.y2 - screen.y1);
}

Rect2 HudPos::nvg_scene_sight_rect(const Rect2 &p_design, const Vector2 &p_surface,
		int p_aspect_mode) {
	if (p_surface.x <= 0.0f || p_surface.y <= 0.0f) {
		return Rect2();
	}
	const opennova::hud::SightRect rect{static_cast<int32_t>(p_design.position.x),
			static_cast<int32_t>(p_design.position.y),
			static_cast<int32_t>(p_design.position.x + p_design.size.x),
			static_cast<int32_t>(p_design.position.y + p_design.size.y)};
	const float side = static_cast<float>(opennova::renderer::kNvgSceneSide);
	const auto scene = opennova::hud::sight_rect_to_viewport_at_ratio(rect, side, side,
			opennova::renderer::aspect_height_over_width(p_aspect_mode, p_surface.x, p_surface.y));
	return Rect2(scene.x1, scene.y1, scene.x2 - scene.x1, scene.y2 - scene.y1);
}

Vector2 HudPos::pixel_delta_to_design(const Vector2 &p_delta, const Vector2 &p_surface) {
	if (p_surface.x <= 0.0f || p_surface.y <= 0.0f) return Vector2();
	return Vector2(
			static_cast<float>(opennova::hud::pixel_delta_to_design(
					p_delta.x, p_surface.x, opennova::hud::kDesignWidth)),
			static_cast<float>(opennova::hud::pixel_delta_to_design(
					p_delta.y, p_surface.y, opennova::hud::kDesignHeight)));
}

int HudPos::fade_decay(int p_elapsed_ticks, int p_ramp_ticks) {
	return opennova::hud::fade_decay(p_elapsed_ticks, p_ramp_ticks);
}

int HudPos::fade_flash_alpha(int p_elapsed_ticks, int p_ramp_ticks,
		int p_base_alpha, int p_max_alpha) {
	return opennova::hud::fade_flash_alpha(p_elapsed_ticks, p_ramp_ticks,
			p_base_alpha, p_max_alpha);
}

int HudPos::stance_current_alpha(int p_elapsed_ticks, int p_ramp_ticks,
		int p_base_alpha) {
	return opennova::hud::stance_current_alpha(p_elapsed_ticks, p_ramp_ticks,
			p_base_alpha);
}

int HudPos::stance_prev_alpha(int p_elapsed_ticks, int p_ramp_ticks) {
	return opennova::hud::stance_prev_alpha(p_elapsed_ticks, p_ramp_ticks);
}

int HudPos::stance_scale_q16(const Vector2i &p_frame0_size) {
	return opennova::hud::stance_scale_q16(p_frame0_size.x, p_frame0_size.y);
}

int HudPos::stance_scaled_dim(int p_dim, int p_q16) {
	return opennova::hud::stance_scaled_dim(p_dim, p_q16);
}

Vector2i HudPos::stance_center_offset(const Vector2i &p_frame0_size, int p_q16) {
	return Vector2i(
			opennova::hud::stance_center_axis(
					opennova::hud::stance_scaled_dim(p_frame0_size.x, p_q16)),
			opennova::hud::stance_center_axis(
					opennova::hud::stance_scaled_dim(p_frame0_size.y, p_q16)));
}

int HudPos::health_color_band(float p_fraction) {
	// The producer domain is the witnessed 16.16 ratio; the float fraction
	// re-enters it by truncation, the original data path.
	return opennova::hud::health_color_band_fp16(
			static_cast<int32_t>(p_fraction * 65536.0f));
}

int HudPos::message_expire_tick(int p_now_ticks, int p_prev_expire, bool p_has_prev) {
	return opennova::hud::message_expire_tick(p_now_ticks, p_prev_expire, p_has_prev);
}

String HudPos::format_ammo(int p_clip, int p_reserve, int p_capacity) {
	return String(opennova::hud::format_ammo(p_clip, p_reserve, p_capacity).c_str());
}

int HudPos::weapon_name_x_nudge(bool p_narrow_surface, int p_align) {
	return opennova::hud::weapon_name_x_nudge(p_narrow_surface, p_align);
}

int HudPos::round_icon_count(int p_clip, int p_reserve, int p_capacity, int p_divisor) {
	return opennova::hud::round_icon_count(p_clip, p_reserve, p_capacity, p_divisor);
}

int HudPos::folded_reserve(int p_clip, int p_reserve, int p_capacity) {
	return opennova::hud::folded_reserve(p_clip, p_reserve, p_capacity);
}

int HudPos::displayed_clip(int p_clip, int p_capacity) {
	return opennova::hud::displayed_clip(p_clip, p_capacity);
}

int HudPos::waypoint_distance_m(const Vector2 &p_ground_delta) {
	return opennova::hud::waypoint_distance_m(p_ground_delta.x, p_ground_delta.y);
}

int HudPos::heat_fill_span(int p_extent_px, int p_heat) {
	return opennova::hud::heat_fill_span(p_extent_px, p_heat);
}

bool HudPos::heat_bar_is_horizontal(const Vector2 &p_bar_size) {
	return opennova::hud::heat_bar_is_horizontal(p_bar_size.x, p_bar_size.y);
}

int HudPos::power_fill_span(int p_progress_fp16, int p_extent_px) {
	return opennova::hud::power_fill_span(p_progress_fp16, p_extent_px);
}

String HudPos::loading_fallback_image() { return opennova::hud::kLoadingFallbackImage; }
String HudPos::loading_font_small() { return opennova::hud::kLoadingFontSmall; }
String HudPos::loading_font_large() { return opennova::hud::kLoadingFontLarge; }
String HudPos::loading_msg_label_key() { return opennova::hud::kLoadingServerMessageLabelKey; }
String HudPos::loading_msg_label_fallback() { return opennova::hud::kLoadingServerMessageLabelFallback; }

String HudPos::loading_sidecar_image_name(const String &p_mission_file) {
	return opennova::to_gd(opennova::hud::loading_sidecar_image_name(
			opennova::to_std(p_mission_file)));
}

bool HudPos::loading_present_due(int p_elapsed_ms, bool p_reported_changed) {
	return opennova::hud::loading_present_due(p_elapsed_ms, p_reported_changed);
}

Vector2i HudPos::loading_bar_fill_span(int p_x, int p_w, int p_displayed) {
	const opennova::hud::LoadingBarSpan span =
			opennova::hud::loading_bar_fill_span(p_x, p_w, p_displayed);
	return Vector2i(span.left, span.right);
}

double HudPos::crosshair_spread_px_fp16(int p_spread_fp16, double p_fov_deg,
		double p_screen_w) {
	return opennova::hud::crosshair_spread_px_fp16(
			static_cast<int32_t>(p_spread_fp16), p_fov_deg, p_screen_w);
}

int HudPos::crosshair_total_spread_fp16(int p_error_fp16,
		int p_recoil_pitch_bam, int p_weight_spread_bam) {
	return opennova::hud::crosshair_total_spread_fp16(
			static_cast<int32_t>(p_error_fp16),
			static_cast<int32_t>(p_recoil_pitch_bam),
			static_cast<int32_t>(p_weight_spread_bam));
}

int HudPos::crosshair_error_row(int p_stance, bool p_scoped) {
	return opennova::hud::crosshair_error_row(p_stance, p_scoped);
}

bool HudPos::crosshair_should_draw(bool p_aimed, bool p_keep_while_aimed) {
	return opennova::hud::crosshair_should_draw(p_aimed, p_keep_while_aimed);
}

// --- game text (hud/hud_game_text.h carries the witnesses) ------------------

String HudPos::waypoint_label(const Ref<RtxtStringFile> &p_mission,
		const Ref<RtxtStringFile> &p_gametext, const Ref<WaypointHudView> &p_view,
		bool p_in_session, int64_t p_game_type) {
	if (p_view.is_null() || p_view->get_current() < 0) return String();
	const opennova::world::WaypointHudView &v = p_view->value();
	opennova::hud::WaypointNameKey key;
	key.name_id = v.entry.name_id;
	key.has_def = v.has_def;
	key.def_type = v.def_type;
	key.def_attrib = v.def_attrib;
	key.zone_number = v.zone_number;
	const uint32_t game_type = static_cast<uint32_t>(p_game_type);
	const opennova::hud::GameTextLookup gametext = game_text_lookup(p_gametext);
	const std::string name = opennova::hud::waypoint_display_name(key, p_in_session, game_type,
			game_text_lookup(p_mission), gametext);
	return opennova::to_gd(opennova::hud::waypoint_label_text(name, key, game_type, gametext));
}

String HudPos::subgoal_message(const Ref<RtxtStringFile> &p_mission, bool p_lost,
		int p_header_id) {
	return opennova::to_gd(opennova::hud::subgoal_message(p_lost, p_header_id,
			game_text_lookup(p_mission)));
}

String HudPos::objective_header(const Ref<RtxtStringFile> &p_gametext) {
	return opennova::to_gd(opennova::hud::objective_header(game_text_lookup(p_gametext)));
}

String HudPos::objective_directive(const Ref<RtxtStringFile> &p_mission, bool p_win,
		int p_header_id) {
	return opennova::to_gd(opennova::hud::objective_directive(p_win, p_header_id,
			game_text_lookup(p_mission)));
}

String HudPos::triggered_text(const Ref<RtxtStringFile> &p_mission, int p_text_id) {
	return opennova::to_gd(opennova::hud::triggered_text(p_text_id, game_text_lookup(p_mission)));
}

String HudPos::weapon_display_name(const Ref<RtxtStringFile> &p_gametext,
		const String &p_weapon_id) {
	return opennova::to_gd(opennova::hud::weapon_display_name(opennova::to_std(p_weapon_id),
			game_text_lookup(p_gametext)));
}

// --- loading screen (hud/loading_screen.h carries the values/witnesses) -----

String HudPos::loading_gametype_text_key(int p_game_type) {
	return String(opennova::hud::loading_gametype_text_key(
			static_cast<uint32_t>(p_game_type)));
}

double HudPos::loading_msg_x_frac() {
	return opennova::hud::kLoadingMsgXFrac;
}

double HudPos::loading_msg_right_frac() {
	return opennova::hud::kLoadingMsgRightFrac;
}

double HudPos::loading_msg_label_y_frac() {
	return opennova::hud::kLoadingMsgLabelYFrac;
}

double HudPos::loading_msg_body_y_frac() {
	return opennova::hud::kLoadingMsgBodyYFrac;
}

Color HudPos::loading_msg_label_color() {
	const uint32_t rgb = opennova::hud::kLoadingMsgLabelRgb;
	return opennova::color_from_rgb24(rgb);
}

Vector2i HudPos::loading_bar_pos() {
	return Vector2i(opennova::hud::kLoadingBarX, opennova::hud::kLoadingBarY);
}

Vector2i HudPos::loading_bar_size() {
	return Vector2i(opennova::hud::kLoadingBarW, opennova::hud::kLoadingBarH);
}

Color HudPos::loading_bar_border_gray() {
	const uint32_t rgb = opennova::hud::kLoadingBarBorderGray;
	return opennova::color_from_rgb24(rgb);
}

Color HudPos::loading_bar_fill_color() {
	const uint32_t argb = opennova::hud::kLoadingBarFillArgb;
	return opennova::color_from_argb(argb);
}

String HudPos::loading_splash_arrow_image() {
	return String(opennova::hud::kSplashArrowImage);
}

String HudPos::loading_splash_sound_set() {
	return String(opennova::hud::kSplashSoundSet);
}

String HudPos::loading_splash_continue_key() {
	return String(opennova::hud::kSplashContinueTextKey);
}

String HudPos::loading_splash_continue_font() {
	return String(opennova::hud::kSplashContinueFont);
}

int HudPos::draw_wrapped_text(CanvasItem *p_item, const Ref<Font> &p_font, int p_font_size,
		const String &p_text, int p_x, int p_y, int p_width, int p_bottom, int p_align,
		const Color &p_color, int p_skip_lines) {
	if (p_item == nullptr || p_font.is_null()) return 0;
	// The measure is the FontFile view of the .fnt (D-LOADSCR-2 carries the
	// CGameFont metric residual); the rules are the engine's.
	const opennova::hud::TextExtent extent = [&p_font, p_font_size](const std::string &s) {
		return static_cast<float>(
				p_font->get_string_size(opennova::to_gd(s), HORIZONTAL_ALIGNMENT_LEFT, -1, p_font_size).x);
	};
	opennova::hud::TextBlockAlign align = opennova::hud::TextBlockAlign::kLeft;
	if (p_align == HORIZONTAL_ALIGNMENT_CENTER) align = opennova::hud::TextBlockAlign::kCenter;
	else if (p_align == HORIZONTAL_ALIGNMENT_RIGHT) align = opennova::hud::TextBlockAlign::kRight;
	const opennova::hud::TextBlock block = opennova::hud::layout_text_block(extent,
			static_cast<int>(p_font->get_height(p_font_size)), opennova::to_std(p_text), p_x, p_y,
			p_x + p_width, p_bottom, align, p_skip_lines);
	// `y` is the line TOP, so each baseline adds the ascent.
	const float ascent = p_font->get_ascent(p_font_size);
	for (const opennova::hud::TextBlockLine &row : block.lines) {
		p_item->draw_string(p_font, Vector2(row.x, row.y + ascent), opennova::to_gd(row.text),
				HORIZONTAL_ALIGNMENT_LEFT, -1, p_font_size, p_color);
	}
	return block.stopped_at;
}

Array HudPos::draw_splash_continue(CanvasItem *p_item, const Ref<FntResource> &p_font, const Array &p_pages,
		const String &p_text, const Vector2i &p_size, bool p_phase_on) {
	Array out;
	const opennova::fnt::fnt_font_t *parsed = p_font.is_valid() ? p_font->parsed_font() : nullptr;
	if (parsed == nullptr) return out;
	// The line in the game's code page, a character it has no byte for drawn as '?'.
	std::string text;
	for (int64_t i = 0; i < p_text.length(); ++i) {
		std::uint8_t byte = '?';
		opennova::cp1252_encode_codepoint(static_cast<char32_t>(p_text[i]), byte);
		text.push_back(static_cast<char>(byte));
	}
	opennova::hud::GameFont font;
	font.set_font(parsed);
	const opennova::hud::GameFontRun run =
			opennova::hud::splash_continue_run(font, text.c_str(), p_size.x, p_size.y, p_phase_on);
	// One triangle array per page run, through the page's material: its MODULATE2X stage rides the glyph
	// shader's UV.y flag, doubling the drawer's halved colour (hud::kFontPageMaterialWord).
	const Vector2 uv_flag(0.0f, font_page_runs_modulate2x() ? kGlyphCanvasUvFlag : 0.0f);
	RenderingServer *rs = RenderingServer::get_singleton();
	for (size_t first = 0; first < run.quads.size();) {
		const uint32_t page = run.quads[first].page;
		size_t end = first + 1;
		while (end < run.quads.size() && run.quads[end].page == page) ++end;
		GlyphRunArrays arrays;
		append_glyph_quads(run.quads, first, end, uv_flag, arrays);
		Ref<Texture2D> texture;
		if (page < static_cast<uint32_t>(p_pages.size())) texture = p_pages[page];
		if (p_item != nullptr && texture.is_valid())
			rs->canvas_item_add_triangle_array(p_item->get_canvas_item(), arrays.indices, arrays.points, arrays.colors,
					arrays.uvs, PackedInt32Array(), PackedFloat32Array(), texture->get_rid());
		Dictionary row;
		row["page"] = static_cast<int64_t>(page);
		row["uvs"] = arrays.uvs;
		row["colors"] = arrays.colors;
		out.push_back(row);
		first = end;
	}
	return out;
}

Array HudPos::font_page_textures(const Ref<FntResource> &p_font) {
	Array out;
	const opennova::fnt::fnt_font_t *parsed = p_font.is_valid() ? p_font->parsed_font() : nullptr;
	if (parsed == nullptr) return out;
	for (uint32_t page = 0; page < parsed->num_pages; ++page) out.push_back(font_page_texture(*parsed, page));
	return out;
}

String HudPos::glyph_shader_code() {
	return String(glyph_canvas_shader_code());
}

// --- first-person view effects (hud/view_effects.h) -------------------------

Rect2 HudPos::binocular_crosshair_rect() {
	return Rect2(opennova::hud::kBinocularCrosshairX,
			opennova::hud::kBinocularCrosshairY,
			opennova::hud::kBinocularCrosshairW,
			opennova::hud::kBinocularCrosshairH);
}

Vector2 HudPos::binocular_digit_pos() {
	return Vector2(opennova::hud::kBinocularDigitX,
			opennova::hud::kBinocularDigitY);
}

Rect2 HudPos::nvg_scale_rect() {
	return Rect2(opennova::hud::kNvgScaleX, opennova::hud::kNvgScaleY,
			opennova::hud::kNvgScaleW, opennova::hud::kNvgScaleH);
}

Color HudPos::nvg_scale_modulate() {
	const float c = opennova::hud::kNvgScaleModulate / 255.0f;
	return Color(c, c, c, 1.0f);
}

int HudPos::binocular_range_step(int p_current, int p_target) {
	return opennova::hud::binocular_range_step(p_current, p_target);
}

String HudPos::view_effect_texture(int p_which) {
	if (p_which < 0 || p_which >= opennova::hud::kViewTexCount) {
		return String();
	}
	return String(opennova::hud::kViewEffectTextureNames[p_which]);
}

namespace {

// One built mask, memoised on its inputs: the typed getters below each want a
// different slice of the same build, and the shell asks for all four whenever
// the surface or the crosshair gate changes.
struct ScopeMaskCache {
	bool valid = false;
	int32_t w = 0;
	int32_t h = 0;
	int32_t screen_w = 0;
	bool draw_crosshair = false;
	int aspect_mode = 0;
	bool nvg_lens = false;
	opennova::hud::ScopeCircleMask mask;
};

const opennova::hud::ScopeCircleMask *scope_mask_build(const Vector2 &p_surface,
		int p_screen_width, bool p_draw_crosshair, int p_aspect_mode, bool p_nvg_lens) {
	static ScopeMaskCache cache;
	const int32_t w = static_cast<int32_t>(p_surface.x);
	const int32_t h = static_cast<int32_t>(p_surface.y);
	if (w <= 0 || h <= 0) {
		return nullptr;
	}
	const int32_t screen_w = p_screen_width > 0 ? p_screen_width : w;
	if (cache.valid && cache.w == w && cache.h == h && cache.screen_w == screen_w &&
			cache.draw_crosshair == p_draw_crosshair && cache.aspect_mode == p_aspect_mode &&
			cache.nvg_lens == p_nvg_lens) {
		return &cache.mask;
	}
	if (p_nvg_lens) {
		// The lens's reticle only when the card drew no row; the lens draws the
		// ring itself.
		cache.mask = p_draw_crosshair
				? opennova::hud::build_nvg_lens_reticle(0, 0, w - 1, h - 1, screen_w)
				: opennova::hud::ScopeCircleMask();
	} else {
		cache.mask = opennova::hud::build_scope_circle_mask(0, 0, w - 1, h - 1, screen_w,
				p_draw_crosshair, p_aspect_mode);
	}
	cache.valid = true;
	cache.w = w;
	cache.h = h;
	cache.screen_w = screen_w;
	cache.draw_crosshair = p_draw_crosshair;
	cache.aspect_mode = p_aspect_mode;
	cache.nvg_lens = p_nvg_lens;
	return &cache.mask;
}

const std::vector<opennova::hud::ScopeMaskVertex> *scope_mask_verts(
		const opennova::hud::ScopeCircleMask &mask, int batch) {
	switch (batch) {
		case godot::HudPos::SCOPE_MASK_RING: return &mask.ring;
		case godot::HudPos::SCOPE_MASK_CROSS: return &mask.crosshair;
		case godot::HudPos::SCOPE_MASK_GRID: return &mask.grid;
		default: return nullptr;
	}
}

const std::vector<uint16_t> *scope_mask_ids(const opennova::hud::ScopeCircleMask &mask,
		int batch) {
	switch (batch) {
		case godot::HudPos::SCOPE_MASK_RING: return &mask.ring_indices;
		case godot::HudPos::SCOPE_MASK_CROSS: return &mask.crosshair_indices;
		case godot::HudPos::SCOPE_MASK_GRID: return &mask.grid_indices;
		default: return nullptr;
	}
}

} // namespace

PackedVector2Array HudPos::scope_mask_points(const Vector2 &p_surface, int p_screen_width,
		bool p_draw_crosshair, int p_batch, int p_aspect_mode, bool p_nvg_lens) {
	PackedVector2Array out;
	const opennova::hud::ScopeCircleMask *mask =
			scope_mask_build(p_surface, p_screen_width, p_draw_crosshair, p_aspect_mode,
					p_nvg_lens);
	if (mask == nullptr) {
		return out;
	}
	const std::vector<opennova::hud::ScopeMaskVertex> *verts = scope_mask_verts(*mask, p_batch);
	if (verts == nullptr) {
		return out;
	}
	out.resize(static_cast<int64_t>(verts->size()));
	for (size_t i = 0; i < verts->size(); ++i) {
		out[static_cast<int64_t>(i)] = Vector2((*verts)[i].x, (*verts)[i].y);
	}
	return out;
}

PackedColorArray HudPos::scope_mask_colors(const Vector2 &p_surface, int p_screen_width,
		bool p_draw_crosshair, int p_batch, int p_aspect_mode, bool p_nvg_lens) {
	PackedColorArray out;
	const opennova::hud::ScopeCircleMask *mask =
			scope_mask_build(p_surface, p_screen_width, p_draw_crosshair, p_aspect_mode,
					p_nvg_lens);
	if (mask == nullptr) {
		return out;
	}
	const std::vector<opennova::hud::ScopeMaskVertex> *verts = scope_mask_verts(*mask, p_batch);
	if (verts == nullptr) {
		return out;
	}
	out.resize(static_cast<int64_t>(verts->size()));
	for (size_t i = 0; i < verts->size(); ++i) {
		out[static_cast<int64_t>(i)] = opennova::color_from_argb((*verts)[i].argb);
	}
	return out;
}

PackedInt32Array HudPos::scope_mask_indices(const Vector2 &p_surface, int p_screen_width,
		bool p_draw_crosshair, int p_batch, int p_aspect_mode, bool p_nvg_lens) {
	PackedInt32Array out;
	const opennova::hud::ScopeCircleMask *mask =
			scope_mask_build(p_surface, p_screen_width, p_draw_crosshair, p_aspect_mode,
					p_nvg_lens);
	if (mask == nullptr) {
		return out;
	}
	const std::vector<uint16_t> *ids = scope_mask_ids(*mask, p_batch);
	if (ids == nullptr) {
		return out;
	}
	out.resize(static_cast<int64_t>(ids->size()));
	for (size_t i = 0; i < ids->size(); ++i) {
		out[static_cast<int64_t>(i)] = static_cast<int32_t>((*ids)[i]);
	}
	return out;
}

PackedFloat32Array HudPos::scope_mask_frame(const Vector2 &p_surface, int p_screen_width,
		int p_aspect_mode) {
	PackedFloat32Array out;
	const int32_t w = static_cast<int32_t>(p_surface.x);
	const int32_t h = static_cast<int32_t>(p_surface.y);
	if (w <= 0 || h <= 0) {
		return out;
	}
	const opennova::hud::ScopeCircleMaskGeometry g = opennova::hud::scope_circle_mask_geometry(
			0, 0, w - 1, h - 1, p_screen_width > 0 ? p_screen_width : w, p_aspect_mode);
	out.resize(9);
	out[0] = g.center_x;
	out[1] = g.center_y;
	out[2] = g.ring_size;
	out[3] = g.radius_inner;
	out[4] = g.radius_outer;
	out[5] = g.scale_x;
	out[6] = g.scale_y;
	out[7] = g.arm_half_thickness;
	out[8] = g.tick_spacing;
	return out;
}

int HudPos::scoped_view_overlay(bool p_binoculars_view_active, bool p_sighted,
		bool p_scoped) {
	return static_cast<int>(opennova::hud::scoped_view_overlay(
			p_binoculars_view_active, p_sighted, p_scoped));
}

bool HudPos::scoped_selector_from_def(int p_weapon_flags, int p_weapon_flags2) {
	return opennova::hud::scoped_selector_from_def(
			static_cast<uint32_t>(p_weapon_flags), static_cast<uint32_t>(p_weapon_flags2));
}

bool HudPos::sighted_selector_from_def(int p_weapon_flags, bool p_slot_switching_from) {
	return opennova::hud::sighted_selector_from_def(
			static_cast<uint32_t>(p_weapon_flags), p_slot_switching_from);
}
