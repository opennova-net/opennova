#include "player/game_config_block.h"

#include "resource_index/resource_root.h"
#include "rtxt/rtxt_string_file.h"
#include "util/string_convert.h"

#include <runtime/inmatch/host_config.h>

#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

namespace {

namespace gamecfg = opennova::gamecfg;

// The words the shell reads and writes, by the keys game.cfg spells them with
// (docs/gamecfg/game-cfg-re.md, the key table): the options' volumes,
// crosshair, aspect, tips and graphics words, the HUD's colour and declutter
// words, the score fanfare's gate and the mpreset word.
struct Word {
	const char *key;
	int32_t gamecfg::GameCfg::*field;
};
constexpr Word kWords[] = {
	{ "SFXLevel", &gamecfg::GameCfg::sfx_level },
	{ "DialogueLevel", &gamecfg::GameCfg::dialogue_level },
	{ "music_volume", &gamecfg::GameCfg::music_volume },
	{ "xhair_appearance", &gamecfg::GameCfg::xhair_appearance },
	{ "xhair_color", &gamecfg::GameCfg::xhair_color },
	{ "xhair_spread", &gamecfg::GameCfg::xhair_spread },
	{ "display_16x9", &gamecfg::GameCfg::display_16x9 },
	{ "enable_keyboardtips", &gamecfg::GameCfg::enable_keyboardtips },
	{ "enable_gameplaytips", &gamecfg::GameCfg::enable_gameplaytips },
	{ "object_polydetail", &gamecfg::GameCfg::object_polydetail },
	{ "texfilter_level", &gamecfg::GameCfg::texfilter_level },
	{ "particle_density", &gamecfg::GameCfg::particle_density },
	{ "texcompression_level", &gamecfg::GameCfg::texcompression_level },
	{ "hud_color_index", &gamecfg::GameCfg::hud_color_index },
	{ "hud_detail", &gamecfg::GameCfg::hud_detail },
	{ "enable_slotmachine", &gamecfg::GameCfg::enable_slotmachine },
	{ "mpreset", &gamecfg::GameCfg::mp_reset },
};

const Word *find_word(const String &p_key) {
	for (const Word &word : kWords) {
		if (p_key == word.key)
			return &word;
	}
	UtilityFunctions::push_warning(vformat("GameConfigBlock: no word '%s'", p_key));
	return nullptr;
}

} // namespace

void GameConfigBlock::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_path", "path"), &GameConfigBlock::set_path);
	ClassDB::bind_method(D_METHOD("get_path"), &GameConfigBlock::get_path);
	ClassDB::bind_method(
			D_METHOD("set_mount_tables", "root", "gametext"), &GameConfigBlock::set_mount_tables);
	ClassDB::bind_method(D_METHOD("set_lan_switch", "on"), &GameConfigBlock::set_lan_switch);
	ClassDB::bind_method(D_METHOD("load"), &GameConfigBlock::load);
	ClassDB::bind_method(D_METHOD("save"), &GameConfigBlock::save);
	ClassDB::bind_method(D_METHOD("reload"), &GameConfigBlock::reload);
	ClassDB::bind_method(D_METHOD("is_file_read"), &GameConfigBlock::is_file_read);
	ClassDB::bind_method(D_METHOD("is_version_reset"), &GameConfigBlock::is_version_reset);
	ClassDB::bind_method(D_METHOD("is_reset_exit"), &GameConfigBlock::is_reset_exit);
	ClassDB::bind_method(D_METHOD("get_word", "key"), &GameConfigBlock::get_word);
	ClassDB::bind_method(D_METHOD("set_word", "key", "value"), &GameConfigBlock::set_word);
	ClassDB::bind_static_method(
			"GameConfigBlock", D_METHOD("word_keys"), &GameConfigBlock::word_keys);
}

void GameConfigBlock::set_path(const String &p_path) {
	block_.set_path(opennova::to_std(p_path));
}

String GameConfigBlock::get_path() const {
	return opennova::to_gd(block_.path());
}

void GameConfigBlock::set_mount_tables(
		const Ref<ResourceRoot> &p_root, const Ref<RtxtStringFile> &p_gametext) {
	block_.set_weapon_roster(p_root.is_valid()
					? opennova::inmatch::game_cfg_weapon_roster(p_root->native_index())
					: gamecfg::WeaponRoster{});
	block_.set_default_texts(opennova::inmatch::game_cfg_default_texts(
			p_gametext.is_valid() ? &p_gametext->get_native() : nullptr));
}

Error GameConfigBlock::save() {
	std::string error;
	if (block_.save(error))
		return OK;
	UtilityFunctions::push_warning(vformat("GameConfigBlock: %s", opennova::to_gd(error)));
	return ERR_FILE_CANT_WRITE;
}

bool GameConfigBlock::reload() {
	std::string error;
	const bool kept = block_.reload(error);
	if (!error.empty())
		UtilityFunctions::push_warning(vformat("GameConfigBlock: %s", opennova::to_gd(error)));
	return kept;
}

int GameConfigBlock::get_word(const String &p_key) const {
	const Word *word = find_word(p_key);
	return word != nullptr ? block_.cfg().*(word->field) : 0;
}

void GameConfigBlock::set_word(const String &p_key, int p_value) {
	if (const Word *word = find_word(p_key))
		block_.cfg().*(word->field) = p_value;
}

PackedStringArray GameConfigBlock::word_keys() {
	PackedStringArray keys;
	for (const Word &word : kWords)
		keys.push_back(word.key);
	return keys;
}
