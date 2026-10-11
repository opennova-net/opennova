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
	// The shell player's rate: a pitch of 0 the mixer's least step, 86; past INT32_MAX from an AUD1 pitch of
	// 0xBE37C63A or a RIFF rate of 0x80000000, which the shell boxes (wave_pitch_scale).
	uint32_t sample_rate = 0;
	uint16_t channels = 0;  // 1 (2 only from wav_decode_pcm16_lenient)
	// What the game's wave loader records for the wave, its AUD1 buffer's +4 and +8 (a dialog line's
	// hold reads both, audio::dialog_clip_hold): the sample count, the data chunk's size as it says
	// for 8-bit samples (past the file's bytes or not), half of it for 16-bit, the `fact` chunk's
	// count for IMA ADPCM (the lenient decode's decoded frames where a wave has none, which the loader
	// refuses), an AUD1's own; and the pitch ratio,
	// ((rate << 16) + 22050) / 44100, an AUD1's own [orig: Audio_LoadWavFileFromArchive @ 0x766480,
	// the count @ 0x766609 / @ 0x76670e / @ 0x7667ba, the ratio @ 0x76662d / @ 0x766735 / @ 0x7667e1].
	uint32_t loader_samples = 0;
	uint32_t loader_pitch_q16 = 0;
};

// Why the game's wave loader refuses a RIFF WAVE (docs/audio/lwf-dbf-sound-re.md, "The wave loader's
// rules" [orig: Audio_LoadWavFileFromArchive @ 0x766480]); None where it takes the wave.
enum class WaveRefusal : uint8_t {
	None,
	NotRiffWave,    // neither RIFF .. WAVE (@ 0x76653b)
	EndsBeforeData, // the walk runs off the bytes before a data chunk (the loader reads on past them)
	SecondFmt,      // a second fmt chunk ahead of the data (@ 0x766589)
	ChunkPastMost,  // a chunk ahead of the data past 0x800 bytes (@ 0x7665a3); a LIST's list type
	                // read as a size after the walk steps into it by its id alone (@ 0x766570)
	TwelveChunks,   // a twelfth chunk ahead of the data (@ 0x7665b2)
	NoFmt,          // no fmt chunk ahead of the data (@ 0x7665cb)
	Bits,           // samples of other than 8, 16 or 4 bits (@ 0x76675f)
	Channels,       // more than one channel (@ 0x7665e3, @ 0x7666db, @ 0x76676a)
	NoFact,         // 4-bit samples with no fact chunk (@ 0x766772)
	NotImaAdpcm,    // 4-bit samples of another format than IMA ADPCM, 0x11 (@ 0x76677d)
	RatioFaults,    // a rate from 0xAC440000, whose pitch ratio's division faults after every test above
	                // (@ 0x76662b, @ 0x766730, @ 0x7667dc): an original bug, the game's loader faulting
	                // where ours refuses the wave (D-SND-54)
};

// The game's wave loader's walk of a RIFF WAVE (BFC1 undone; an AUD1 buffer is the caller's) to its
// data and its tests there [orig: Audio_LoadWavFileFromArchive @ 0x766480]: from offset 12 a nested
// RIFF header stepped over (12 bytes), a LIST stepped into by its 4-byte id alone, the first fmt and
// the last fact kept, the walk stopped at the first data of nonzero size, every other chunk stepped
// over by its size, unpadded. The fmt fields are the bytes the loader reads at the fmt chunk, whatever
// its size; the walk's offsets and sizes are the bytes', the data size as its chunk says.
struct WaveLoaderWalk {
	WaveRefusal refusal = WaveRefusal::None;
	size_t chunk = 0;         // ChunkPastMost: the refused chunk's offset
	uint32_t chunk_size = 0;  // and its size as read
	bool after_list = false;  // ChunkPastMost: the chunk is the one a LIST's id step left the walk on
	size_t fmt = 0;           // the fmt chunk's offset
	bool has_fact = false;
	size_t fact = 0;          // the last fact chunk's offset ahead of the data
	size_t data = 0;          // the data's first byte
	uint32_t data_size = 0;
	uint16_t tag = 0;         // fmt +8, read for 4-bit samples alone
	uint16_t channels = 0;    // fmt +10
	uint32_t rate = 0;        // fmt +12
	uint16_t block_align = 0; // fmt +20
	uint8_t bits = 0;         // fmt +22, the one byte the loader reads
};
WaveLoaderWalk wave_loader_walk(const uint8_t *bytes, size_t size);

