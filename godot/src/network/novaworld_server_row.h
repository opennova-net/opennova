#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <net/novaworld/gsb.h>

namespace godot {

// One server-browser row: the engine's GsbServerEntry (the GSB list record)
// as a typed record. The client fills rows from the parsed list; the setters
// exist so a test or a literal fixture can author one. Text fields are the
// host's cp1252 bytes on the wire and decode on read; a value set from
// GDScript is stored as-is (ASCII fixtures round-trip).
class NovaWorldServerRow : public RefCounted {
	GDCLASS(NovaWorldServerRow, RefCounted)

	opennova::GsbServerEntry value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::GsbServerEntry &p_value) { value_ = p_value; }
	const opennova::GsbServerEntry &value() const { return value_; }

	int get_rid() const { return static_cast<int>(value_.rid); }
	void set_rid(int p_rid) { value_.rid = static_cast<uint32_t>(p_rid); }
	int get_players() const { return value_.players; }
	void set_players(int p_players) { value_.players = p_players; }
	int get_max_players() const { return value_.max_players; }
	void set_max_players(int p_max) { value_.max_players = p_max; }
	PackedStringArray get_player_names() const;
	void set_player_names(const PackedStringArray &p_names);

#define NOVAWORLD_ROW_TEXT(m_name, m_field)     \
	String get_##m_name() const;                \
	void set_##m_name(const String &p_value);
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
};

} // namespace godot
