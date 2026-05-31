#pragma once

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>

namespace godot {

// One screen (page) of a menu. A plain Control container that carries the
// screen's music track index and cursor for the navigation controller / runtime
// (NovaMnuMenu) to act on when the screen becomes visible. Built by
// nova_mnu_builder; its children are the screen's widgets.
class NovaMnuScreen : public Control {
	GDCLASS(NovaMnuScreen, Control)

private:
	int music_var_ = 0;
	String cursor_file_;
	String screen_name_;

protected:
	static void _bind_methods();

public:
	void set_music_var(int p_value) { music_var_ = p_value; }
	int get_music_var() const { return music_var_; }

	void set_cursor_file(const String &p_file) { cursor_file_ = p_file; }
	String get_cursor_file() const { return cursor_file_; }

	void set_screen_name(const String &p_name) { screen_name_ = p_name; }
	String get_screen_name() const { return screen_name_; }
};

} // namespace godot
