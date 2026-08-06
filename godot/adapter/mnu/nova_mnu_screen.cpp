#include "nova_mnu_screen.h"

#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/vector2.hpp>

using namespace godot;

void NovaMnuScreen::_ready() {
	connect("visibility_changed", callable_mp(this, &NovaMnuScreen::on_visibility_changed));
	if (is_visible() && cursor_texture_.is_valid() && !edit_mode_) {
		apply_cursor();
	}
}

void NovaMnuScreen::on_visibility_changed() {
	if (is_visible() && cursor_texture_.is_valid() && !edit_mode_) {
		apply_cursor();
	}
}

void NovaMnuScreen::apply_cursor() {
	Input *input = Input::get_singleton();
	if (input != nullptr && cursor_texture_.is_valid()) {
		input->set_custom_mouse_cursor(cursor_texture_, Input::CURSOR_ARROW, Vector2(0, 0));
	}
}

void NovaMnuScreen::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_music_var", "value"), &NovaMnuScreen::set_music_var);
	ClassDB::bind_method(D_METHOD("get_music_var"), &NovaMnuScreen::get_music_var);
	ClassDB::bind_method(D_METHOD("set_cursor_file", "file"), &NovaMnuScreen::set_cursor_file);
	ClassDB::bind_method(D_METHOD("get_cursor_file"), &NovaMnuScreen::get_cursor_file);
	ClassDB::bind_method(D_METHOD("set_screen_name", "name"), &NovaMnuScreen::set_screen_name);
	ClassDB::bind_method(D_METHOD("get_screen_name"), &NovaMnuScreen::get_screen_name);
	ClassDB::bind_method(D_METHOD("set_edit_mode", "edit"), &NovaMnuScreen::set_edit_mode);
	ClassDB::bind_method(D_METHOD("get_edit_mode"), &NovaMnuScreen::get_edit_mode);
	ClassDB::bind_method(D_METHOD("set_cursor_texture", "texture"), &NovaMnuScreen::set_cursor_texture);
	ClassDB::bind_method(D_METHOD("get_cursor_texture"), &NovaMnuScreen::get_cursor_texture);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "music_var"), "set_music_var", "get_music_var");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "cursor_file"), "set_cursor_file", "get_cursor_file");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "screen_name"), "set_screen_name", "get_screen_name");
}
