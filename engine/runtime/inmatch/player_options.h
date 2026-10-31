#pragma once

// THE PLAYER'S OPTIONS: the game.cfg words the front-end and in-game Options screens show and
// edit, as one typed record over the process's cfg block (game_config_block.h). Each is retail's
// word in g_GameConfigState, written to game.cfg at the block's save points
// (docs/gamecfg/game-cfg-re.md): the screens edit the block and their changes reach the file at
// the next save point, as retail's do. The controls words and the key bindings are the player
// profile's instead (runtime/profile/profile_controls.h).
//
// The read normalizes at the boundary: each volume to its slider's witnessed range
// (menu/options_policy.h kOptionsScrollRanges, by the slider's control name), the crosshair
// style to the art the options offer and its colour to the 24-bit mask, the object detail, the
// texture filter and the particle density to the clamps their load applies; the aspect, tip,
// compression and texture-detail words pass as the block holds them (the block's own read
// clamps the texture details, gamecfg clamp_graphics_options).
//
// THE MISSION START'S APPLY. The words reach the running game at two points: the volumes at
// once (the device's mixer), and the aspect mode as the session's copy each mission start takes
// (renderer/aspect_ratio.h aspect_viewport_scale_y carries the witness: the options write only
// the cfg word, each mission start applies the session's copy, and nothing else moves the
// frame's aspect globals); every graphics word reaches its consumer at its own next load (each
// policy header above carries its witness).

#include <formats/gamecfg/game_cfg.h>
#include <runtime/menu/options_policy.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace opennova::inmatch {

struct PlayerOptions {
	int32_t sound_fx_volume = 0;     // `SFXLevel`
	int32_t dialog_volume = 0;       // `DialogueLevel`
	int32_t music_volume = 0;        // `music_volume`
	int32_t crosshair_style = 0;     // `xhair_appearance` (cross%02d.tga)
	int32_t crosshair_color = 0;     // `xhair_color`
	bool crosshair_spread = false;   // `xhair_spread`
	int32_t aspect_mode = 0;         // `display_16x9`
	bool keyboard_tips = false;      // `enable_keyboardtips`
	bool gameplay_tips = false;      // `enable_gameplaytips`
	int32_t object_polydetail = 0;   // `object_polydetail`
	int32_t texfilter_level = 0;     // `texfilter_level`
	int32_t particle_density = 0;    // `particle_density`
	int32_t texcompression_level = 0; // `texcompression_level`
	int32_t terrain_texdetail = 0;   // `terrain_texdetail`
	int32_t object_texdetail = 0;    // `object_texdetail`

	bool operator==(const PlayerOptions &other) const;
	bool operator!=(const PlayerOptions &other) const { return !(*this == other); }
};

// A record before any read: the channel volumes' configuration default, the crosshair's
// defaults (HudLayout, Config_SetDefaults), both tips on, the aspect word's pre-seed 0 and each
// graphics word's fresh-profile value (each policy header carries its witness).
PlayerOptions player_options_defaults();

// The boundary's normalization (above).
PlayerOptions normalized_player_options(const PlayerOptions &options);

// The record off the block's words, normalized.
PlayerOptions read_player_options(const gamecfg::GameCfg &cfg);
// The record, normalized, into the block's words; returns what was written.
PlayerOptions write_player_options(gamecfg::GameCfg &cfg, const PlayerOptions &options);

// The words the Options screens show, by menu::OptionsWord's indices, and the record back from
// their edit (`count` short of OptionsWord::kCount leaves the record as it stands).
using PlayerOptionsWords = std::array<int32_t, static_cast<size_t>(menu::OptionsWord::kCount)>;
PlayerOptionsWords player_options_words(const PlayerOptions &options);
PlayerOptions player_options_from_words(const PlayerOptions &options, const int32_t *words,
		size_t count);

} // namespace opennova::inmatch
