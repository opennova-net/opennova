// NovaLogic .wav payload decode: the RIFF chunk walk and the IMA-ADPCM
// (audioFormat 0x11) block decode the voice/zone audio ships in, plus the
// PCM8/PCM16 normalizations — everything emits interleaved signed 16-bit LE
// PCM. Moved from the shell adapter's NovaWavLoader (ADR 0031: format
// semantics live engine-side; the adapter boxes the result into an
// AudioStreamWAV).
#ifndef OPENNOVA_LWF_WAV_PCM_H
#define OPENNOVA_LWF_WAV_PCM_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova {
namespace lwf {

struct WavPcm {
	std::vector<uint8_t> pcm16;  // interleaved signed 16-bit LE frames
	uint32_t sample_rate = 0;
	uint16_t channels = 0;  // 1 or 2
};

// Decode a RIFF/WAVE buffer to 16-bit PCM. Handles PCM8 (unsigned, upconverted
// signed<<8), PCM16 (passthrough), and IMA-ADPCM 0x11 (mono block-based; the
// defensive stereo path interleaves 4-byte nibble words round-robin per
// channel). A truncated final data chunk clamps and plays what is present.
// Returns false with r_error set on a malformed/unsupported stream.
bool wav_decode_pcm16(const uint8_t *bytes, size_t size, WavPcm &r_out,
		std::string &r_error);

}  // namespace lwf
}  // namespace opennova

#endif  // OPENNOVA_LWF_WAV_PCM_H
