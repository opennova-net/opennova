// Generator + guard for fixtures/gamecfg/synth_game.cfg: a synthetic game.cfg
// written by gamecfg::write from the model in tests/gamecfg/gamecfg_synth.h
// (every written key off its default, a four-row weapon roster of which three
// rows are loadout_selectable). No retail file is carried: a retail game.cfg is
// written by the game on the player's machine and carries its hardware names.
// The layout is Game_SaveConfig's [orig: Game_SaveConfig @0x54C490].
//
// Default: rebuild in memory and byte-compare the committed file. `--write`
// (re)writes it.
#include "common/test_paths.h"

#include "gamecfg/gamecfg_synth.h"

#include <formats/gamecfg/game_cfg.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "common/file_io.h"

namespace {

bool expect(bool cond, const char *msg) {
	if (!cond) std::fprintf(stderr, "FAIL: %s\n", msg);
	return cond;
}

} // namespace

int main(int argc, char **argv) {
	using namespace opennova::gamecfg;
	bool write_mode = false;
	for (int i = 1; i < argc; ++i)
		if (std::strcmp(argv[i], "--write") == 0) write_mode = true;
	const std::string path = std::string(test_paths_repo_root(__FILE__)) + "/fixtures/gamecfg/synth_game.cfg";

	const WeaponRoster weapons = gamecfg_synth::roster();
	const std::string text = write(gamecfg_synth::config(), weapons);

	// The writer's output is its own fixed point: a read of it, then a write.
	LoadOptions options;
	options.weapons = weapons;
	const LoadResult back = load(text.data(), text.size(), options);
	if (!expect(back.file_read && !back.version_reset && !back.reset_exit, "the minted text did not load cleanly"))
		return 1;
	if (!expect(write(back.cfg, weapons) == text, "write(load(write(cfg))) is not byte-stable")) return 1;
	if (!expect(back.cfg.game_name == "Synthetic Server" && back.cfg.dedicated == 1 && back.cfg.mp_max_players == 24 &&
	                    back.cfg.weapon_availability[1] == 2 && back.cfg.remote_admin_port == 4711,
	            "the re-read model lost a pinned field"))
		return 1;

	if (write_mode) {
		std::ofstream o(path, std::ios::binary);
		o.write(text.data(), static_cast<std::streamsize>(text.size()));
		std::printf("wrote %s (%zu bytes)\n", path.c_str(), text.size());
		return 0;
	}
	std::vector<uint8_t> committed;
	if (!expect(test_io::read_file(path, committed), ("committed file missing; run with --write: " + path).c_str()))
		return 1;
	if (!expect(std::string(committed.begin(), committed.end()) == text,
	            ("differs from the generator output; regenerate with --write: " + path).c_str()))
		return 1;
	std::printf("OK: fixtures/gamecfg/synth_game.cfg byte-reproducible\n");
	return 0;
}
