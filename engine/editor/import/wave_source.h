#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <editor/import/importer.h>
#include <formats/lwf/wav_source.h>

namespace opennova::editor {

// A wave as the game takes it, and as a modder brings it (ADR 0046, the sound lane). The game loads a wave
// a bank's single names, or a script plays, through one loader [orig: Audio_LoadWavFileFromArchive @
// 0x766480] (docs/audio/lwf-dbf-sound-re.md "The wave loader's rules"): BFC1-compressed or not, an AOA1
// buffer taken as it is, else a RIFF WAVE walked chunk by chunk to its data, whose samples are mono 8-bit or
// 16-bit PCM or mono 4-bit IMA ADPCM with a fact chunk, at any rate (played at rate / 44100 of the device's).
// The rest is refused and the sound plays nothing. A modder's wave (a DAW's 24-bit stereo with its metadata)
// is read as an audio program reads it, never through the game's faults, and written in the form the game
// takes. The loader's walk (lwf::wave_retail_check), the read, the facts and the conversion are the
// engine's (formats/lwf/wav_source.h); the importer over them is the editor's.

// The wave importer (importer.h; the sound lane): a wave its import record makes a source (record_extensions:
// .wav) made into the wave the game plays, by the record's options, each a row of wave_import_option_rows:
// `channels` (mono, left, right), `bits` (keep, 16, 8), `rate` (keep, 11025, 22050, 44100, or a rate of 4000
// to 96000), `name` (the output's file name; left out, the source's stem and .wav). The same rows' defaults
// make an author's wave the game refuses into one it plays as it comes into the project
// (assets/asset_import.cpp: prepare_authored_wave).
inline constexpr int kWaveImporterVersion = 1;
const std::vector<ImportOptionRow> &wave_import_option_rows();
lwf::WaveConversion wave_conversion_of(const ImportOptions &options);
bool run_wave_import(ImportContext &context, ImportProduct &out);

// An author's wave as it comes into the project (import_assets): as it is where the game plays it; else made
// into one it plays by the importer's defaults, `note` saying what changed; false, with `error`, where it
// reads as no wave at all.
bool prepare_authored_wave(const std::string &name, std::vector<uint8_t> &bytes, std::string &note, std::string &error);

} // namespace opennova::editor
