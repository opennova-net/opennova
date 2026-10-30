#pragma once

#include <cstdint>
#include <string>

// The audio output (ADR 0077 d2, d4: the engine's platform API, backend-neutral): one PCM sink the
// engine opens at its own rate, channel count and sample format and pushes interleaved frames
// into; the backend plays them, converting to the device's format and rate as it must. The mix,
// the panning, the resampling and the sample shift are the engine's (runtime/audio); the device
// keeps the output gain and reports the level the engine's audio meter follows.
//
// The design follows SDL3's SDL_AudioStream (SDL_OpenAudioDeviceStream with an SDL_AudioSpec of
// format, channels and freq and no callback, SDL_PutAudioStreamData, SDL_GetAudioStreamQueued,
// SDL_SetAudioStreamGain, SDL_ResumeAudioStreamDevice) and miniaudio's pushed stream (an
// ma_device fed from an ma_pcm_rb ring the engine writes): a push model, the engine keeping the
// queue as full as it wants it by the queued count; GLFW and sokol_app have no audio (sokol_audio
// is the same stream model).
namespace opennova::platform {

// Interleaved samples: S16, signed 16-bit little-endian (retail's mixer's own); F32, 32-bit
// float, full scale +-1.0.
enum class SampleFormat : uint8_t { S16, F32 };

struct AudioSpec {
	uint32_t rate = 44100; // frames per second
	uint8_t channels = 2;
	SampleFormat format = SampleFormat::S16;
};

// The bytes one frame (a sample for each channel) takes.
constexpr uint32_t audio_frame_bytes(const AudioSpec &spec) {
	return static_cast<uint32_t>(spec.channels) * (spec.format == SampleFormat::S16 ? 2u : 4u);
}

class AudioDevice {
public:
	virtual ~AudioDevice() = default;

	// The sink opened at `spec` and playing (silence until frames come). False with `error` when
	// no device opens; the engine then runs without sound. An open sink is closed first.
	virtual bool open(const AudioSpec &spec, std::string &error) = 0;
	// The sink closed, its queued frames dropped.
	virtual void close() = 0;
	virtual bool is_open() const = 0;

	// `frame_count` interleaved frames at the open spec queued behind the ones still waiting.
	// False when the sink is not open or did not take them.
	virtual bool push(const void *frames, uint32_t frame_count) = 0;
	// The frames pushed and not yet played.
	virtual uint32_t queued_frames() const = 0;

	// The output gain, linear (1.0 unchanged, 0 silent), applied to every frame the device plays.
	virtual void set_gain(float gain) = 0;

	// The output's peak since the last read: the largest absolute sample of the frames pushed
	// since then, before the gain, full scale 1.0 (0 when none came); reading starts the next
	// window. The engine's audio meter follows it (runtime/audio/audio_out_meter.h).
	virtual float take_peak() = 0;
};

} // namespace opennova::platform
