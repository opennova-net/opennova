#include <runtime/inmatch/game_config_block.h>

namespace opennova::inmatch {

GameConfigBlock::GameConfigBlock(std::string path) : path_(std::move(path)) {}

bool GameConfigBlock::load() {
	gamecfg::LoadOptions options;
	options.texts = texts_;
	options.weapons = roster_;
	options.player_index = cfg_.player_index;
	options.hw3d_deviceno = cfg_.hw3d_deviceno;
	options.lan_switch = lan_switch_;
	gamecfg::LoadResult loaded = gamecfg::load_file(path_, options);
	cfg_ = std::move(loaded.cfg);
	file_read_ = loaded.file_read;
	version_reset_ = loaded.version_reset;
	reset_exit_ = loaded.reset_exit;
	return !reset_exit_;
}

bool GameConfigBlock::save(std::string &error) const {
	return gamecfg::save_file(path_, cfg_, roster_, error);
}

bool GameConfigBlock::reload(std::string &error) {
	// [orig: Game_ReloadExpansionAndMods @0x55272A saves, @0x55281A reads
	//  through Menu_InitShellResources @0x552534; the save's result unchecked]
	(void)save(error);
	return load();
}

} // namespace opennova::inmatch
