#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>

namespace godot {

// What a mission load shows on the loading screen (the former
// loading_screen_info.gd, ADR 0043 slice G10): the .bms name driving the
// sidecar background lookup, whether the load carries an MP session (the
// text-overlay gate) and the session variables retail resolves before its
// load (the witnesses sit on LoadingScreen.setup / update_session_info). A
// JOINER's post-auth 0x7B record refreshes the same fields mid-load; empty
// strings and a negative game type leave the current values alone. The
// world's `join_session_identified` signal carries one.
class LoadingScreenInfo : public RefCounted {
	GDCLASS(LoadingScreenInfo, RefCounted)

public:
	static Ref<LoadingScreenInfo> make(const String &p_mission_file, bool p_in_session,
			const String &p_server_name, const String &p_mission_name, int p_game_type,
			const String &p_custom_text);
	// A single-player load: only the mission file matters (no session overlay).
	static Ref<LoadingScreenInfo> for_mission(const String &p_mission_file);

	String get_mission_file() const { return mission_file_; }
	void set_mission_file(const String &p_value) { mission_file_ = p_value; }
	bool get_in_session() const { return in_session_; }
	void set_in_session(bool p_value) { in_session_ = p_value; }
	String get_server_name() const { return server_name_; }
	void set_server_name(const String &p_value) { server_name_ = p_value; }
	String get_mission_name() const { return mission_name_; }
	void set_mission_name(const String &p_value) { mission_name_ = p_value; }
	String get_custom_text() const { return custom_text_; }
	void set_custom_text(const String &p_value) { custom_text_ = p_value; }
	// The numeric session game type; -1 = not carried.
	int get_game_type() const { return game_type_; }
	void set_game_type(int p_value) { game_type_ = p_value; }

protected:
	static void _bind_methods();

private:
	String mission_file_;
	bool in_session_ = false;
	String server_name_;
	String mission_name_;
	String custom_text_;
	int game_type_ = -1;
};

} // namespace godot
