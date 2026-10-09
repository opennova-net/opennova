#include <formats/def/def_hudpos_write.h>

// HUDPOS.DEF: the writer (ADR 0003), the model's lines as HUD_ParseHudposToken reads them.

#include <base/io/crt_ftol.h>
#include <base/io/strutil.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>

namespace opennova::def {

namespace {

// How a key's values are written, each as the parser's arm reads its tokens [orig:
// HUD_ParseHudposToken @ 0x59F370].
enum class Form : uint8_t {
	Name,      // a name, strcpy'd from token 2 (the fonts @0x5A1599 / @0x5A15CD, HUDLS_BRACKET @0x59FE84)
	Ints,      // `count` numbers, each atof then ftol (HUDHEALTH @0x5A136F, HUDCLIP @0x5A0932, ...)
	PosAlign4, // x, y, hidden, then the alignment word (AMMOCOUNTPOS @0x59FC31, HUDWEAPONNAME @0x59F925)
	PosAlign3, // x, y, then the alignment word (BREATHTIME @0x59FB44, ZONEINFO @0x5A0636)
	Rgb,       // r, g, b (hud_textcolor @0x5A0F3D, the tag colours @0x5A1000..0x5A117C)
	Argb,      // a, r, g, b (the stance colours @0x5A0D65.., HUDHEALTHBORDER @0x5A13D9, AGLCOLOR @0x5A00AF)
	Reals,     // `count` doubles (alphafade @0x5A0872: atof, converted by its consumer)
};

struct Row {
	const char *key;
	Form form;
	size_t offset;
	size_t size;
	int count;
};

#define HUD_ROW(key, form, member, count) \
	Row { key, Form::form, offsetof(DefHudPosDef, member), sizeof(DefHudPosDef::member), count }

// The keys the model holds one value set of, in the writer's order, each spelled as the parser's
// _stricmp names it.
const Row kRows[] = {
	HUD_ROW("fonthud1_hi", Name, font_hi, 1),
	HUD_ROW("fonthud1_lo", Name, font_lo, 1),
	HUD_ROW("MRCLIPPYNORMAL", Ints, mrclippy_normal, 4),
	HUD_ROW("MRCLIPPYALTERNATE", Ints, mrclippy_alternate, 4),
	HUD_ROW("HUDHEALTH", Ints, health, 4),
	HUD_ROW("HUDHEAT", Ints, heat, 4),
	HUD_ROW("HUDPOWERBAR", Ints, powerbar, 4),
	HUD_ROW("STARTTIMER", Ints, starttimer, 4),
	HUD_ROW("HUDHEALTHBORDER", Argb, health_border, 1),
	HUD_ROW("HUDHEATBORDER", Argb, heat_border, 1),
	HUD_ROW("hud_textcolor", Rgb, hud_textcolor, 1),
	HUD_ROW("weapon_textcolor", Rgb, weapon_textcolor, 1),
	HUD_ROW("tagcolor_blueteam", Rgb, tagcolor_blueteam, 1),
	HUD_ROW("tagcolor_redteam", Rgb, tagcolor_redteam, 1),
	HUD_ROW("tagcolor_good", Rgb, tagcolor_good, 1),
	HUD_ROW("tagcolor_middle", Rgb, tagcolor_middle, 1),
	HUD_ROW("tagcolor_bad", Rgb, tagcolor_bad, 1),
	HUD_ROW("stanceicon_color", Argb, stanceicon_color, 1),
	HUD_ROW("stancecolor_good", Argb, stancecolor_good, 1),
	HUD_ROW("stancecolor_middle", Argb, stancecolor_middle, 1),
	HUD_ROW("stancecolor_bad", Argb, stancecolor_bad, 1),
	HUD_ROW("destaglcolor", Argb, dest_agl_color, 1),
	HUD_ROW("AGLCOLOR", Argb, agl_color, 1),
	HUD_ROW("HUDSPINMAPX1", Ints, spinmap_x1, 1),
	HUD_ROW("HUDSPINMAPX2", Ints, spinmap_x2, 1),
	HUD_ROW("HUDSPINMAPY1", Ints, spinmap_y1, 1),
	HUD_ROW("HUDSPINMAPY2", Ints, spinmap_y2, 1),
	HUD_ROW("SPINMAPWPDISTOFF", Ints, spinmap_wp_dist_off, 1),
	HUD_ROW("HUDFLAGCARRIER", PosAlign4, flag_carrier, 1),
	HUD_ROW("GAMEINFO", PosAlign4, game_info, 1),
	HUD_ROW("HUDWPDINFO", PosAlign4, wpd_info, 1),
	HUD_ROW("ZONEINFO", PosAlign3, zone_info, 1),
	HUD_ROW("EXPPOINTS", PosAlign4, exp_points, 1),
	HUD_ROW("CONNECTSTATUS", PosAlign4, connect_status, 1),
	HUD_ROW("HUDTEAMXY", PosAlign4, team_xy, 1),
	HUD_ROW("HUDPLAYERCOUNT", PosAlign4, player_count, 1),
	HUD_ROW("AMMOCOUNTPOS", PosAlign4, ammo_count_pos, 1),
	HUD_ROW("HUDWEAPONNAME", PosAlign4, weapon_name_pos, 1),
	HUD_ROW("mapcoords", PosAlign4, map_coords, 1),
	HUD_ROW("HUDTIMECLOCK", PosAlign4, time_clock, 1),
	HUD_ROW("BREATHTIME", PosAlign3, breath_time, 1),
	HUD_ROW("HUDLS_SYSTEM", Ints, hudls_system, 1),
	HUD_ROW("HUDLS_BRACKET", Name, hudls_bracket, 1),
	HUD_ROW("HUDLS_KEYOFST", Ints, hudls_keyofst, 2),
	HUD_ROW("HUDTITLEX", Ints, title_x, 1),
	HUD_ROW("HUDTITLEY", Ints, title_y, 1),
	HUD_ROW("HUDPINGX", Ints, ping_x, 1),
	HUD_ROW("HUDPINGY", Ints, ping_y, 1),
	HUD_ROW("HUDPINGRIGHT", Ints, ping_right, 1),
	HUD_ROW("HUDORDERS", Ints, orders, 2),
	HUD_ROW("SPECMODE_LABEL", Ints, spec_mode_label, 2),
	HUD_ROW("LFP_FLAGS", Ints, lfp_flags, 2),
	HUD_ROW("LFP_TAKEOVERDLG", Ints, lfp_takeover_dlg, 2),
	HUD_ROW("cargopos", Ints, cargo_pos, 2),
	HUD_ROW("PAUSEDPOS", Ints, paused_pos, 2),
	HUD_ROW("roomtkpos", Ints, roomtk_pos, 2),
	HUD_ROW("roomtktxtpos", Ints, roomtk_txt_pos, 2),
	HUD_ROW("HUDSTANCEPOS", Ints, stance_pos, 2),
	HUD_ROW("HUDVEHSTANCEPOS", Ints, veh_stance_pos, 2),
	HUD_ROW("HUDGeartext", Ints, gear_text, 2),
	HUD_ROW("HUDWPNICON", Ints, wpn_icon, 2),
	HUD_ROW("HUDCLIP", Ints, clip_pos, 2),
	HUD_ROW("HUDSCOPERANGEXY", Ints, scope_range, 2),
	HUD_ROW("HUDSCOPEZEROXY", Ints, scope_zero, 2),
	HUD_ROW("HUDSCOPEMAGXY", Ints, scope_mag, 2),
	HUD_ROW("ShowImpactDistPos", Ints, impact_dist_pos, 2),
	HUD_ROW("HUDCHATTEXT", Ints, chat_text, 2),
	HUD_ROW("HUDSYSTEXT", Ints, sys_text, 2),
	HUD_ROW("HUDCHLINE", Ints, hud_chline, 1),
	HUD_ROW("HUDAGLRADIUS", Ints, agl_radius, 1),
	HUD_ROW("HUDROCLEN", Ints, roc_len, 1),
	HUD_ROW("alphafade", Reals, alpha_fade, 3),
	HUD_ROW("HUDAGLTLRX", Ints, agl_tlrx, 2),
	HUD_ROW("HUDAGLYLEN", Ints, agl_ylen, 2),
};

#undef HUD_ROW

const char *const kNetworkIndicator = "NETWORKINDICATOR";
const char *const kMoreAvailable = "HUDLS_MOREAV";
const char *const kSlot = "HUDLS_SLOT";
const char *const kStance = "HUDSTANCE";
const char *const kDeclutterPrefix = "HUDDECLUT_";
const char *const kStaticFrame = "StaticFrame";
const char *const kParachute = "ParachuteIcon";
const char *const kArmor = "ArmorIcon";

const DefHudPosDef &unauthored() {
	return hudpos_unauthored();
}

const uint8_t *bytes_of(const DefHudPosDef &hud, const Row &row) {
	return reinterpret_cast<const uint8_t *>(&hud) + row.offset;
}

bool authored(const DefHudPosDef &hud, const Row &row) {
	return std::memcmp(bytes_of(hud, row), bytes_of(unauthored(), row), row.size) != 0;
}

// A name field as one token (hudpos_name_token), to its NUL or its capacity.
std::string name_token(const char *name, size_t capacity) {
	const auto *end = static_cast<const char *>(std::memchr(name, 0, capacity));
	return hudpos_name_token(std::string_view(name, size_t((end ? end : name + capacity) - name)));
}

void row_values(const DefHudPosDef &hud, const Row &row, std::vector<std::string> &out) {
	const uint8_t *at = bytes_of(hud, row);
	switch (row.form) {
	case Form::Name: out.push_back(name_token(reinterpret_cast<const char *>(at), row.size)); break;
	case Form::Ints: {
		const int *values = reinterpret_cast<const int *>(at);
		for (int i = 0; i < row.count; ++i) out.push_back(hudpos_number(values[i]));
		break;
	}
	case Form::PosAlign4:
	case Form::PosAlign3: {
		const int *values = reinterpret_cast<const int *>(at);
		const int numbers = row.form == Form::PosAlign4 ? 3 : 2;
		for (int i = 0; i < numbers; ++i) out.push_back(hudpos_number(values[i]));
		out.push_back(hudpos_align_word(values[numbers]));
		break;
	}
	case Form::Rgb:
	case Form::Argb: {
		const DefHudColor &color = *reinterpret_cast<const DefHudColor *>(at);
		if (row.form == Form::Argb) out.push_back(hudpos_number(color.a));
		out.push_back(hudpos_number(color.r));
		out.push_back(hudpos_number(color.g));
		out.push_back(hudpos_number(color.b));
		// The parser reads a fourth RGB field as the alpha (formats/def/def_scan.cpp parse_hud_color): a model
		// that holds another than the 255 it starts at was read with one.
		if (row.form == Form::Rgb && color.a != 255) out.push_back(hudpos_number(color.a));
		break;
	}
	case Form::Reals: {
		const double *values = reinterpret_cast<const double *>(at);
		for (int i = 0; i < row.count; ++i) out.push_back(hudpos_number(values[i]));
		break;
	}
	}
}

const Row *row_of(const std::string &key) {
	for (const Row &row : kRows)
		if (strutil::iequals(key, row.key)) return &row;
	return nullptr;
}

void network_values(const DefHudPosDef &hud, std::vector<std::string> &out) {
	for (int value : hud.network_indicator) out.push_back(hudpos_number(value));
}

void more_available_values(const DefHudPosDef &hud, std::vector<std::string> &out) {
	out.push_back(name_token(hud.hudls_moreav, sizeof(hud.hudls_moreav)));
	out.push_back(hudpos_number(int(hud.hudls_moreav_off[0])));
	out.push_back(hudpos_number(int(hud.hudls_moreav_off[1])));
}

void stance_values(const DefHudStance &stance, std::vector<std::string> &out) {
	out.push_back(hudpos_number(stance.id));
	out.push_back(hudpos_number(stance.offset_x));
	out.push_back(hudpos_number(stance.offset_y));
	out.push_back(name_token(stance.texture, sizeof(stance.texture)));
	out.push_back(name_token(stance.name, sizeof(stance.name)));
}

void declutter_values(const DefDeclutterEntry &row, std::vector<std::string> &out) {
	for (int flag : row.flags) out.push_back(hudpos_number(flag));
}

void graphic_values(const DefHudGraphic &graphic, std::vector<std::string> &out) {
	out.push_back(name_token(graphic.texture, sizeof(graphic.texture)));
	out.push_back(hudpos_number(graphic.x));
	out.push_back(hudpos_number(graphic.y));
}

bool graphic_authored(const DefHudGraphic &graphic) {
	static const DefHudGraphic none{};
	return std::memcmp(&graphic, &none, sizeof(graphic)) != 0;
}

// A block's lines: its sid, its names, its driver's place and its emplacement and seat pairs, each one the
// model holds [orig: the block arms @0x59F5CE..0x59F74E].
void vehicle_lines(const DefVehicleHudBlock &block, std::vector<HudposLine> &out) {
	out.push_back({ "VEHICLE_HUD", {} });
	auto name = [&](const char *key, const char *value, size_t capacity) {
		if (value[0]) out.push_back({ key, { name_token(value, capacity) } });
	};
	name("sid", block.sid, sizeof(block.sid));
	name("icon", block.icon, sizeof(block.icon));
	name("interface", block.interface_texture, sizeof(block.interface_texture));
	name("statictexture", block.static_texture, sizeof(block.static_texture));
	if (block.driver_x || block.driver_y)
		out.push_back({ "driver", { hudpos_number(block.driver_x), hudpos_number(block.driver_y) } });
	auto pairs = [&](const char *key, int count, const int *x, const int *y) {
		if (count <= 0) return;
		HudposLine line{ key, { hudpos_number(count) } };
		for (int i = 0; i < count; ++i) {
			line.values.push_back(hudpos_number(x[i]));
			line.values.push_back(hudpos_number(y[i]));
		}
		out.push_back(std::move(line));
	};
	pairs("emplace", block.emplace_count, block.emplace_x, block.emplace_y);
	pairs("seats", block.seat_count, block.seat_x, block.seat_y);
	out.push_back({ "VEHICLE_END", {} });
}

template <typename T>
bool same_rows(const T *a, size_t a_count, const T *b, size_t b_count) {
	if (a_count != b_count) return false;
	for (size_t i = 0; i < a_count; ++i)
		if (std::memcmp(&a[i], &b[i], sizeof(T)) != 0) return false;
	return true;
}

} // namespace

const DefHudPosDef &hudpos_unauthored() {
	static const DefHudPosFile file = [] {
		DefHudPosFile f;
		static const uint8_t nothing = 0;
		def_parse_hudpos_memory(&nothing, 0, &f);
		return f;
	}();
	return file.hud;
}

std::string hudpos_name_token(std::string_view name) {
	const bool quoted = name.find_first_of(" ,\t;") != std::string_view::npos || name.find("//") != std::string_view::npos;
	return quoted ? "\"" + std::string(name) + "\"" : std::string(name);
}

int hudpos_color_channels(const std::string &key) {
	const Row *row = row_of(key);
	if (!row) return 0;
	return row->form == Form::Rgb ? 3 : row->form == Form::Argb ? 4 : 0;
}

std::string hudpos_number(int value) {
	return std::to_string(value);
}

std::string hudpos_number(double value) {
	// The fewest digits that read back the same, written plain ("30", "1.5") where a plain form reads back.
	char text[40];
	for (int digits = 1; digits <= 17; ++digits) {
		std::snprintf(text, sizeof(text), "%.*g", digits, value);
		const bool plain = std::strchr(text, 'e') == nullptr;
		if ((plain || digits == 17) && io::retail_atof_n(text, std::strlen(text)) == value) break;
	}
	return text;
}

const char *hudpos_align_word(int align) {
	return align == 1 ? "right" : align == 2 ? "center" : "left";
}

std::vector<HudposLine> hudpos_lines(const DefHudPosDef &hud) {
	std::vector<HudposLine> out;
	for (const Row &row : kRows) {
		if (!authored(hud, row)) continue;
		HudposLine line{ row.key, {} };
		row_values(hud, row, line.values);
		out.push_back(std::move(line));
	}
	if (hud.hudls_moreav[0] || hud.hudls_moreav_off[0] || hud.hudls_moreav_off[1]) {
		HudposLine line{ kMoreAvailable, {} };
		more_available_values(hud, line.values);
		out.push_back(std::move(line));
	}
	for (int slot = 0; slot < 10; ++slot)
		if (hud.hudls_slot[slot][0] || hud.hudls_slot[slot][1])
			out.push_back({ kSlot, { hudpos_number(slot + 1), hudpos_number(hud.hudls_slot[slot][0]),
			                         hudpos_number(hud.hudls_slot[slot][1]) } });
	if (hud.network_indicator_present) {
		HudposLine line{ kNetworkIndicator, {} };
		network_values(hud, line.values);
		out.push_back(std::move(line));
	}
	for (size_t i = 0; i < hud.stances_count; ++i) {
		HudposLine line{ kStance, {} };
		stance_values(hud.stances[i], line.values);
		out.push_back(std::move(line));
	}
	for (size_t i = 0; i < hud.declutter_count; ++i) {
		HudposLine line{ std::string(kDeclutterPrefix) + hud.declutter[i].name, {} };
		declutter_values(hud.declutter[i], line.values);
		out.push_back(std::move(line));
	}
	for (size_t i = 0; i < hud.static_frames_count; ++i) {
		HudposLine line{ kStaticFrame, {} };
		graphic_values(hud.static_frames[i], line.values);
		out.push_back(std::move(line));
	}
	if (graphic_authored(hud.parachute_icon)) {
		HudposLine line{ kParachute, {} };
		graphic_values(hud.parachute_icon, line.values);
		out.push_back(std::move(line));
	}
	if (graphic_authored(hud.armor_icon)) {
		HudposLine line{ kArmor, {} };
		graphic_values(hud.armor_icon, line.values);
		out.push_back(std::move(line));
	}
	for (size_t i = 0; i < hud.vehicle_huds_count; ++i) vehicle_lines(hud.vehicle_huds[i], out);
	return out;
}

std::string hudpos_line_text(const HudposLine &line) {
	std::string out = line.key;
	for (size_t i = 0; i < line.values.size(); ++i) {
		out += i == 0 ? '\t' : ',';
		out += line.values[i];
	}
	return out;
}

bool hudpos_key_values(const DefHudPosDef &hud, const std::string &key, const std::string &first,
                       std::vector<std::string> &out) {
	out.clear();
	if (const Row *row = row_of(key)) {
		row_values(hud, *row, out);
		return true;
	}
	if (strutil::iequals(key, kNetworkIndicator)) {
		network_values(hud, out);
		return true;
	}
	if (strutil::iequals(key, kMoreAvailable)) {
		more_available_values(hud, out);
		return true;
	}
	if (strutil::iequals(key, kSlot)) {
		const int slot = io::retail_ftol_sse2(io::retail_atof_n(first.c_str(), first.size()));
		if (slot < 1 || slot > 10) return false;
		out = { hudpos_number(slot), hudpos_number(hud.hudls_slot[slot - 1][0]),
		        hudpos_number(hud.hudls_slot[slot - 1][1]) };
		return true;
	}
	if (strutil::iequals(key, kStance)) {
		const int id = io::retail_ftol_sse2(io::retail_atof_n(first.c_str(), first.size()));
		const DefHudStance *found = nullptr;
		for (size_t i = 0; i < hud.stances_count; ++i)
			if (hud.stances[i].id == id) found = &hud.stances[i];
		if (!found) return false;
		stance_values(*found, out);
		return true;
	}
	if (key.size() > std::strlen(kDeclutterPrefix) && strutil::starts_with_icase(key.c_str(), kDeclutterPrefix)) {
		const std::string name = key.substr(std::strlen(kDeclutterPrefix));
		DefDeclutterEntry none{};
		const DefDeclutterEntry *found = &none;
		for (size_t i = 0; i < hud.declutter_count; ++i)
			if (strutil::iequals(hud.declutter[i].name, name)) found = &hud.declutter[i];
		declutter_values(*found, out);
		return true;
	}
	if (strutil::iequals(key, kStaticFrame)) {
		if (hud.static_frames_count == 0) return false;
		graphic_values(hud.static_frames[hud.static_frames_count - 1], out);
		return true;
	}
	if (strutil::iequals(key, kParachute) || strutil::iequals(key, kArmor)) {
		graphic_values(strutil::iequals(key, kParachute) ? hud.parachute_icon : hud.armor_icon, out);
		return true;
	}
	return false;
}

bool def_hudpos_same(const DefHudPosDef &a, const DefHudPosDef &b, std::string *difference) {
	auto differ = [&](const std::string &what) {
		if (difference) *difference = what;
		return false;
	};
	for (const Row &row : kRows)
		if (std::memcmp(bytes_of(a, row), bytes_of(b, row), row.size) != 0) return differ(row.key);
	if (std::strncmp(a.hudls_moreav, b.hudls_moreav, sizeof(a.hudls_moreav)) != 0 ||
	    std::memcmp(a.hudls_moreav_off, b.hudls_moreav_off, sizeof(a.hudls_moreav_off)) != 0)
		return differ(kMoreAvailable);
	if (std::memcmp(a.hudls_slot, b.hudls_slot, sizeof(a.hudls_slot)) != 0) return differ(kSlot);
	if (a.network_indicator_present != b.network_indicator_present ||
	    std::memcmp(a.network_indicator, b.network_indicator, sizeof(a.network_indicator)) != 0)
		return differ(kNetworkIndicator);
	if (!same_rows(a.stances, a.stances_count, b.stances, b.stances_count)) return differ(kStance);
	if (!same_rows(a.declutter, a.declutter_count, b.declutter, b.declutter_count)) return differ("HUDDECLUT_");
	if (!same_rows(a.static_frames, a.static_frames_count, b.static_frames, b.static_frames_count))
		return differ(kStaticFrame);
	if (std::memcmp(&a.parachute_icon, &b.parachute_icon, sizeof(a.parachute_icon)) != 0) return differ(kParachute);
	if (std::memcmp(&a.armor_icon, &b.armor_icon, sizeof(a.armor_icon)) != 0) return differ(kArmor);
	if (!same_rows(a.vehicle_huds, a.vehicle_huds_count, b.vehicle_huds, b.vehicle_huds_count))
		return differ("VEHICLE_HUD");
	// Every value the rows above name: nothing of the model is left out of the comparison (the scalars
	// end where the stances begin; both models start zeroed, their padding with them).
	if (std::memcmp(&a, &b, offsetof(DefHudPosDef, stances)) != 0) return differ("an unnamed value");
	if (difference) difference->clear();
	return true;
}

DefWriteResult def_write_hudpos(const DefHudPosFile &file) {
	DefWriteResult out;
	for (const HudposLine &line : hudpos_lines(file.hud)) {
		out.text += hudpos_line_text(line);
		out.text += "\r\n";
	}
	// Read back through the family's parser: what does not read the same is not written.
	DefHudPosFile back;
	std::string difference;
	if (def_parse_hudpos_memory(reinterpret_cast<const uint8_t *>(out.text.data()), out.text.size(), &back) != 0 ||
	    !def_hudpos_same(file.hud, back.hud, &difference)) {
		DefIssue issue;
		issue.code = DefIssueCode::Unrepresentable;
		issue.field = difference;
		issue.message = "The layout's " + (difference.empty() ? std::string("text") : difference) +
		                " does not read back the same once written (a name the tokenizer cannot carry, or a "
		                "HUDSTANCE with no name, which the parser reads only with one).";
		out.diagnostics.push_back(std::move(issue));
	}
	def_free_hudpos(&back);
	return out;
}

} // namespace opennova::def
