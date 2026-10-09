// NovaLogic .wav payload decode: the RIFF chunk walk and the IMA-ADPCM
// (audioFormat 0x11) block decode the voice/zone audio ships in, plus the
// PCM8/PCM16 normalizations — everything emits interleaved signed 16-bit LE
// PCM. Moved from the shell binding's WavLoader (ADR 0031: format
// semantics live engine-side; the binding boxes the result into an
// AudioStreamWAV).
#pragma once

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
	// What the game's wave loader records for the wave, its AOA1 buffer's +4 and +8 (a dialog line's
	// hold reads both, audio::dialog_clip_hold): the sample count, the data's bytes for 8-bit samples,
	// half of them for 16-bit, the `fact` chunk's count for IMA ADPCM (the decoded frames where a wave
	// has none, which the loader refuses), an AOA1's own; and the pitch ratio,
	// ((rate << 16) + 22050) / 44100, an AOA1's own [orig: Audio_LoadWavFileFromArchive @ 0x766480,
	// the count @ 0x766609 / @ 0x76670e / @ 0x7667ba, the ratio @ 0x76662d / @ 0x766735 / @ 0x7667e1].
	uint32_t loader_samples = 0;
	uint32_t loader_pitch_q16 = 0;
};

// Decode RIFF/WAVE or AOA1 to 16-bit PCM. AOA1 stores mono signed PCM8/16,
// a sample count and Q16 rate relative to 44100 Hz; trailing mixer padding is
// excluded. RIFF handles PCM8 (unsigned, upconverted
// signed<<8), PCM16 (passthrough), and IMA-ADPCM 0x11 (mono block-based; the
// defensive stereo path interleaves 4-byte nibble words round-robin per
// channel). A truncated final data chunk clamps and plays what is present.
// Returns false with r_error set on a malformed/unsupported stream.
bool wav_decode_pcm16(const uint8_t *bytes, size_t size, WavPcm &r_out,
		std::string &r_error);

// The plain RIFF/WAVE the game's wave loader reads (docs/audio/lwf-dbf-sound-re.md, "The wave
// loader's rules" [orig: Audio_LoadWavFileFromArchive @ 0x766480]): RIFF..WAVE, one 16-byte `fmt `
// chunk (PCM, mono: the loader refuses a second channel @ 0x7665e3) before one `data` chunk, nothing
// else, the data padded to an even size. `data` is the samples as stored, `bits` 8 (unsigned) or 16
// (signed little-endian), at `rate` per second. False with `error` for no sample (the loader steps over
// an empty data chunk and walks past the file's end, @ 0x76659b..0x7665a5), a 16-bit data of an odd size,
// other bits, or a rate of 0.
bool wav_write_pcm_mono(const uint8_t *data, size_t size, uint32_t rate, uint16_t bits,
		std::vector<uint8_t> &out, std::string &error);

}  // namespace lwf
}  // namespace opennova
