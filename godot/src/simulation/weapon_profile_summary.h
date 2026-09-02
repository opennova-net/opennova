#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

namespace godot {

// One side (blue / red) of a weapon.sav character header
// (Simulation.read_weapon_profile_summary / get_weapon_profile_summary): the
// class byte, the avatar ids (nationality, division, the packed selection)
// and, for the live profile view, the kit page the class byte selects.
// Read-write so a harness authors one.
class WeaponProfileSide : public RefCounted {
	GDCLASS(WeaponProfileSide, RefCounted)

public:
	int get_player_class() const { return player_class_; }
	void set_player_class(int p_class) { player_class_ = p_class; }
	int get_avatar_a() const { return avatar_a_; }
	void set_avatar_a(int p_value) { avatar_a_ = p_value; }
	int get_avatar_b() const { return avatar_b_; }
	void set_avatar_b(int p_value) { avatar_b_ = p_value; }
	int get_avatar_packed() const { return avatar_packed_; }
	void set_avatar_packed(int p_value) { avatar_packed_ = p_value; }
	PackedStringArray get_kit() const { return kit_; }
	void set_kit(const PackedStringArray &p_kit) { kit_ = p_kit; }

protected:
	static void _bind_methods();

private:
	int player_class_ = 0;
	int avatar_a_ = 0;
	int avatar_b_ = 0;
	int avatar_packed_ = 0;
	PackedStringArray kit_;
};

// The two-side summary of slot 0: `error` / `loaded` from the file read (the
// boot seam) or the latch state of the live profile (the shell's status
// copy), plus the blue and red sides.
class WeaponProfileSummary : public RefCounted {
	GDCLASS(WeaponProfileSummary, RefCounted)

public:
	int get_error() const { return error_; }
	void set_error(int p_error) { error_ = p_error; }
	bool is_loaded() const { return loaded_; }
	void set_loaded(bool p_loaded) { loaded_ = p_loaded; }
	Ref<WeaponProfileSide> get_blue() const { return blue_; }
	void set_blue(const Ref<WeaponProfileSide> &p_side) { blue_ = p_side; }
	Ref<WeaponProfileSide> get_red() const { return red_; }
	void set_red(const Ref<WeaponProfileSide> &p_side) { red_ = p_side; }

protected:
	static void _bind_methods();

private:
	int error_ = 0;
	bool loaded_ = false;
	Ref<WeaponProfileSide> blue_;
	Ref<WeaponProfileSide> red_;
};

} // namespace godot
