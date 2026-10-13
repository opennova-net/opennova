#include <runtime/inmatch/game_config_block.h>

#include <runtime/renderer/aspect_ratio.h>
#include <runtime/renderer/object_lod.h>
#include <runtime/renderer/particle_density.h>
#include <runtime/renderer/texture_compression.h>
#include <runtime/renderer/texture_filter.h>

namespace opennova::inmatch {

void run_video_test(gamecfg::GameCfg &cfg, int32_t desktop_width, int32_t desktop_height) {
	cfg.display_16x9 = renderer::fresh_profile_aspect_mode(desktop_width, desktop_height);
	cfg.object_polydetail = renderer::kObjectLodDetailFreshProfile;
	cfg.texfilter_level = renderer::kTexFilterLevelFreshProfile;
	cfg.particle_density = renderer::kParticleDensityFreshProfile;
	cfg.texcompression_level = renderer::kTexCompressionLevelFreshProfile;
}

GameConfigBlock::GameConfigBlock(std::string path) : path_(std::move(path)) {}

bool GameConfigBlock::load() {
	gamecfg::LoadOptions options;
	options.texts = texts_;
	options.weapons = roster_;
	options.player_index = cfg_.player_index;
	options.hw3d_deviceno = cfg_.hw3d_deviceno;
	options.lan_switch = lan_switch_; // [orig: g_LanSwitch @0x5514B1]
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

bool GameConfigBlock::enter_main_menu(const MenuMount &mount, std::string &error) {
	if (!menu_entered_) {
		// The subsystems' read over the mount's tables, then the video test
		// on a fresh profile, which saves at once [orig: Game_InitSubsystems
		// @0x4A70AB; the test's trigger @0x4A711C, Game_RunVideoTestDialog
		// @0x4A7125, its save @0x53EDB6].
		menu_entered_ = true;
		menu_mount_ = mount.id;
		roster_ = mount.weapons;
		texts_ = mount.texts;
		if (!load()) return false;
		video_test_ran_ = version_reset_;
		if (video_test_ran_) {
			run_video_test(cfg_, mount.desktop_width, mount.desktop_height);
			(void)save(error);
		}
	} else if (mount.id != menu_mount_) {
		return reload_main_menu(mount, error);
	}
	// The MainMenu scene's init reads the file [orig: @0x552534].
	return load();
}

bool GameConfigBlock::reload_main_menu(const MenuMount &mount, std::string &error) {
	// [orig: Game_ReloadExpansionAndMods @0x55272A saves, @0x55281A reads
	//  through Menu_InitShellResources @0x552534; the save's result unchecked;
	//  the weapon table rebuilt after the read @0x55281F..0x55282B]
	menu_entered_ = true;
	menu_mount_ = mount.id;
	(void)save(error);
	const bool kept = load();
	roster_ = mount.weapons;
	texts_ = mount.texts;
	return kept;
}

void GameConfigBlock::switch_expansion(std::string id, gamecfg::WeaponRoster weapons) {
	// [orig: Expansion_SwitchTo @0x5689AF rebuilds the weapon table; no read]
	menu_mount_ = std::move(id);
	roster_ = std::move(weapons);
}

} // namespace opennova::inmatch
