#include "nova_hud_pos.h"

#include "resource_index/nova_resource_root.h"
#include "util/nova_data_format.h"

#include <def/def.h>

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

void NovaHudPos::_bind_methods() {
	ClassDB::bind_method(D_METHOD("load", "path"), &NovaHudPos::load);
	ClassDB::bind_method(D_METHOD("load_from_resource_root", "resource_root", "name"), &NovaHudPos::load_from_resource_root);
	ClassDB::bind_method(D_METHOD("is_loaded"), &NovaHudPos::is_loaded);
	ClassDB::bind_method(D_METHOD("get_source_path"), &NovaHudPos::get_source_path);
	ClassDB::bind_method(D_METHOD("get_last_error"), &NovaHudPos::get_last_error);
	ClassDB::bind_method(D_METHOD("get_font_hi"), &NovaHudPos::get_font_hi);
	ClassDB::bind_method(D_METHOD("get_font_lo"), &NovaHudPos::get_font_lo);
	ClassDB::bind_method(D_METHOD("get_health_rect"), &NovaHudPos::get_health_rect);
	ClassDB::bind_method(D_METHOD("get_stance_pos"), &NovaHudPos::get_stance_pos);
	ClassDB::bind_method(D_METHOD("get_veh_stance_pos"), &NovaHudPos::get_veh_stance_pos);
	ClassDB::bind_method(D_METHOD("get_stances"), &NovaHudPos::get_stances);
	ClassDB::bind_method(D_METHOD("get_static_frames"), &NovaHudPos::get_static_frames);
	ClassDB::bind_method(D_METHOD("get_parachute_icon"), &NovaHudPos::get_parachute_icon);
	ClassDB::bind_method(D_METHOD("get_armor_icon"), &NovaHudPos::get_armor_icon);
	ClassDB::bind_method(D_METHOD("get_spinmap_bounds"), &NovaHudPos::get_spinmap_bounds);
	ClassDB::bind_method(D_METHOD("get_colors"), &NovaHudPos::get_colors);
	ClassDB::bind_method(D_METHOD("to_dictionary"), &NovaHudPos::to_dictionary);

	BIND_CONSTANT(DESIGN_WIDTH);
	BIND_CONSTANT(DESIGN_HEIGHT);
}

NovaHudPos::NovaHudPos() {}

NovaHudPos::~NovaHudPos() {
	clear_();
}

void NovaHudPos::clear_() {
	if (loaded_) {
		def_free_hudpos(&file_);
	}
	file_ = {};
	loaded_ = false;
}

Error NovaHudPos::parse_bytes_(const PackedByteArray &bytes, const String &src) {
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

Error NovaHudPos::load(const String &path) {
	PackedByteArray bytes;
	if (!read_nova_payload_file(path, bytes)) {
		clear_();
		last_error_ = String("Cannot open hudpos.def: ") + path;
		return ERR_CANT_OPEN;
	}
	return parse_bytes_(bytes, path);
}

Error NovaHudPos::load_from_resource_root(const Ref<NovaResourceRoot> &p_resource_root, const String &p_name) {
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

bool NovaHudPos::is_loaded() const {
	return loaded_;
}

String NovaHudPos::get_source_path() const {
	return source_path_;
}

String NovaHudPos::get_last_error() const {
	return last_error_;
}

String NovaHudPos::get_font_hi() const {
	return loaded_ ? String(file_.hud.font_hi) : String();
}

String NovaHudPos::get_font_lo() const {
	return loaded_ ? String(file_.hud.font_lo) : String();
}

Rect2i NovaHudPos::get_health_rect() const {
	return loaded_ ? rect_from_corners(file_.hud.health) : Rect2i();
}

Vector2i NovaHudPos::get_stance_pos() const {
	return loaded_ ? pos2(file_.hud.stance_pos) : Vector2i();
}

Vector2i NovaHudPos::get_veh_stance_pos() const {
	return loaded_ ? pos2(file_.hud.veh_stance_pos) : Vector2i();
}

Array NovaHudPos::get_stances() const {
	Array out;
	if (!loaded_) {
		return out;
	}
	for (size_t i = 0; i < file_.hud.stances_count; ++i) {
		out.push_back(stance_to_dict(file_.hud.stances[i]));
	}
	return out;
}

Array NovaHudPos::get_static_frames() const {
	Array out;
	if (!loaded_) {
		return out;
	}
	for (size_t i = 0; i < file_.hud.static_frames_count; ++i) {
		out.push_back(graphic_to_dict(file_.hud.static_frames[i]));
	}
	return out;
}

Dictionary NovaHudPos::get_parachute_icon() const {
	return loaded_ ? graphic_to_dict(file_.hud.parachute_icon) : Dictionary();
}

Dictionary NovaHudPos::get_armor_icon() const {
	return loaded_ ? graphic_to_dict(file_.hud.armor_icon) : Dictionary();
}

Rect2i NovaHudPos::get_spinmap_bounds() const {
	if (!loaded_) {
		return Rect2i();
	}
	const DefHudPosDef &h = file_.hud;
	return Rect2i(h.spinmap_x1, h.spinmap_y1, h.spinmap_x2 - h.spinmap_x1, h.spinmap_y2 - h.spinmap_y1);
}

Dictionary NovaHudPos::get_colors() const {
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

Dictionary NovaHudPos::to_dictionary() const {
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
