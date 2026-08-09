#include "nova_hud_pos.h"

#include "resource_index/nova_resource_root.h"
#include "util/nova_data_format.h"

#include <def/def.h>
#include <hud/hud_math.h>

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

double godot::HudPos::percent_to_alpha() {
	return opennova::hud::kPercentToAlpha;
}

using namespace godot;

namespace {

// hudpos.def [4] rects are stored as corners (x1,y1,x2,y2); the witnessed draws
// read them that way. [orig: HUD_DrawHealthBar @0x5a2e50 reads dword_27237C8/CC/D0/D4]
Rect2i rect_from_corners(const int v[4]) {
	return Rect2i(v[0], v[1], v[2] - v[0], v[3] - v[1]);
}

// HUDPOWERBAR alone is authored x,y,w,h — its witnessed consumer adds the third
// and fourth dwords to the anchor (JOX authors "20,720,72,11": as corners the
// height would be negative). [orig: HUD_DrawPowerThrowChargeBar @0x599830 draws
// (x, y)-(x+w, y+h) from dword_27237EC..F8]
Rect2i rect_from_xywh(const int v[4]) {
	return Rect2i(v[0], v[1], v[2], v[3]);
}

// Positioned text tokens keep the original's 4-dword layout: x, y, hidden
// (0 = draw), alignment (0=left 1=right 2=center). [orig: AMMOCOUNTPOS parse
// @0x59fc3d; the draws gate on the hidden dword @0x5939f3]
Vector4i pos4(const int v[4]) {
	return Vector4i(v[0], v[1], v[2], v[3]);
}

Vector2i pos2(const int v[2]) {
	return Vector2i(v[0], v[1]);
}

// DefHudColor is 0..255 ARGB-ish ints; expose a Godot-normalized Color.
Color to_color(const DefHudColor &c) {
	return Color(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f);
}

Dictionary stance_to_dict(const DefHudStance &s) {
	Dictionary d;
	d["id"] = s.id;
	d["offset"] = Vector2i(s.offset_x, s.offset_y);
	d["texture"] = String(s.texture);
	d["name"] = String(s.name);
	return d;
}

Dictionary graphic_to_dict(const DefHudGraphic &g) {
	Dictionary d;
	d["texture"] = String(g.texture);
	d["pos"] = Vector2i(g.x, g.y);
	return d;
}

} // namespace

