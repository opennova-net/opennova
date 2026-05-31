#include "nova_mnu_screen.h"

using namespace godot;

void NovaMnuScreen::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_music_var", "value"), &NovaMnuScreen::set_music_var);
	ClassDB::bind_method(D_METHOD("get_music_var"), &NovaMnuScreen::get_music_var);
	ClassDB::bind_method(D_METHOD("set_cursor_file", "file"), &NovaMnuScreen::set_cursor_file);
	ClassDB::bind_method(D_METHOD("get_cursor_file"), &NovaMnuScreen::get_cursor_file);
	ClassDB::bind_method(D_METHOD("set_screen_name", "name"), &NovaMnuScreen::set_screen_name);
	ClassDB::bind_method(D_METHOD("get_screen_name"), &NovaMnuScreen::get_screen_name);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "music_var"), "set_music_var", "get_music_var");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "cursor_file"), "set_cursor_file", "get_cursor_file");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "screen_name"), "set_screen_name", "get_screen_name");
}
