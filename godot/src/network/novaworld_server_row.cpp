#include "network/novaworld_server_row.h"

#include "util/string_convert.h"

namespace godot {

PackedStringArray NovaWorldServerRow::get_player_names() const {
	PackedStringArray out;
	for (const std::string &player : value_.player_names) {
		out.push_back(opennova::cp1252_to_gd(player));
	}
	return out;
}

void NovaWorldServerRow::set_player_names(const PackedStringArray &p_names) {
	value_.player_names.clear();
	for (int i = 0; i < p_names.size(); ++i) {
		value_.player_names.push_back(opennova::to_std(p_names[i]));
	}
}

#define NOVAWORLD_ROW_TEXT(m_name, m_field)                                                 \
	String NovaWorldServerRow::get_##m_name() const { return opennova::cp1252_to_gd(value_.m_field); } \
	void NovaWorldServerRow::set_##m_name(const String &p_value) { value_.m_field = opennova::to_std(p_value); }
NOVAWORLD_ROW_TEXT(ip, ip)
NOVAWORLD_ROW_TEXT(name, server_name)
NOVAWORLD_ROW_TEXT(game_type, game_type)
NOVAWORLD_ROW_TEXT(mission_name, mission_name)
NOVAWORLD_ROW_TEXT(region, region)
NOVAWORLD_ROW_TEXT(dedicated, dedicated)
NOVAWORLD_ROW_TEXT(time_left, time_left)
NOVAWORLD_ROW_TEXT(password, password)
NOVAWORLD_ROW_TEXT(country, country)
NOVAWORLD_ROW_TEXT(msg, msg)
NOVAWORLD_ROW_TEXT(age, age)
NOVAWORLD_ROW_TEXT(time_of_day, time_of_day)
NOVAWORLD_ROW_TEXT(stat, stat)
NOVAWORLD_ROW_TEXT(level_range, level_range)
NOVAWORLD_ROW_TEXT(locked, locked)
NOVAWORLD_ROW_TEXT(tracers, tracers)
NOVAWORLD_ROW_TEXT(skins, skins)
NOVAWORLD_ROW_TEXT(bb_mode, bb_mode)
NOVAWORLD_ROW_TEXT(mod, mod)
NOVAWORLD_ROW_TEXT(pix, pix)
NOVAWORLD_ROW_TEXT(pb_server, pb_server)
NOVAWORLD_ROW_TEXT(ver1, ver1)
NOVAWORLD_ROW_TEXT(exp, exp)
NOVAWORLD_ROW_TEXT(exp_bits, exp_bits)
NOVAWORLD_ROW_TEXT(joicon2, joicon2)
#undef NOVAWORLD_ROW_TEXT

void NovaWorldServerRow::_bind_methods() {
#define NOVAWORLD_ROW_PROP(m_type, m_name)                                                   \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &NovaWorldServerRow::set_##m_name); \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &NovaWorldServerRow::get_##m_name);        \
	ADD_PROPERTY(PropertyInfo(m_type, #m_name), "set_" #m_name, "get_" #m_name)
	NOVAWORLD_ROW_PROP(Variant::INT, rid);
	NOVAWORLD_ROW_PROP(Variant::STRING, ip);
	NOVAWORLD_ROW_PROP(Variant::STRING, name);
	NOVAWORLD_ROW_PROP(Variant::STRING, game_type);
	NOVAWORLD_ROW_PROP(Variant::STRING, mission_name);
	NOVAWORLD_ROW_PROP(Variant::STRING, region);
	NOVAWORLD_ROW_PROP(Variant::INT, players);
	NOVAWORLD_ROW_PROP(Variant::INT, max_players);
	NOVAWORLD_ROW_PROP(Variant::STRING, dedicated);
	NOVAWORLD_ROW_PROP(Variant::STRING, time_left);
	NOVAWORLD_ROW_PROP(Variant::STRING, password);
	NOVAWORLD_ROW_PROP(Variant::STRING, country);
	NOVAWORLD_ROW_PROP(Variant::STRING, msg);
	NOVAWORLD_ROW_PROP(Variant::STRING, age);
	NOVAWORLD_ROW_PROP(Variant::STRING, time_of_day);
	NOVAWORLD_ROW_PROP(Variant::STRING, stat);
	NOVAWORLD_ROW_PROP(Variant::STRING, level_range);
	NOVAWORLD_ROW_PROP(Variant::STRING, locked);
	NOVAWORLD_ROW_PROP(Variant::STRING, tracers);
	NOVAWORLD_ROW_PROP(Variant::STRING, skins);
	NOVAWORLD_ROW_PROP(Variant::STRING, bb_mode);
	NOVAWORLD_ROW_PROP(Variant::STRING, mod);
	NOVAWORLD_ROW_PROP(Variant::STRING, pix);
	NOVAWORLD_ROW_PROP(Variant::STRING, pb_server);
	NOVAWORLD_ROW_PROP(Variant::STRING, ver1);
	NOVAWORLD_ROW_PROP(Variant::STRING, exp);
	NOVAWORLD_ROW_PROP(Variant::STRING, exp_bits);
	NOVAWORLD_ROW_PROP(Variant::STRING, joicon2);
	NOVAWORLD_ROW_PROP(Variant::PACKED_STRING_ARRAY, player_names);
#undef NOVAWORLD_ROW_PROP
}

} // namespace godot
