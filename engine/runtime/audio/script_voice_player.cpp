#include <runtime/audio/script_voice_player.h>

#include <base/io/fixed.h>
#include <runtime/audio/mixer.h>
#include <runtime/audio/sound_listener.h>

namespace opennova::audio {

namespace {

// The local voice plays at pan 0; another's at its speaker's bearing from the listener [orig:
// Audio_UpdateAmbientStream @0x4EDA90..0x4EDAD8; Audio_Play3DPositionalSound @0x4ECCF8..0x4ECD39].
uint8_t voice_pan(const SoundListener &listener, const ScriptVoicePlayer::Frame &frame) {
	if (frame.local) return 0;
	return listener.voice_pan_of(int32_t(frame.position.x * io::kFp16One), int32_t(frame.position.y * io::kFp16One));
}

} // namespace

void ScriptVoicePlayer::follow(Mixer &mixer, const SoundListener &listener, const Frame &frame) {
	if (frame.serial == frame_.serial && frame.state.clip.get() == frame_.state.clip.get()) return;
	stop(mixer);
	frame_ = frame;
	if (!frame.state.clip) return;
	const MixWaveRef wave = mix_wave_from_pcm(*frame.state.clip);
	if (!wave) return;
	// [orig: Audio_Play3DPositionalSound @0x4ECD79 -> AudioChannel_PlaySound(0, ...)]
	handle_ = mixer.open(kVoiceChannel,
			voice_params(wave, voice_pan(listener, frame), static_cast<uint8_t>(frame.volume), frame.state.pitch_q16));
}

bool ScriptVoicePlayer::lapsed(Mixer &mixer) const {
	// [orig: Audio_UpdateAmbientStream @0x4EDA20 -> AudioChannel_ValidateHandle]
	return frame_.state.clip && mixer.validate(handle_) == 0;
}

void ScriptVoicePlayer::place(Mixer &mixer, const SoundListener &listener, const Frame &frame) {
	if (handle_ == 0 || !frame_.state.clip) return;
	// [orig: Audio_UpdateAmbientStream @0x4EDB3E -> AudioChannel_SetAndPlay]
	mixer.set_and_play(handle_, voice_pan(listener, frame), static_cast<uint8_t>(frame.volume), frame.state.pitch_q16);
}

void ScriptVoicePlayer::stop(Mixer &mixer) {
	if (frame_.state.clip) mixer.reset(handle_);
	handle_ = 0;
	frame_ = {};
}

} // namespace opennova::audio