// Decode a wave as the game's loader takes it [orig: Audio_LoadWavFileFromArchive @ 0x766480] to
// 16-bit PCM: the loader's own AUD1 buffer (a sample count, a Q16 pitch ratio to the 44100 Hz device,
// mono signed samples, 16-bit where its width byte is 2 and 8-bit otherwise, shifted down by its byte
// at +13; the bytes past its count excluded), copied unchecked, else a RIFF WAVE through
// wave_loader_walk, its samples by their width alone: 8 bits PCM (unsigned, upconverted signed<<8),
// 16 bits PCM (passthrough), 4 bits mono block
// IMA ADPCM by the loader's own decode, exactly the fact chunk's count of its own nibble steps, read
// on from the data (none for a count of 0 or less). False with r_error set for an AUD1 buffer that
// ends inside its header and for every wave the walk refuses (an AOA1 buffer among them), which the
// game plays nothing for. A data chunk, an AUD1 buffer's samples or an IMA ADPCM count running past
// the bytes plays the bytes there.
bool wav_decode_pcm16(const uint8_t *bytes, size_t size, WavPcm &r_out,
		std::string &r_error);

// The tooling's read of the same forms, not a port: the RIFF chunks walked as the form lays them
// (padded to an even size, every chunk read, the last data kept), the format by its tag (1 PCM8 or
// PCM16, 0x11 IMA ADPCM: mono, or stereo interleaving 4-byte nibble words round-robin per channel),
// one or two channels, IMA ADPCM with or without a fact chunk, decoded as an audio program decodes
// it (every block the data holds, each nibble by the standard IMA steps, not the loader's); an AUD1
// buffer as wav_decode_pcm16 reads it. What lwf::decode_wave_source reads a modder's IMA ADPCM or
// AUD1 wave through, so a wave the game refuses can still be read and converted into one it takes.
bool wav_decode_pcm16_lenient(const uint8_t *bytes, size_t size, WavPcm &r_out,
		std::string &r_error);

// The pitch scale a player of a decoded wave takes over the one its voice composes (`play_scale`,
// the play factor), on a stream whose mix rate is `mix_rate`, the wave's own rate `sample_rate`
// (WavPcm's) as the shell's player holds it: the player's whole mix rate holds INT32_MAX, so a rate
// past it is boxed there and its scale carries the rest, sample_rate / mix_rate. The mixer's step
// composes the play factor with the wave's own pitch, (((play * factor) >> 16) * pitch + 0x400000)
// >> 23, so a wave of pitch 0 steps at 0 whatever the play factor and is forced to the least step
// [orig: AudioChannel_ComputeMixCoefficients @ 0x7bd4b0, the pitch's mul @ 0x7bd603, the force
// @ 0x7bd619..0x7bd61d]: the rate its decode already hands the player, so 1. Any other wave keeps
// `play_scale` over its own rate.
double wave_pitch_scale(uint32_t loader_pitch_q16, uint32_t sample_rate, uint32_t mix_rate, double play_scale);

// The plain RIFF/WAVE the game's wave loader reads (docs/audio/lwf-dbf-sound-re.md, "The wave
// loader's rules" [orig: Audio_LoadWavFileFromArchive @ 0x766480]): RIFF..WAVE, one 16-byte `fmt `
// chunk (PCM, mono: the loader refuses a second channel @ 0x7665e3) before one `data` chunk, nothing
// else, the data padded to an even size. `data` is the samples as stored, `bits` 8 (unsigned) or 16
// (signed little-endian), at `rate` per second. False with `error` for no sample (the loader steps over
// an empty data chunk and walks past the file's end, @ 0x76659b..0x7665a5), a 16-bit data of an odd size,
// other bits, a rate of 0, or a rate from 0xAC440000 (the loader's ratio division faults on it,
// WaveRefusal::RatioFaults).
bool wav_write_pcm_mono(const uint8_t *data, size_t size, uint32_t rate, uint16_t bits,
		std::vector<uint8_t> &out, std::string &error);

}  // namespace lwf
}  // namespace opennova
