#include "player_options.h"

#include <runtime/audio/volume_law.h>
#include <runtime/hud/hud_frame.h>
#include <runtime/hud/hud_texture_names.h>
#include <runtime/renderer/device_texture.h>
#include <runtime/renderer/object_lod.h>
#include <runtime/renderer/particle_density.h>
#include <runtime/renderer/texture_compression.h>
#include <runtime/renderer/texture_filter.h>

#include <algorithm>
#include <cstring>

namespace opennova::inmatch {

namespace {

using menu::OptionsWord;

// A slider's witnessed range by its control name; a control the table does not range passes
// [orig: UI_OptionsScreenInit @0x554800; UI_PopulateRenderAndAudioSettings @0x55c830].
int32_t clamp_to_control(int32_t value, const char *control) {
	for (const menu::OptionsScrollRange &range : menu::kOptionsScrollRanges)
		if (std::strcmp(range.control, control) == 0)
			return std::clamp(value, range.minimum, range.maximum);
	return value;
}

constexpr size_t index(OptionsWord word) { return static_cast<size_t>(word); }

} // namespace

bool PlayerOptions::operator==(const PlayerOptions &o) const {
	return sound_fx_volume == o.sound_fx_volume && dialog_volume == o.dialog_volume &&
			music_volume == o.music_volume && crosshair_style == o.crosshair_style &&
			crosshair_color == o.crosshair_color && crosshair_spread == o.crosshair_spread &&
			aspect_mode == o.aspect_mode && keyboard_tips == o.keyboard_tips &&
			gameplay_tips == o.gameplay_tips && object_polydetail == o.object_polydetail &&
			texfilter_level == o.texfilter_level && particle_density == o.particle_density &&
			texcompression_level == o.texcompression_level &&
			terrain_texdetail == o.terrain_texdetail && object_texdetail == o.object_texdetail;
}

PlayerOptions player_options_defaults() {
	PlayerOptions options;
	options.sound_fx_volume = audio::kDefaultChannelVolume;
	options.dialog_volume = audio::kDefaultChannelVolume;
	options.music_volume = audio::kDefaultChannelVolume;
	options.crosshair_style = hud::kHudCrosshairStyleMin;
	options.crosshair_color = static_cast<int32_t>(hud::HudLayout::kCrosshairColorDefault);
	options.crosshair_spread = hud::HudLayout::kCrosshairSpreadDefault;
	options.aspect_mode = 0;
	options.keyboard_tips = true;
	options.gameplay_tips = true;
	options.object_polydetail = renderer::kObjectLodDetailFreshProfile;
	options.texfilter_level = renderer::kTexFilterLevelFreshProfile;
	options.particle_density = renderer::kParticleDensityFreshProfile;
	options.texcompression_level = renderer::kTexCompressionLevelFreshProfile;
	options.terrain_texdetail = renderer::kTerrainTexDetailFreshProfile;
	options.object_texdetail = renderer::kObjectTexDetailFreshProfile;
	return options;
}

PlayerOptions normalized_player_options(const PlayerOptions &in) {
	PlayerOptions out = in;
	out.sound_fx_volume = clamp_to_control(in.sound_fx_volume, "SOUNDFXVOLUME");
	out.dialog_volume = clamp_to_control(in.dialog_volume, "DIALOGVOLUME");
	out.music_volume = clamp_to_control(in.music_volume, "MUSICVOLUME");
	out.crosshair_style =
			std::clamp(in.crosshair_style, hud::kHudCrosshairStyleMin, hud::kHudCrosshairStyleMax);
	out.crosshair_color = static_cast<int32_t>(static_cast<uint32_t>(in.crosshair_color) &
			hud::HudLayout::kCrosshairColorMask);
	out.object_polydetail = renderer::clamp_object_lod_detail(in.object_polydetail);
	out.texfilter_level = renderer::clamp_texfilter_level(in.texfilter_level);
	out.particle_density = renderer::clamp_particle_density(in.particle_density);
	return out;
}

PlayerOptions read_player_options(const gamecfg::GameCfg &cfg) {
	PlayerOptions options;
	options.sound_fx_volume = cfg.sfx_level;
	options.dialog_volume = cfg.dialogue_level;
	options.music_volume = cfg.music_volume;
	options.crosshair_style = cfg.xhair_appearance;
	options.crosshair_color = cfg.xhair_color;
	options.crosshair_spread = cfg.xhair_spread != 0;
	options.aspect_mode = cfg.display_16x9;
	options.keyboard_tips = cfg.enable_keyboardtips != 0;
	options.gameplay_tips = cfg.enable_gameplaytips != 0;
	options.object_polydetail = cfg.object_polydetail;
	options.texfilter_level = cfg.texfilter_level;
	options.particle_density = cfg.particle_density;
	options.texcompression_level = cfg.texcompression_level;
	options.terrain_texdetail = cfg.terrain_texdetail;
	options.object_texdetail = cfg.object_texdetail;
	return normalized_player_options(options);
}

PlayerOptions write_player_options(gamecfg::GameCfg &cfg, const PlayerOptions &in) {
	const PlayerOptions options = normalized_player_options(in);
	cfg.sfx_level = options.sound_fx_volume;
	cfg.dialogue_level = options.dialog_volume;
	cfg.music_volume = options.music_volume;
	cfg.xhair_appearance = options.crosshair_style;
	cfg.xhair_color = options.crosshair_color;
	cfg.xhair_spread = options.crosshair_spread ? 1 : 0;
	cfg.display_16x9 = options.aspect_mode;
	cfg.enable_keyboardtips = options.keyboard_tips ? 1 : 0;
	cfg.enable_gameplaytips = options.gameplay_tips ? 1 : 0;
	cfg.object_polydetail = options.object_polydetail;
	cfg.texfilter_level = options.texfilter_level;
	cfg.particle_density = options.particle_density;
	cfg.texcompression_level = options.texcompression_level;
	cfg.terrain_texdetail = options.terrain_texdetail;
	cfg.object_texdetail = options.object_texdetail;
	return options;
}

PlayerOptionsWords player_options_words(const PlayerOptions &o) {
	PlayerOptionsWords words{};
	words[index(OptionsWord::SoundFxVolume)] = o.sound_fx_volume;
	words[index(OptionsWord::DialogVolume)] = o.dialog_volume;
	words[index(OptionsWord::MusicVolume)] = o.music_volume;
	words[index(OptionsWord::CrosshairStyle)] = o.crosshair_style;
	words[index(OptionsWord::CrosshairColor)] = o.crosshair_color;
	words[index(OptionsWord::CrosshairSpread)] = o.crosshair_spread ? 1 : 0;
	words[index(OptionsWord::KeyboardTips)] = o.keyboard_tips ? 1 : 0;
	words[index(OptionsWord::GameplayTips)] = o.gameplay_tips ? 1 : 0;
	words[index(OptionsWord::AspectMode)] = o.aspect_mode;
	words[index(OptionsWord::ObjectPolyDetail)] = o.object_polydetail;
	words[index(OptionsWord::TexFilterLevel)] = o.texfilter_level;
	words[index(OptionsWord::ParticleDensity)] = o.particle_density;
	words[index(OptionsWord::TexCompressionLevel)] = o.texcompression_level;
	words[index(OptionsWord::TerrainTexDetail)] = o.terrain_texdetail;
	words[index(OptionsWord::ObjectTexDetail)] = o.object_texdetail;
	return words;
}

PlayerOptions player_options_from_words(const PlayerOptions &options, const int32_t *w,
		size_t count) {
	if (w == nullptr || count < static_cast<size_t>(OptionsWord::kCount)) return options;
	PlayerOptions o = options;
	o.sound_fx_volume = w[index(OptionsWord::SoundFxVolume)];
	o.dialog_volume = w[index(OptionsWord::DialogVolume)];
	o.music_volume = w[index(OptionsWord::MusicVolume)];
	o.crosshair_style = w[index(OptionsWord::CrosshairStyle)];
	o.crosshair_color = w[index(OptionsWord::CrosshairColor)];
	o.crosshair_spread = w[index(OptionsWord::CrosshairSpread)] != 0;
	o.keyboard_tips = w[index(OptionsWord::KeyboardTips)] != 0;
	o.gameplay_tips = w[index(OptionsWord::GameplayTips)] != 0;
	o.aspect_mode = w[index(OptionsWord::AspectMode)];
	o.object_polydetail = w[index(OptionsWord::ObjectPolyDetail)];
	o.texfilter_level = w[index(OptionsWord::TexFilterLevel)];
	o.particle_density = w[index(OptionsWord::ParticleDensity)];
	o.texcompression_level = w[index(OptionsWord::TexCompressionLevel)];
	o.terrain_texdetail = w[index(OptionsWord::TerrainTexDetail)];
	o.object_texdetail = w[index(OptionsWord::ObjectTexDetail)];
	return o;
}

} // namespace opennova::inmatch
