#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <editor/import/importer.h>

namespace opennova::editor {

// A wave as the game takes it, and as a modder brings it (ADR 0046, the sound lane). The game loads a wave
// a bank's single names, or a script plays, through one loader [orig: Audio_LoadWavFileFromArchive @
// 0x766480] (docs/audio/lwf-dbf-sound-re.md "The wave loader's rules"): BFC1-compressed or not, an AOA1
// buffer taken as it is, else a RIFF WAVE walked chunk by chunk to its data, whose samples are mono 8-bit or
// 16-bit PCM or mono 4-bit IMA ADPCM with a fact chunk, at any rate (played at rate / 44100 of the device's).
// The rest is refused and the sound plays nothing. A modder's wave (a DAW's 24-bit stereo with its metadata)
// is read as an audio program reads it, never through the game's faults, and written in the form the game
// takes: the editor's tooling, not a port.

// Whether the game's loader takes the bytes (as stored: BFC1 is undone first, as the loader does), and the
// first refusal in words where it does not, each from the loader's own walk [orig: Audio_LoadWavFileFromArchive
// @ 0x766480]: neither AOA1 nor RIFF WAVE (@ 0x76653b); a twelfth chunk ahead of the data (@ 0x7665b2); a
// chunk ahead of the data past 2048 bytes (@ 0x7665a3), which a LIST chunk always is, the walk stepping 4
// bytes into it and reading its list type as a size (@ 0x766570); a second fmt chunk (@ 0x766589); no fmt
// ahead of the data (@ 0x7665cb); samples of other than 8, 16 or 4 bits (@ 0x76675f); more than one channel
// (@ 0x7665e3, @ 0x7666db, @ 0x76676a); 4-bit samples with no fact chunk or of another format than IMA ADPCM
// (0x11) (@ 0x766772, @ 0x76677d).
struct WaveRetailCheck {
	bool plays = false;
	std::string why;
};
WaveRetailCheck wave_retail_check(const std::vector<uint8_t> &bytes);

// The format a RIFF WAVE's fmt chunk says (an AOA1 buffer's: mono PCM of its width; tag 0 for neither).
struct WaveFormat {
	uint16_t tag = 0;      // 1 PCM, 3 IEEE float, 0x11 IMA ADPCM, 0xFFFE extensible
	uint16_t channels = 0;
	uint32_t rate = 0;
	uint16_t bits = 0;
	uint16_t block_align = 0;
	bool aoa1 = false;
};
// The format in words: "16-bit PCM, mono, 22050 Hz".
std::string wave_format_words(const WaveFormat &format);

// A wave's samples as an audio program reads them: interleaved, each -1..1, and the format they came in.
// PCM of 8 (unsigned), 16, 24 or 32 bits, IEEE float of 32 or 64 bits, the extensible form of either, IMA
// ADPCM and AOA1 (through the game's own decode, lwf::wav_decode_pcm16), BFC1 undone first. False, with
// `error`, for anything else.
struct WaveSamples {
	WaveFormat format;
	uint32_t rate = 0;
	uint16_t channels = 0;
	std::vector<float> samples;
	size_t frames() const { return channels ? samples.size() / channels : 0; }
};
bool decode_wave_source(const std::vector<uint8_t> &bytes, WaveSamples &out, std::string &error);

// What a file card says of a wave (session/file_card.h): its format, length, loudest sample and RMS (each
// 0..1 of full scale), a coarse picture of it (`envelope`: each bin's loudest sample, 0..1), and whether the
// game plays it, and why not.
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
// (`left`, `right`); its samples 16-bit, or 8-bit, or (`keep`) 8-bit where the source is 8-bit PCM and 16-bit
// otherwise; its rate kept (0) or resampled (linear) to another. A plain RIFF WAVE: fmt (PCM) and data, no
// other chunk, which the game's loader takes [orig: Audio_LoadWavFileFromArchive @ 0x766480].
struct WaveConversion {
	std::string channels = "mono";
	std::string bits = "keep";
	uint32_t rate = 0;
};
bool convert_wave(const std::vector<uint8_t> &source, const WaveConversion &conversion, std::vector<uint8_t> &out,
                  std::string &error);
// What a conversion changes of a source, in words ("stereo mixed to mono, 24-bit written as 16-bit, the
// LIST chunk left out"); "" for none.
std::string wave_conversion_words(const WaveSamples &source, const WaveConversion &conversion);

// The wave importer (importer.h; the sound lane): a wave its import record makes a source (record_extensions:
// .wav) made into the wave the game plays, by the record's options, each a row of wave_import_option_rows:
// `channels` (mono, left, right), `bits` (keep, 16, 8), `rate` (keep, 11025, 22050, 44100, or a rate of 4000
// to 96000), `name` (the output's file name; left out, the source's stem and .wav). The same rows' defaults
// make an author's wave the game refuses into one it plays as it comes into the project
// (assets/asset_import.cpp: prepare_authored_wave).
inline constexpr int kWaveImporterVersion = 1;
const std::vector<ImportOptionRow> &wave_import_option_rows();
WaveConversion wave_conversion_of(const ImportOptions &options);
bool run_wave_import(ImportContext &context, ImportProduct &out);

// An author's wave as it comes into the project (import_assets): as it is where the game plays it; else made
// into one it plays by the importer's defaults, `note` saying what changed; false, with `error`, where it
// reads as no wave at all.
bool prepare_authored_wave(const std::string &name, std::vector<uint8_t> &bytes, std::string &note, std::string &error);

} // namespace opennova::editor
