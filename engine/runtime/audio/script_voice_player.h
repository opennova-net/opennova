#pragma once

// The scripted voice on the mixer: the clip the world's channel holds (world::ScriptVoiceChannel)
// played on the mixer's voice channel, zero, and kept on it as retail's sound calls keep it. A start
// opens the channel on the clip at the frame's gain and pan [orig: Audio_Play3DPositionalSound
// @0x4ECD79 -> AudioChannel_PlaySound(0, ...)]; each scene frame validates the channel's handle,
// a channel that no longer plays the clip finishing the voice [orig: Audio_UpdateAmbientStream
// @0x4EDA20 -> AudioChannel_ValidateHandle], then sets the channel's pan, gain and play factor anew
// [orig: Audio_UpdateAmbientStream @0x4EDB3E -> AudioChannel_SetAndPlay]. The pan is the local
// voice's 0, else the speaker's bearing from the listener (SoundListener::voice_pan_of).

#include <cstdint>

#include <runtime/world/script_voice.h>

namespace opennova::audio {

class Mixer;
class SoundListener;

class ScriptVoicePlayer {
public:
	using Frame = world::ScriptVoiceChannel::Frame;

	// The device follows the channel: a new generation or clip stops the voice and opens channel
	// zero on the new clip (none for a frame without one); the same clip leaves the channel be.
	void follow(Mixer &mixer, const SoundListener &listener, const Frame &frame);
	// The scene frame's validation: true when a voice's channel stopped playing its clip. The
	// world's finish is the caller's to report (ScriptVoiceChannel::finish, with frame()), then
	// stop().
	bool lapsed(Mixer &mixer) const;
	// The scene frame's channel update: pan, the frame's distance gain and the clip's play factor.
	void place(Mixer &mixer, const SoundListener &listener, const Frame &frame);
	// The voice let go: its channel reset to the silent buffer, the clip forgotten.
	void stop(Mixer &mixer);

	// Channel zero's handle for the clip (0 none) and the frame the voice opened with.
	uint32_t handle() const { return handle_; }
	const Frame &frame() const { return frame_; }

private:
	uint32_t handle_ = 0;
	Frame frame_;
};

} // namespace opennova::audio
