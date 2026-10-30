#include "player/game_config_block.h"

#include "mnu/menu_driver.h"
#include "mnu/mnu_document.h"
#include "network/join_target.h"
#include "network/novaworld_identity.h" // sample_device_identity, primary_desktop_size
#include "resource_index/launch_flags.h"
#include "resource_index/resource_root.h"
#include "rtxt/rtxt_string_file.h"
#include "util/string_convert.h"

#include <runtime/inmatch/host_config.h>
#include <runtime/inmatch/host_settings.h>
#include <formats/gamecfg/server_settings.h>
#include <base/io/cp1252.h>
#include <net/protocol/protocol.h> // the armory-only value (DFX2-10)

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
	const std::shared_ptr<const opennova::rtxt::File> text_override =
			RtxtStringFile::installed_text_override();
	mount.texts = inmatch::game_cfg_default_texts(
			p_gametext.is_valid() ? &p_gametext->get_native() : nullptr, text_override.get());
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
	ClassDB::bind_method(D_METHOD("server_configuration_json"), &GameConfigBlock::server_configuration_json);
	ClassDB::bind_method(D_METHOD("apply_server_configuration_json", "json"), &GameConfigBlock::apply_server_configuration_json);
	ClassDB::bind_method(D_METHOD("set_path", "path"), &GameConfigBlock::set_path);
	ClassDB::bind_method(D_METHOD("get_path"), &GameConfigBlock::get_path);
	ClassDB::bind_method(D_METHOD("load"), &GameConfigBlock::load);
	ClassDB::bind_method(D_METHOD("save"), &GameConfigBlock::save);
	ClassDB::bind_method(D_METHOD("enter_main_menu", "root", "gametext", "desktop"),
			&GameConfigBlock::enter_main_menu);
	ClassDB::bind_method(D_METHOD("reload_main_menu", "root", "gametext", "desktop"),
			&GameConfigBlock::reload_main_menu);
	ClassDB::bind_static_method("GameConfigBlock", D_METHOD("primary_desktop_size"),
			&GameConfigBlock::primary_desktop_size);
	ClassDB::bind_method(D_METHOD("switch_expansion", "root"), &GameConfigBlock::switch_expansion);
	ClassDB::bind_method(D_METHOD("is_file_read"), &GameConfigBlock::is_file_read);
	ClassDB::bind_method(D_METHOD("is_version_reset"), &GameConfigBlock::is_version_reset);
	ClassDB::bind_method(D_METHOD("is_reset_exit"), &GameConfigBlock::is_reset_exit);
	ClassDB::bind_method(D_METHOD("is_video_test_ran"), &GameConfigBlock::is_video_test_ran);
	ClassDB::bind_method(D_METHOD("get_word", "key"), &GameConfigBlock::get_word);
	ClassDB::bind_method(D_METHOD("set_word", "key", "value"), &GameConfigBlock::set_word);
	ClassDB::bind_method(D_METHOD("populate_host_dialog"), &GameConfigBlock::populate_host_dialog);
	ClassDB::bind_method(D_METHOD("host_dialog_value", "control"), &GameConfigBlock::host_dialog_value);
	ClassDB::bind_method(D_METHOD("single_player_mission_start"),
			&GameConfigBlock::single_player_mission_start);
	ClassDB::bind_method(D_METHOD("apply_host_dialog_control", "control", "value"),
			&GameConfigBlock::apply_host_dialog_control);
	ClassDB::bind_method(D_METHOD("populate_host_screen", "driver"), &GameConfigBlock::populate_host_screen);
	ClassDB::bind_method(D_METHOD("read_host_screen", "driver"), &GameConfigBlock::read_host_screen);
	ClassDB::bind_method(D_METHOD("read_untouched_host_dialog", "doc"),
			&GameConfigBlock::read_untouched_host_dialog);
}

GameConfigBlock::GameConfigBlock() {
	// The launch's title, whose cfg table every read and save walks: a DFX2 launch's game.cfg
	// keeps its mpdfxcategory row, which a block left on the default table once dropped.
	block_.set_title(ResourceRoot::session_title(Ref<ResourceRoot>()));
}

