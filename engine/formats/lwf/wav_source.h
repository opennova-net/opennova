#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova::lwf {

// A wave as the game takes it, and as a modder brings it. The game loads a wave a bank's single names,
// or a script plays, through one loader [orig: Audio_LoadWavFileFromArchive @ 0x766480]
// (docs/audio/lwf-dbf-sound-re.md "The wave loader's rules"): BFC1-compressed or not, an AUD1 buffer
// (the loader's own form) taken as it is, else a RIFF WAVE walked chunk by chunk to its data, whose
// samples are mono 8-bit or 16-bit PCM or mono 4-bit IMA ADPCM with a fact chunk, at any rate (played
// at rate / 44100 of the device's). The rest is refused and the sound plays nothing. That walk is
// wav_pcm.h's wave_loader_walk, which the runtime's decode (wav_decode_pcm16) walks and wave_retail_check
// words.
// The rest of this file is tooling, not a port: a modder's wave (a DAW's 24-bit stereo with its
// metadata) is read as an audio program reads it, never through the game's faults, and written in the
// form the game takes.

// Whether the game's loader takes the bytes (as stored: BFC1 is undone first, as the loader does), and the
// first refusal in words where it does not, each from the loader's own walk [orig: Audio_LoadWavFileFromArchive
// @ 0x766480]: neither AUD1 nor RIFF WAVE (@ 0x766523, @ 0x76653b); a twelfth chunk ahead of the data
// (@ 0x7665b2); a chunk ahead of the data past 2048 bytes (@ 0x7665a3), which a LIST chunk always is, the
// walk stepping 4 bytes into it and reading its list type as a size (@ 0x766570); a second fmt chunk
// (@ 0x766589); no fmt ahead of the data (@ 0x7665cb); samples of other than 8, 16 or 4 bits (@ 0x76675f);
// more than one channel (@ 0x7665e3, @ 0x7666db, @ 0x76676a); 4-bit samples with no fact chunk or of another
// format than IMA ADPCM (0x11) (@ 0x766772, @ 0x76677d).
struct WaveRetailCheck {
	bool plays = false;
	std::string why;
};
WaveRetailCheck wave_retail_check(const std::vector<uint8_t> &bytes);

// A fmt chunk's format tags.
inline constexpr uint16_t kWaveTagPcm = 1;
inline constexpr uint16_t kWaveTagFloat = 3;
inline constexpr uint16_t kWaveTagImaAdpcm = 0x11;
inline constexpr uint16_t kWaveTagExtensible = 0xFFFE;

// The format a RIFF WAVE's fmt chunk says (an AUD1 buffer's: mono PCM of its width; tag 0 for neither).
struct WaveFormat {
	uint16_t tag = 0;      // kWaveTagPcm, kWaveTagFloat, kWaveTagImaAdpcm, kWaveTagExtensible
	uint16_t channels = 0;
	uint32_t rate = 0;
	uint16_t bits = 0;
	uint16_t block_align = 0;
	bool aud1 = false;
};
// The format in words: "16-bit PCM, mono, 22050 Hz".
std::string wave_format_words(const WaveFormat &format);

// A wave's samples as an audio program reads them: interleaved, each -1..1, and the format they came in.
// PCM of 8 (unsigned), 16, 24 or 32 bits, IEEE float of 32 or 64 bits, the extensible form of either, IMA
// ADPCM and AUD1 (through the game's decode as tooling reads it, wav_decode_pcm16_lenient: stereo, or no
// fact chunk, read too), BFC1 undone first. False, with `error`, for anything else.
struct WaveSamples {
	WaveFormat format;
	uint32_t rate = 0;
	uint16_t channels = 0;
	std::vector<float> samples;
	size_t frames() const { return channels ? samples.size() / channels : 0; }
};
bool decode_wave_source(const std::vector<uint8_t> &bytes, WaveSamples &out, std::string &error);

// What a wave's facts are: its format, length, loudest sample and RMS (each 0..1 of full scale), a coarse
// picture of it (`envelope`: each bin's loudest sample, 0..1), and whether the game plays it, and why not.
struct WaveFacts {
	bool read = false;
	std::string error;
	WaveFormat format;
	uint64_t frames = 0;
	double seconds = 0.0;
	float peak = 0.0f;
	float rms = 0.0f;
	std::vector<float> envelope;
	WaveRetailCheck retail;
};
WaveFacts wave_facts(const std::vector<uint8_t> &bytes, size_t bins = 48);
// How long a wave plays, in seconds, as it decodes (its frames at its rate); 0 for one that does not read.
double wave_seconds(const std::vector<uint8_t> &bytes);

// The form a wave is written in for the game: its channels mixed to one (`mono`) or one of them taken
// (`left`, `right`); its frames from `start` to `end` kept (a trim; `end` 0 to the last); each sample times
// `gain`, clamped to full scale (1 as it is; a normalise gives the mix's peak over full scale); its samples
// 16-bit, or 8-bit, or (`keep`) 8-bit where the source is 8-bit PCM and 16-bit otherwise; its rate kept (0) or
// resampled (linear) to another. A plain RIFF WAVE: fmt (PCM) and data, no other chunk, which the game's loader
// takes [orig: Audio_LoadWavFileFromArchive @ 0x766480].
struct WaveConversion {
	std::string channels = "mono";
	std::string bits = "keep";
	uint32_t rate = 0;
	uint64_t start = 0;
	uint64_t end = 0;
	float gain = 1.0f;
};
bool convert_wave(const std::vector<uint8_t> &source, const WaveConversion &conversion, std::vector<uint8_t> &out,
                  std::string &error);
// The loudest sample of the one channel a conversion of `source` makes (its mix or the one taken, its frames
// from start to end, before its gain and its rate), 0..1 of full scale: what a normalise divides by. -1 for a
// source that does not read or a trim that keeps no frame.
float wave_conversion_peak(const std::vector<uint8_t> &source, const WaveConversion &conversion);
// The bits a conversion of `source` writes: 8 or 16.
int wave_bits_written(const WaveSamples &source, const WaveConversion &conversion);
// What a conversion changes of a source, in words ("2 channels mixed to mono, 24-bit PCM written as 16-bit
// PCM, frames 1000..1600 of 4800 kept, scaled by 4.00 (+12.04 dB)"); "" for none.
std::string wave_conversion_words(const WaveSamples &source, const WaveConversion &conversion);

} // namespace opennova::lwf
