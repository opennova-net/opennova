#pragma once

// The witnessed menu UI sound math — the per-play channel volume and pitch
// decisions of the LWF set player, pure over ints/doubles so the embedder
// only routes the selected members into its audio device
// [orig: the <SOUND> element play path widget_process_mouse_event @ 0x647a00
//  -> collection play @ 0x652de0 -> SoundBank_FindTriggerByName @ 0x75be90
//  -> SoundBank_PlayTriggerEntries @ 0x75ccd0]. Member selection per layer
// rides audio/sound_selector.h; the per-play jitter draws stay an accepted
// divergence (docs/audio/lwf-dbf-sound-re.md).

namespace opennova::menu {

// One pooled channel per original mixer slot: a set play opens up to eight
// channels, one per layer [orig: the 8-slot open loop @ 0x75ce06].
inline constexpr int kMenuSoundChannels = 8;

// The menu master volume default [orig: CGameMenu+92 initialises 255].
inline constexpr int kMenuMasterVolumeDefault = 255;

// The channel volume for one selected layer member [orig: @ 0x75cf25..
// 0x75cf6e]: a layer with no falloff radius (every shipped menu layer) plays
// at the menu's master volume and the member volume is not consulted; a
// falloff layer scales the member volume by (master+1)/256 toward its clamp
// (the distance curve @ 0x75ca20 evaluated at the UI emitter's distance 0).
inline int menu_channel_volume(int master_volume, int member_volume,
		int clamp_volume, int falloff_radius) {
	if (falloff_radius <= 0) {
		return master_volume;
	}
	int att = (member_volume * (master_volume + 1)) >> 8;
	if (clamp_volume > 0 && att > clamp_volume) {
		att = clamp_volume;
	}
	return att;
}

// Set-level pitch composes multiplicatively with the member pitch (Q16;
// 0xFFFF ~ 1.0) [orig: (member * set) >> 16 @ 0x75c0be]; a degenerate
// product resets to 1.0.
inline double menu_effective_pitch(double member_pitch, double set_pitch) {
	const double pitch = member_pitch * set_pitch;
	return pitch <= 0.01 ? 1.0 : pitch;
}

}  // namespace opennova::menu
