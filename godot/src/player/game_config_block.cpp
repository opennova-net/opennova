#include "player/game_config_block.h"

#include "resource_index/launch_flags.h"
#include "resource_index/resource_root.h"
#include "rtxt/rtxt_string_file.h"
#include "util/string_convert.h"

#include <runtime/inmatch/host_config.h>
#include <runtime/inmatch/host_settings.h>

#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

namespace {

namespace inmatch = opennova::inmatch;

// The mount's name: the root and its expansion name the game.
std::string mount_id(const Ref<ResourceRoot> &p_root) {
	return opennova::to_std(p_root->get_root_dir() + "|" + p_root->get_expansion().to_lower());
}

// The mount as the main menu sees it: its name, its index's weapon table and
// the gametext table's default texts.
inmatch::GameConfigBlock::MenuMount menu_mount(const Ref<ResourceRoot> &p_root,
		const Ref<RtxtStringFile> &p_gametext, const Vector2i &p_desktop) {
	inmatch::GameConfigBlock::MenuMount mount;
	if (p_root.is_valid()) {
		mount.id = mount_id(p_root);
		mount.weapons = inmatch::game_cfg_weapon_roster(p_root->native_index());
	}
	mount.texts = inmatch::game_cfg_default_texts(
			p_gametext.is_valid() ? &p_gametext->get_native() : nullptr);
	mount.desktop_width = p_desktop.x;
	mount.desktop_height = p_desktop.y;
	return mount;
}

void warn_save_error(const std::string &p_error) {
	if (!p_error.empty())
		UtilityFunctions::push_warning(vformat("GameConfigBlock: %s", opennova::to_gd(p_error)));
}

// The process's command-line switches every read takes: `/LAN` (LaunchFlags).
void take_launch_switches(inmatch::GameConfigBlock &p_block) {
	p_block.set_lan_switch(LaunchFlags::lan());
}

} // namespace

void GameConfigBlock::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_path", "path"), &GameConfigBlock::set_path);
	ClassDB::bind_method(D_METHOD("get_path"), &GameConfigBlock::get_path);
	ClassDB::bind_method(D_METHOD("load"), &GameConfigBlock::load);
	ClassDB::bind_method(D_METHOD("save"), &GameConfigBlock::save);
	ClassDB::bind_method(D_METHOD("enter_main_menu", "root", "gametext", "desktop"),
			&GameConfigBlock::enter_main_menu);
	ClassDB::bind_method(D_METHOD("reload_main_menu", "root", "gametext", "desktop"),
			&GameConfigBlock::reload_main_menu);
	ClassDB::bind_method(D_METHOD("switch_expansion", "root"), &GameConfigBlock::switch_expansion);
	ClassDB::bind_method(D_METHOD("is_file_read"), &GameConfigBlock::is_file_read);
	ClassDB::bind_method(D_METHOD("is_version_reset"), &GameConfigBlock::is_version_reset);
	ClassDB::bind_method(D_METHOD("is_reset_exit"), &GameConfigBlock::is_reset_exit);
	ClassDB::bind_method(D_METHOD("is_video_test_ran"), &GameConfigBlock::is_video_test_ran);
	ClassDB::bind_method(D_METHOD("get_word", "key"), &GameConfigBlock::get_word);
	ClassDB::bind_method(D_METHOD("set_word", "key", "value"), &GameConfigBlock::set_word);
	ClassDB::bind_method(D_METHOD("host_dialog_values"), &GameConfigBlock::host_dialog_values);
	ClassDB::bind_method(D_METHOD("single_player_mission_start"),
			&GameConfigBlock::single_player_mission_start);
	ClassDB::bind_method(D_METHOD("apply_host_dialog_control", "control", "value"),
			&GameConfigBlock::apply_host_dialog_control);
}

void GameConfigBlock::set_path(const String &p_path) {
	block_.set_path(opennova::to_std(p_path));
}

String GameConfigBlock::get_path() const {
	return opennova::to_gd(block_.path());
}

bool GameConfigBlock::load() {
	take_launch_switches(block_);
	return block_.load();
}

Error GameConfigBlock::save() {
	std::string error;
	if (block_.save(error)) return OK;
	warn_save_error(error);
	return ERR_FILE_CANT_WRITE;
}

bool GameConfigBlock::enter_main_menu(const Ref<ResourceRoot> &p_root,
		const Ref<RtxtStringFile> &p_gametext, const Vector2i &p_desktop) {
	std::string error;
	take_launch_switches(block_);
	const bool kept = block_.enter_main_menu(menu_mount(p_root, p_gametext, p_desktop), error);
	warn_save_error(error);
	return kept;
}

bool GameConfigBlock::reload_main_menu(const Ref<ResourceRoot> &p_root,
		const Ref<RtxtStringFile> &p_gametext, const Vector2i &p_desktop) {
	std::string error;
	take_launch_switches(block_);
	const bool kept = block_.reload_main_menu(menu_mount(p_root, p_gametext, p_desktop), error);
	warn_save_error(error);
	return kept;
}

void GameConfigBlock::switch_expansion(const Ref<ResourceRoot> &p_root) {
	if (p_root.is_null()) return;
	block_.switch_expansion(
			mount_id(p_root), inmatch::game_cfg_weapon_roster(p_root->native_index()));
}

int GameConfigBlock::get_word(const String &p_key) const {
	if (const int32_t *word = opennova::gamecfg::int_word(block_.cfg(), p_key.utf8().get_data()))
		return *word;
	UtilityFunctions::push_warning(vformat("GameConfigBlock: no word '%s'", p_key));
	return 0;
}

void GameConfigBlock::set_word(const String &p_key, int p_value) {
	if (int32_t *word = opennova::gamecfg::int_word(block_.cfg(), p_key.utf8().get_data())) {
		*word = p_value;
		return;
	}
	UtilityFunctions::push_warning(vformat("GameConfigBlock: no word '%s'", p_key));
}

Dictionary GameConfigBlock::host_dialog_values() {
	inmatch::populate_host_dialog(block_.cfg());
	Dictionary values;
	for (const std::string_view control : inmatch::host_dialog_controls())
		values[String::utf8(control.data(), static_cast<int64_t>(control.size()))] =
				opennova::to_gd(inmatch::host_dialog_value(block_.cfg(), control));
	return values;
}

bool GameConfigBlock::apply_host_dialog_control(const String &p_control, const String &p_value) {
	return inmatch::apply_host_dialog_control(
			block_.cfg(), opennova::to_std(p_control), opennova::to_std(p_value));
}
