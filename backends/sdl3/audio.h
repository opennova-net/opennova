#pragma once

#include <base/platform/audio_device.h>

#include <SDL3/SDL.h>

#include <string>

// The audio sink over an SDL_AudioStream bound to the default playback device
// (platform/audio_device.h): the engine opens it at its own spec and pushes interleaved frames;
// SDL converts them to the device's format and rate and plays them. The gain is the stream's;
// the peak is read off each push before it. Backend internal.
namespace opennova::sdl3 {

class AudioDevice final : public platform::AudioDevice {
public:
	AudioDevice() = default;
	~AudioDevice() override { close(); }
	AudioDevice(const AudioDevice &) = delete;
	AudioDevice &operator=(const AudioDevice &) = delete;

	bool open(const platform::AudioSpec &spec, std::string &error) override;
	void close() override;
	bool is_open() const override { return stream_ != nullptr; }
	bool push(const void *frames, uint32_t frame_count) override;
	uint32_t queued_frames() const override;
	void set_gain(float gain) override;
	float take_peak() override;

private:
	SDL_AudioStream *stream_ = nullptr;
	platform::AudioSpec spec_;
	float gain_ = 1.0f;
	float peak_ = 0.0f;
};

} // namespace opennova::sdl3