String GameConfigBlock::server_configuration_json() const {
	using namespace opennova;
	auto out = io::JsonValue::make_object();
	out.set("schema", gamecfg::server_settings_schema(block_.config_table(), false));
	auto values = gamecfg::server_settings_values(block_.cfg(), false);
	auto weapons = io::JsonValue::make_array(), availability = io::JsonValue::make_object();
	const auto &roster = block_.weapon_roster();
	for (size_t i = 1; i < roster.size() && i < gamecfg::kWeaponAvailabilityCount; ++i) {
		if (!roster[i].loadout_selectable) continue;
		const auto name = cp1252_to_utf8(roster[i].name);
		weapons.push(io::json_string(name));
		const auto value = block_.cfg().weapon_availability[i];
		// The armory-only value is the protocol's (DFX2-10: JO's 2, DFX2's 3).
		const int armory = protocol_in_match(title_protocol(block_.title()->code)).armory_only_availability;
		availability.set(name, io::json_string(value == 0 ? "never" : value == armory ? "armory" : "always"));
	}
	values.set("weapons", std::move(availability));
	out.set("values", std::move(values)); out.set("weapons", std::move(weapons));
	return to_gd(io::json_write(out));
}

String GameConfigBlock::apply_server_configuration_json(const String &p_json) {
	using namespace opennova;
	io::JsonValue patch; std::string error, field;
	if (!io::json_parse(to_std(p_json), patch, error)) return to_gd(error);
	if (!gamecfg::apply_server_settings(block_.cfg(), patch, false, block_.weapon_roster(), field, error))
		return to_gd(field + ": " + error);
	return {};
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
	// The boot's renderer start and device creation follow its game.cfg read: the NWHWI device
	// facts are sampled there, once (network/novaworld_identity.h).
	sample_device_identity(block_.cfg());
	return kept;
}

Vector2i GameConfigBlock::primary_desktop_size() {
	return godot::primary_desktop_size();
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

void GameConfigBlock::populate_host_dialog() {
	inmatch::populate_host_dialog(block_.cfg(), *block_.title());
}

String GameConfigBlock::host_dialog_value(const String &p_control) const {
	return opennova::to_gd(inmatch::host_dialog_value(block_.cfg(), opennova::to_std(p_control)));
}

bool GameConfigBlock::apply_host_dialog_control(const String &p_control, const String &p_value) {
	return inmatch::apply_host_dialog_control(block_.cfg(), *block_.title(),
			opennova::to_std(p_control), opennova::to_std(p_value));
}

void GameConfigBlock::populate_host_screen(const Ref<MenuDriver> &p_driver) {
	if (p_driver.is_valid()) inmatch::populate_host_screen(p_driver->runtime(), block_.cfg(), *block_.title());
}

void GameConfigBlock::read_host_screen(const Ref<MenuDriver> &p_driver) {
	if (p_driver.is_valid()) inmatch::read_host_screen(p_driver->runtime(), block_.cfg(), *block_.title());
}

void GameConfigBlock::read_untouched_host_dialog(const Ref<MnuDocument> &p_doc) {
	inmatch::read_untouched_host_dialog(block_.cfg(), *block_.title(),
			p_doc.is_valid() ? &p_doc->get_native() : nullptr);
}

void GameConfigBlock::join_choice(const Ref<GameConfigBlock> &p_block,
		const Ref<JoinTarget> &p_target, int &r_role, String &r_spectator_password) {
	if (p_block.is_null()) {
		r_role = p_target->get_join_role();
		r_spectator_password = p_target->get_spectator_password();
		return;
	}
	inmatch::GameConfigBlock &block = p_block->block_;
	if (p_target->get_role_explicit()) {
		block.commit_join_choice(p_target->get_join_role() == JoinTarget::ROLE_SPECTATOR,
				opennova::to_std(p_target->get_spectator_password()));
	}
	r_role = block.cfg().join_spectate != 0 ? JoinTarget::ROLE_SPECTATOR : JoinTarget::ROLE_PLAYER;
	r_spectator_password = opennova::to_gd(block.cfg().join_spectator_password);
}
