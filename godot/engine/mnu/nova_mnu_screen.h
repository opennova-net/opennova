#pragma once

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>

namespace godot {

// One screen (page) of a menu. A plain Control container that carries the
// screen's music track index and cursor for the navigation controller / runtime
// (NovaMnuMenu) to act on when the screen becomes visible. Built by
// nova_mnu_builder; its children are the screen's widgets.
//
// When a cursor texture is resolved (by the builder, through the resource root)
// the screen applies it via Input::set_custom_mouse_cursor each time it becomes
// visible (port of mnu_screen_node.gd). edit_mode suppresses that global-state
// change so the ONED preview never hijacks the editor's cursor.
class NovaMnuScreen : public Control {
	GDCLASS(NovaMnuScreen, Control)

private:
	int music_var_ = 0;
	String cursor_file_;
	String screen_name_;
	Ref<Texture2D> cursor_texture_;
	bool edit_mode_ = false;

	void on_visibility_changed();
	void apply_cursor();

protected:
	static void _bind_methods();

public:
	void _ready() override;

	void set_music_var(int p_value) { music_var_ = p_value; }
	int get_music_var() const { return music_var_; }

	void set_cursor_file(const String &p_file) { cursor_file_ = p_file; }
	String get_cursor_file() const { return cursor_file_; }

	void set_screen_name(const String &p_name) { screen_name_ = p_name; }
	String get_screen_name() const { return screen_name_; }

	// Build-time configuration.
	void set_cursor_texture(const Ref<Texture2D> &p_texture) { cursor_texture_ = p_texture; }
	Ref<Texture2D> get_cursor_texture() const { return cursor_texture_; }
	void set_edit_mode(bool p_edit) { edit_mode_ = p_edit; }
	bool get_edit_mode() const { return edit_mode_; }
};

} // namespace godot
