#include "audio.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace opennova::sdl3 {

bool AudioDevice::open(const platform::AudioSpec &spec, std::string &error) {
	close();
	if (spec.channels == 0 || spec.rate == 0) {
		error = "the audio spec has no channels or no rate";
		return false;
	}
	SDL_AudioSpec sdl{};
	sdl.format = spec.format == platform::SampleFormat::S16 ? SDL_AUDIO_S16LE : SDL_AUDIO_F32LE;
	sdl.channels = spec.channels;
	sdl.freq = static_cast<int>(spec.rate);
	// No callback: the engine pushes, SDL pulls from the stream's queue as the device plays.
	stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &sdl, nullptr, nullptr);
	if (stream_ == nullptr) {
		error = std::string("SDL_OpenAudioDeviceStream: ") + SDL_GetError();
		return false;
	}
	spec_ = spec;
	peak_ = 0.0f;
	SDL_SetAudioStreamGain(stream_, gain_);
	// A device stream opens paused.
	if (!SDL_ResumeAudioStreamDevice(stream_)) {
		error = std::string("SDL_ResumeAudioStreamDevice: ") + SDL_GetError();
		close();
		return false;
	}
	return true;
}

void AudioDevice::close() {
	if (stream_ == nullptr) return;
	SDL_DestroyAudioStream(stream_);
	stream_ = nullptr;
}

bool AudioDevice::push(const void *frames, uint32_t frame_count) {
	if (stream_ == nullptr) return false;
	if (frame_count == 0) return true;
	const uint32_t bytes = frame_count * platform::audio_frame_bytes(spec_);
	// The peak before the gain, full scale 1.0 (platform/audio_device.h take_peak).
	const uint32_t samples = frame_count * spec_.channels;
	float peak = peak_;
	if (spec_.format == platform::SampleFormat::S16) {
		const auto *s = static_cast<const uint8_t *>(frames);
		for (uint32_t i = 0; i < samples; ++i) {
			int16_t v = 0;
			std::memcpy(&v, s + 2 * i, sizeof v);
			peak = std::max(peak, std::fabs(static_cast<float>(v)) / 32768.0f);
		}
	} else {
		const auto *s = static_cast<const uint8_t *>(frames);
		for (uint32_t i = 0; i < samples; ++i) {
			float v = 0.0f;
			std::memcpy(&v, s + 4 * i, sizeof v);
			peak = std::max(peak, std::fabs(v));
		}
	}
	if (!SDL_PutAudioStreamData(stream_, frames, static_cast<int>(bytes))) return false;
	peak_ = peak;
	return true;
}

uint32_t AudioDevice::queued_frames() const {
	if (stream_ == nullptr) return 0;
	const int bytes = SDL_GetAudioStreamQueued(stream_);
	return bytes > 0 ? static_cast<uint32_t>(bytes) / platform::audio_frame_bytes(spec_) : 0;
}

void AudioDevice::set_gain(float gain) {
	gain_ = gain;
	if (stream_ != nullptr) SDL_SetAudioStreamGain(stream_, gain_);
}

float AudioDevice::take_peak() {
	const float peak = peak_;
	peak_ = 0.0f;
	return peak;
}

} // namespace opennova::sdl3
