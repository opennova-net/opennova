#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>

namespace godot {

// The DEATH screen's spawn-zone feed (Simulation.get_deploy_spawn_zones):
// one row per team-owned spawn zone with its SECURED verdict, the 0x6E wave
// countdown and the queued occupants, and the compiled SPAWNPOINTS_LIST
// rows (get_deploy_list_rows) the engine builder emits over them. The
// witnesses live on the engine feed (world/deploy_screen_feed.h); read-write
// so a stub sim authors them.

// One queued wave member of a zone: the roster name and the local-player
// marker the builder renders as "<b><cFF4040>** name **".
class DeployOccupantRow : public RefCounted {
	GDCLASS(DeployOccupantRow, RefCounted)

public:
	int get_handle() const { return handle_; }
	void set_handle(int p_handle) { handle_ = p_handle; }
	String get_name() const { return name_; }
	void set_name(const String &p_name) { name_ = p_name; }
	bool is_self() const { return self_; }
	void set_self(bool p_self) { self_ = p_self; }

protected:
	static void _bind_methods();

private:
	int handle_ = 0xFFFF;
	String name_;
	bool self_ = false;
};

// One team-owned spawn zone: `param` is the pick parameter (registry index
// + 1), `letter` 'A' + index, `name_key` the WPNames/STRWPNAME%03d key.
class DeployZoneRow : public RefCounted {
	GDCLASS(DeployZoneRow, RefCounted)

public:
	int get_param() const { return param_; }
	void set_param(int p_param) { param_ = p_param; }
	String get_letter() const { return letter_; }
	void set_letter(const String &p_letter) { letter_ = p_letter; }
	String get_name_key() const { return name_key_; }
	void set_name_key(const String &p_key) { name_key_ = p_key; }
	bool is_secured() const { return secured_; }
	void set_secured(bool p_secured) { secured_ = p_secured; }
	int get_wave_countdown() const { return wave_countdown_; }
	void set_wave_countdown(int p_ticks) { wave_countdown_ = p_ticks; }
	TypedArray<DeployOccupantRow> get_occupants() const { return occupants_; }
	void set_occupants(const TypedArray<DeployOccupantRow> &p_rows) { occupants_ = p_rows; }

protected:
	static void _bind_methods();

private:
	int param_ = 0;
	String letter_;
	String name_key_;
	bool secured_ = false;
	int wave_countdown_ = 0;
	TypedArray<DeployOccupantRow> occupants_;
};

// One compiled SPAWNPOINTS_LIST row: the tagged text and the pick value
// (0 default, index + 1 zone, -1 occupant/blank — never a pick).
class DeployListRow : public RefCounted {
	GDCLASS(DeployListRow, RefCounted)

public:
	String get_text() const { return text_; }
	void set_text(const String &p_text) { text_ = p_text; }
	int get_value() const { return value_; }
	void set_value(int p_value) { value_ = p_value; }

protected:
	static void _bind_methods();

private:
	String text_;
	int value_ = 0;
};

} // namespace godot
