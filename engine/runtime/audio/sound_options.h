#pragma once

// The process's three sound option words and the sound-effects option's park. The words are the
// cfg's SFXLevel, DialogueLevel and music_volume [orig: Game_ApplySessionSettingsToGlobals
// @0x551581..0x5515A2 (SFXLevel into g_SoundVolumeOption @0x24D20CC, DialogueLevel into
// g_DialogueVolume @0x24D20C8); UI_IngameOptionsDialogEventHandler @0x554F57..0x554F83; the
// music's through AudioVM_SetGlobalVolume @0x671F20, called @0x5255A4]. The sound calls scale
// a voice's volume byte by them as they open or update its mixer channel: (volume * option) >> 8
// for the one-shots and the emitter channels [orig: Sound_Play3DPositional @0x527E2C;
// Sound_PlayTriggerSetScaled @0x527B9D; Sound_PlayInterfaceTriggerSet @0x527BED;
// SoundEmitter_UpdateAndMixTop8 @0x52891A], the dialogue's for the dialog line and the scripted
// voice [orig: Dialog_ApplyDialogueVolumeAndPlayFactor @0x527560; Audio_UpdateAmbientStream
// @0x4EDB2C], the music's global volume for the music stream (audio::MusicStream).
//
// The single-player round end parks the sound-effects option: with none saved it saves the word
// and zeroes it, so every sound opened or updated after falls silent while a sound already
// sounding plays out at the volume it opened with; the cine release puts the saved word back
// [orig: Sound_ParkEffectsVolume @0x5280B0, called by Server_ProcessRoundEnd @0x516958;
// Sound_RestoreEffectsVolume @0x5280D0, called by Cine_ReleaseAll @0x577940]. The world says when
// the park stands (world::EpilogCine::world_sound_parked).

#include <cstdint>

namespace opennova::audio {

inline constexpr int32_t kVolumeByteMax = 255;
// Music, SFX, dialogue and rotor channels share this initial profile value.
// [orig: Config_SetDefaults @0x54d030]
inline constexpr int32_t kDefaultChannelVolume = 192;

class SoundOptions {
public:
	// The options' words as the options write them.
	void set(int32_t sound_fx, int32_t dialogue, int32_t music) {
		sound_fx_ = sound_fx;
		dialogue_ = dialogue;
		music_ = music;
	}
	int32_t sound_fx() const { return sound_fx_; }
	int32_t dialogue() const { return dialogue_; }
	int32_t music() const { return music_; }

	// The park: with nothing saved (the saved word 0), the word saved and zeroed [orig:
	// Sound_ParkEffectsVolume @0x5280B0..0x5280CE].
	void park() {
		if (parked_ != 0) return;
		parked_ = sound_fx_;
		sound_fx_ = 0;
	}
	// The restore: a saved word back, the park cleared [orig: Sound_RestoreEffectsVolume
	// @0x5280D0..0x5280E9].
	void restore() {
		if (parked_ == 0) return;
		sound_fx_ = parked_;
		parked_ = 0;
	}
	bool parked() const { return parked_ != 0; }

private:
	int32_t sound_fx_ = kDefaultChannelVolume;
	int32_t dialogue_ = kDefaultChannelVolume;
	int32_t music_ = kDefaultChannelVolume;
	int32_t parked_ = 0; // g_SoundVolumeParked @0x24E0898
};

// A volume byte through an option word: (volume * option) >> 8 [orig: Sound_Play3DPositional
// @0x527E1F..0x527E2C].
inline int32_t scaled_volume(int32_t volume, int32_t option) { return (volume * option) >> 8; }

} // namespace opennova::audio
