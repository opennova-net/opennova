#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

#include <formats/playersav/weapon_sav.h>

namespace godot {

// One side (blue / red) of a weapon.sav character header
// (Simulation.read_weapon_profile_summary): a value wrapper over the
// engine's playersav::Side — the class byte, the avatar ids (nationality,
// division, the packed selection) and the kit page the class byte selects.
class WeaponProfileSide : public RefCounted {
	GDCLASS(WeaponProfileSide, RefCounted)

	opennova::playersav::Side value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::playersav::Side &p_value) { value_ = p_value; }

	int get_player_class() const { return static_cast<int>(value_.player_class); }
	int get_avatar_a() const { return static_cast<int>(value_.avatar_a); }
	int get_avatar_b() const { return static_cast<int>(value_.avatar_b); }
	int get_avatar_packed() const { return static_cast<int>(value_.avatar_packed); }
	// The SELECTED page's weapon names — the one the class byte picks.
	PackedStringArray get_kit() const;
};

// The two-side summary of slot 0 (the menu boot seam over the same
// five-record file load_weapon_profile reads): the read outcome (`error` /
// `loaded`) beside the slot's playersav::Record, whose blue and red sides
// read as WeaponProfileSide records.
class WeaponProfileSummary : public RefCounted {
	GDCLASS(WeaponProfileSummary, RefCounted)

	opennova::playersav::Record value_;
	int error_ = 0;
	bool loaded_ = false;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::playersav::Record &p_value, int p_error, bool p_loaded) {
		value_ = p_value;
		error_ = p_error;
		loaded_ = p_loaded;
	}

	int get_error() const { return error_; }
	bool is_loaded() const { return loaded_; }
	Ref<WeaponProfileSide> get_blue() const;
	Ref<WeaponProfileSide> get_red() const;
};

} // namespace godot