void HudPos::_bind_methods() {
	ClassDB::bind_static_method("HudPos", D_METHOD("scale_point", "design", "surface"), &HudPos::scale_point);
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
	ClassDB::bind_static_method("HudPos", D_METHOD("half_bright", "color"), &HudPos::half_bright);
	ClassDB::bind_static_method("HudPos", D_METHOD("format_ammo", "clip", "reserve", "capacity"), &HudPos::format_ammo);
	ClassDB::bind_static_method("HudPos", D_METHOD("weapon_name_x_nudge", "narrow_surface", "align"), &HudPos::weapon_name_x_nudge);
	ClassDB::bind_static_method("HudPos", D_METHOD("round_icon_count", "clip", "reserve", "capacity", "divisor"), &HudPos::round_icon_count);
	ClassDB::bind_static_method("HudPos", D_METHOD("folded_reserve", "clip", "reserve", "capacity"), &HudPos::folded_reserve);
	ClassDB::bind_static_method("HudPos", D_METHOD("waypoint_distance_m", "ground_delta"), &HudPos::waypoint_distance_m);
	ClassDB::bind_static_method("HudPos", D_METHOD("heat_fill_span", "extent_px", "heat"), &HudPos::heat_fill_span);
	ClassDB::bind_static_method("HudPos", D_METHOD("heat_bar_is_horizontal", "bar_size"), &HudPos::heat_bar_is_horizontal);
	ClassDB::bind_static_method("HudPos", D_METHOD("power_throw_progress_fp16", "held_ticks"), &HudPos::power_throw_progress_fp16);
	ClassDB::bind_static_method("HudPos", D_METHOD("power_fill_span", "progress_fp16", "extent_px"), &HudPos::power_fill_span);
	ClassDB::bind_static_method("HudPos", D_METHOD("loading_bar_step", "displayed", "reported"), &HudPos::loading_bar_step);
	ClassDB::bind_static_method("HudPos", D_METHOD("loading_bar_fill_span", "x", "w", "displayed"), &HudPos::loading_bar_fill_span);
	ClassDB::bind_static_method("HudPos", D_METHOD("crosshair_spread_px_fp16", "spread_fp16", "fov_deg", "screen_w"), &HudPos::crosshair_spread_px_fp16);
	ClassDB::bind_static_method("HudPos", D_METHOD("crosshair_total_spread_fp16", "error_fp16", "recoil_pitch_bam", "weight_spread_bam"), &HudPos::crosshair_total_spread_fp16);
	ClassDB::bind_static_method("HudPos", D_METHOD("crosshair_error_row", "stance", "scoped"), &HudPos::crosshair_error_row);
	ClassDB::bind_static_method("HudPos", D_METHOD("crosshair_should_draw", "aimed", "keep_while_aimed"), &HudPos::crosshair_should_draw);
	ClassDB::bind_method(D_METHOD("load", "path"), &HudPos::load);
	ClassDB::bind_method(D_METHOD("load_from_resource_root", "resource_root", "name"), &HudPos::load_from_resource_root);
	ClassDB::bind_method(D_METHOD("is_loaded"), &HudPos::is_loaded);
	ClassDB::bind_method(D_METHOD("get_source_path"), &HudPos::get_source_path);
	ClassDB::bind_method(D_METHOD("get_last_error"), &HudPos::get_last_error);
	ClassDB::bind_method(D_METHOD("get_font_hi"), &HudPos::get_font_hi);
	ClassDB::bind_method(D_METHOD("get_font_lo"), &HudPos::get_font_lo);
	ClassDB::bind_method(D_METHOD("get_health_rect"), &HudPos::get_health_rect);
	ClassDB::bind_method(D_METHOD("get_stance_pos"), &HudPos::get_stance_pos);
	ClassDB::bind_method(D_METHOD("get_veh_stance_pos"), &HudPos::get_veh_stance_pos);
	ClassDB::bind_method(D_METHOD("get_stances"), &HudPos::get_stances);
	ClassDB::bind_method(D_METHOD("get_static_frames"), &HudPos::get_static_frames);
	ClassDB::bind_method(D_METHOD("get_parachute_icon"), &HudPos::get_parachute_icon);
	ClassDB::bind_method(D_METHOD("get_armor_icon"), &HudPos::get_armor_icon);
	ClassDB::bind_method(D_METHOD("get_spinmap_bounds"), &HudPos::get_spinmap_bounds);
	ClassDB::bind_method(D_METHOD("get_colors"), &HudPos::get_colors);
	ClassDB::bind_method(D_METHOD("to_dictionary"), &HudPos::to_dictionary);

	BIND_CONSTANT(DESIGN_WIDTH);
	BIND_CONSTANT(DESIGN_HEIGHT);
	BIND_CONSTANT(MESSAGE_LIFE_TICKS);
	BIND_CONSTANT(MESSAGE_EXPIRY_STAGGER);
	BIND_CONSTANT(MESSAGE_TEXT_MAX);
	BIND_CONSTANT(MESSAGE_SLOT_COUNT);
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
	if (bytes.is_empty()) {
		last_error_ = String("hudpos.def is empty: ") + src;
		return ERR_FILE_CANT_READ;
	}
	if (def_parse_hudpos_memory(bytes.ptr(), static_cast<size_t>(bytes.size()), &file_) != 0) {
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
	if (bytes.is_empty()) {
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

String HudPos::get_font_hi() const {
	return loaded_ ? String(file_.hud.font_hi) : String();
}

String HudPos::get_font_lo() const {
	return loaded_ ? String(file_.hud.font_lo) : String();
}

Rect2i HudPos::get_health_rect() const {
	return loaded_ ? rect_from_corners(file_.hud.health) : Rect2i();
}

Vector2i HudPos::get_stance_pos() const {
	return loaded_ ? pos2(file_.hud.stance_pos) : Vector2i();
}

Vector2i HudPos::get_veh_stance_pos() const {
	return loaded_ ? pos2(file_.hud.veh_stance_pos) : Vector2i();
}

Array HudPos::get_stances() const {
	Array out;
	if (!loaded_) {
		return out;
	}
	for (size_t i = 0; i < file_.hud.stances_count; ++i) {
		out.push_back(stance_to_dict(file_.hud.stances[i]));
	}
	return out;
}

Array HudPos::get_static_frames() const {
	Array out;
	if (!loaded_) {
		return out;
	}
	for (size_t i = 0; i < file_.hud.static_frames_count; ++i) {
		out.push_back(graphic_to_dict(file_.hud.static_frames[i]));
	}
	return out;
}

Dictionary HudPos::get_parachute_icon() const {
	return loaded_ ? graphic_to_dict(file_.hud.parachute_icon) : Dictionary();
}

Dictionary HudPos::get_armor_icon() const {
	return loaded_ ? graphic_to_dict(file_.hud.armor_icon) : Dictionary();
}

Rect2i HudPos::get_spinmap_bounds() const {
	if (!loaded_) {
		return Rect2i();
	}
	const DefHudPosDef &h = file_.hud;
	return Rect2i(h.spinmap_x1, h.spinmap_y1, h.spinmap_x2 - h.spinmap_x1, h.spinmap_y2 - h.spinmap_y1);
}

Dictionary HudPos::get_colors() const {
	Dictionary out;
	if (!loaded_) {
		return out;
	}
	const DefHudPosDef &h = file_.hud;
	out["health_border"] = to_color(h.health_border);
	out["heat_border"] = to_color(h.heat_border);
	out["hud_textcolor"] = to_color(h.hud_textcolor);
	out["weapon_textcolor"] = to_color(h.weapon_textcolor);
	out["tagcolor_blueteam"] = to_color(h.tagcolor_blueteam);
	out["tagcolor_redteam"] = to_color(h.tagcolor_redteam);
	out["tagcolor_good"] = to_color(h.tagcolor_good);
	out["tagcolor_middle"] = to_color(h.tagcolor_middle);
	out["tagcolor_bad"] = to_color(h.tagcolor_bad);
	out["stanceicon_color"] = to_color(h.stanceicon_color);
	out["stancecolor_good"] = to_color(h.stancecolor_good);
	out["stancecolor_middle"] = to_color(h.stancecolor_middle);
	out["stancecolor_bad"] = to_color(h.stancecolor_bad);
	out["dest_agl_color"] = to_color(h.dest_agl_color);
	out["agl_color"] = to_color(h.agl_color);
	return out;
}

Dictionary HudPos::to_dictionary() const {
	Dictionary out;
	if (!loaded_) {
		return out;
	}
	const DefHudPosDef &h = file_.hud;

	Dictionary fonts;
	fonts["hi"] = String(h.font_hi);
	fonts["lo"] = String(h.font_lo);
	out["fonts"] = fonts;

	Dictionary rects;
	rects["health"] = rect_from_corners(h.health);
	rects["heat"] = rect_from_corners(h.heat);
	rects["powerbar"] = rect_from_xywh(h.powerbar);
	rects["starttimer"] = rect_from_corners(h.starttimer);
	rects["mrclippy_normal"] = rect_from_corners(h.mrclippy_normal);
	rects["mrclippy_alternate"] = rect_from_corners(h.mrclippy_alternate);
	out["rects"] = rects;

	// [4] = (x, y, hidden, alignment); [2] = (x, y).
	Dictionary positions;
	positions["flag_carrier"] = pos4(h.flag_carrier);
	positions["game_info"] = pos4(h.game_info);
	positions["wpd_info"] = pos4(h.wpd_info);
	positions["zone_info"] = pos4(h.zone_info);
	positions["exp_points"] = pos4(h.exp_points);
	positions["connect_status"] = pos4(h.connect_status);
	positions["team_xy"] = pos4(h.team_xy);
	positions["player_count"] = pos4(h.player_count);
	positions["ammo_count"] = pos4(h.ammo_count_pos);
	positions["weapon_name"] = pos4(h.weapon_name_pos);
	positions["map_coords"] = pos4(h.map_coords);
	positions["time_clock"] = pos4(h.time_clock);
	positions["breath_time"] = pos4(h.breath_time);
	positions["orders"] = pos2(h.orders);
	positions["spec_mode_label"] = pos2(h.spec_mode_label);
	positions["cargo"] = pos2(h.cargo_pos);
	positions["stance"] = pos2(h.stance_pos);
	positions["veh_stance"] = pos2(h.veh_stance_pos);
	positions["gear_text"] = pos2(h.gear_text);
	positions["wpn_icon"] = pos2(h.wpn_icon);
	positions["clip"] = pos2(h.clip_pos);
	positions["scope_range"] = pos2(h.scope_range);
	positions["scope_zero"] = pos2(h.scope_zero);
	positions["scope_mag"] = pos2(h.scope_mag);
	positions["impact_dist"] = pos2(h.impact_dist_pos);
	positions["chat_text"] = pos2(h.chat_text);
	positions["sys_text"] = pos2(h.sys_text);
	positions["title"] = Vector2i(h.title_x, h.title_y);
	positions["ping"] = Vector2i(h.ping_x, h.ping_y);
	out["positions"] = positions;

	out["spinmap"] = get_spinmap_bounds();
	out["colors"] = get_colors();
	out["stances"] = get_stances();
	out["static_frames"] = get_static_frames();
	out["parachute_icon"] = get_parachute_icon();
	out["armor_icon"] = get_armor_icon();

	Array declutter;
	for (size_t i = 0; i < h.declutter_count; ++i) {
		Dictionary d;
		d["name"] = String(h.declutter[i].name);
		Array flags;
		for (int f = 0; f < 4; ++f) {
			flags.push_back(h.declutter[i].flags[f]);
		}
		d["flags"] = flags;
		declutter.push_back(d);
	}
	out["declutter"] = declutter;

	Dictionary misc;
	misc["hud_chline"] = h.hud_chline;
	misc["agl_radius"] = h.agl_radius;
	misc["roc_len"] = h.roc_len;
	misc["ping_right"] = h.ping_right;
	// ALPHAFADE raw file fields: base%, max%, seconds — floats, because the
	// original reads them via atof and the fraction survives into the stored
	// base*2.55 / max*2.55 (0..255 alpha) and seconds*62 (ticks) converts;
	// consumers do that conversion. [orig: alphafade parse @0x5a0882..0x5a08c2
	// -> 0x2723614/18/1C]
	misc["alpha_fade"] = Vector3(h.alpha_fade[0], h.alpha_fade[1], h.alpha_fade[2]);
	out["misc"] = misc;

	return out;
}
Vector2 HudPos::scale_point(const Vector2 &p_design, const Vector2 &p_surface) {
	return Vector2(
			static_cast<float>(opennova::hud::scale_axis(
					p_design.x, p_surface.x, opennova::hud::kDesignWidth)),
			static_cast<float>(opennova::hud::scale_axis(
					p_design.y, p_surface.y, opennova::hud::kDesignHeight)));
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

Color HudPos::half_bright(const Color &p_color) {
	const auto channel = [](float c) {
		const int v = static_cast<int>(c * 255.0f + 0.5f);
		return static_cast<uint32_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
	};
	const uint32_t argb = 0xFF000000u | (channel(p_color.r) << 16) |
			(channel(p_color.g) << 8) | channel(p_color.b);
	const uint32_t hb = opennova::hud::half_bright_argb(argb);
	return Color(((hb >> 16) & 0xFFu) / 255.0f, ((hb >> 8) & 0xFFu) / 255.0f,
			(hb & 0xFFu) / 255.0f, 1.0f);
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

int HudPos::waypoint_distance_m(const Vector2 &p_ground_delta) {
	return opennova::hud::waypoint_distance_m(p_ground_delta.x, p_ground_delta.y);
}

int HudPos::heat_fill_span(int p_extent_px, int p_heat) {
	return opennova::hud::heat_fill_span(p_extent_px, p_heat);
}

bool HudPos::heat_bar_is_horizontal(const Vector2 &p_bar_size) {
	return opennova::hud::heat_bar_is_horizontal(p_bar_size.x, p_bar_size.y);
}

int HudPos::power_throw_progress_fp16(int p_held_ticks) {
	return opennova::hud::power_throw_progress_fp16(p_held_ticks);
}

int HudPos::power_fill_span(int p_progress_fp16, int p_extent_px) {
	return opennova::hud::power_fill_span(p_progress_fp16, p_extent_px);
}

int HudPos::loading_bar_step(int p_displayed, int p_reported) {
	return opennova::hud::loading_bar_step(p_displayed, p_reported);
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
