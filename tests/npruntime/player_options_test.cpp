// The player's options over the process's cfg block (runtime/inmatch/player_options.h): the
// record's defaults, the boundary's normalization (the volumes to their sliders' ranges, the
// crosshair style to the offered art, the colour to 24 bits, the object detail, the texture
// filter and the particle density to their load clamps; the aspect, compression and texture
// detail words verbatim), the words it reads and writes, and the Options screens' word array.
#include <formats/gamecfg/game_cfg.h>
#include <runtime/inmatch/player_options.h>

#include "common/test_expect.h"

#include <cstdio>

using namespace opennova;
using namespace opennova::inmatch;
using test_expect::expect;

namespace {

void defaults() {
	const PlayerOptions d = player_options_defaults();
	expect(d.sound_fx_volume == 192 && d.dialog_volume == 192 && d.music_volume == 192,
			"the channel volumes' configuration default");
	expect(d.crosshair_style == 0, "the first crosshair art");
	expect(d.crosshair_color == 0xFFFFFF, "the default crosshair colour is white");
	expect(d.crosshair_spread, "the default spread is on");
	expect(d.aspect_mode == 0, "the defaults' memset placeholder, until the video test");
	expect(d.keyboard_tips && d.gameplay_tips, "both tips default on");
	expect(d.object_polydetail == 2 && d.texfilter_level == 1 && d.particle_density == 2 &&
					d.texcompression_level == 1 && d.terrain_texdetail == 3 &&
					d.object_texdetail == 2,
			"each graphics word's fresh-profile value");
}

void write_normalizes() {
	gamecfg::GameCfg cfg;
	PlayerOptions edit = player_options_defaults();
	edit.sound_fx_volume = -2;
	edit.dialog_volume = 93;
	edit.music_volume = 999;
	edit.crosshair_style = 99;
	edit.crosshair_color = 0x1FF8040;
	edit.crosshair_spread = false;
	edit.keyboard_tips = false;
	edit.aspect_mode = -1;
	edit.object_polydetail = 12;
	edit.texfilter_level = -4;
	edit.particle_density = 9;
	edit.texcompression_level = 7;
	const PlayerOptions written = write_player_options(cfg, edit);
	expect(written.sound_fx_volume == 0 && cfg.sfx_level == 0, "SFXLevel clamps to 0");
	expect(written.dialog_volume == 93 && cfg.dialogue_level == 93, "DialogueLevel in range");
	expect(written.music_volume == 255 && cfg.music_volume == 255, "music_volume clamps to 255");
	expect(written.crosshair_style == 24 && cfg.xhair_appearance == 24,
			"the style clamps to the offered art");
	expect(written.crosshair_color == 0xFF8040 && cfg.xhair_color == 0xFF8040,
			"the colour masks to its 24-bit RGB");
	expect(!written.crosshair_spread && cfg.xhair_spread == 0, "xhair_spread off");
	expect(cfg.enable_keyboardtips == 0 && cfg.enable_gameplaytips == 1,
			"the tips are retail's integers");
	expect(cfg.display_16x9 == -1, "an aspect word outside 0..3 is the surface's own ratio, kept");
	expect(cfg.object_polydetail == 3 && cfg.texfilter_level == 0 && cfg.particle_density == 2,
			"the object detail, filter and density clamp as their loads do");
	expect(cfg.texcompression_level == 7, "the compression word is never clamped");
	expect(read_player_options(cfg) == written, "the block reads back what it wrote");
}

void read_normalizes_corrupt_words() {
	gamecfg::GameCfg cfg;
	cfg.sfx_level = 260;
	cfg.dialogue_level = -1;
	cfg.music_volume = 71;
	cfg.xhair_appearance = -7;
	cfg.xhair_color = 0x7F123456;
	cfg.terrain_texdetail = 1;
	const PlayerOptions o = read_player_options(cfg);
	expect(o.sound_fx_volume == 255 && o.dialog_volume == 0 && o.music_volume == 71,
			"a saved volume past its slider reads clamped");
	expect(o.crosshair_style == 0, "a saved style under the art reads as the first");
	expect(o.crosshair_color == 0x123456, "an out-of-range colour masks down to its RGB");
	expect(o.terrain_texdetail == 1, "the texture detail reads as the block holds it");
}

void words_round_trip() {
	PlayerOptions o = player_options_defaults();
	o.music_volume = 50;
	o.crosshair_spread = false;
	o.aspect_mode = 2;
	o.object_texdetail = 1;
	const PlayerOptionsWords words = player_options_words(o);
	expect(words[static_cast<size_t>(menu::OptionsWord::MusicVolume)] == 50, "the music word");
	expect(words[static_cast<size_t>(menu::OptionsWord::CrosshairSpread)] == 0,
			"a check box's word is 0 or 1");
	expect(player_options_from_words(player_options_defaults(), words.data(), words.size()) == o,
			"the screens' edit comes back as the record");
	expect(player_options_from_words(o, words.data(), words.size() - 1) == o,
			"a short array leaves the record as it stands");
}

} // namespace

int main() {
	defaults();
	write_normalizes();
	read_normalizes_corrupt_words();
	words_round_trip();
	if (test_expect::failures != 0) return 1;
	std::printf("OK: player options\n");
	return 0;
}
