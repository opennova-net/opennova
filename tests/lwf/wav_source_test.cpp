// A wave as the game's loader takes it and as a modder brings it (formats/lwf/wav_source.h). What the
// loader takes, by its own walk [orig: Audio_LoadWavFileFromArchive @ 0x766480]: the minted mono 16-bit
// tone; not a stereo wave, a 24-bit or float one, a LIST ahead of the data, an ADPCM wave with no fact
// chunk. A wave the game refuses converts into one it takes, the samples kept; a wave's facts: the
// format, the peak and the RMS of a sine at 0.5, its picture. The retail leg (OPENNOVA_JO_ASSETS): every
// shipped wave the loader takes but DSkid.wav (D-SND-33), by the check and by the runtime's decode.
#include <formats/lwf/wav_source.h>

#include <formats/bfc1/bfc1.h>
#include <formats/lwf/wav_pcm.h>

#include <base/io/le.h>
#include <base/io/os_path.h>
#include <base/io/strutil.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "common/file_io.h"
#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "common/test_paths.h"

using namespace opennova;
using namespace opennova::lwf;

namespace {

std::string repo() { return test_paths_repo_root(__FILE__); }

std::vector<uint8_t> text_bytes(const std::string &text) { return std::vector<uint8_t>(text.begin(), text.end()); }

// A RIFF WAVE of `frames` frames of a 441 Hz sine at amplitude 0.5, each channel the same, its samples
// `bits` wide (8 unsigned, 16, 24; 32 float with `real`), a LIST chunk before its data when `list`.
std::vector<uint8_t> wave_of(uint16_t channels, uint16_t bits, uint32_t rate, size_t frames, bool list = false,
                             bool real = false) {
	std::vector<uint8_t> data;
	for (size_t f = 0; f < frames; ++f) {
		const double s = 0.5 * std::sin(2.0 * 3.14159265358979 * 441.0 * double(f) / double(rate));
		for (uint16_t c = 0; c < channels; ++c) {
			if (real) {
				float v = float(s);
				uint32_t u;
				std::memcpy(&u, &v, 4);
				io::append_u32_le(data, u);
			} else if (bits == 8) {
				data.push_back(uint8_t(std::lround(s * 127.0) + 128));
			} else if (bits == 16) {
				io::append_u16_le(data, uint16_t(int16_t(std::lround(s * 32767.0))));
			} else {
				const int32_t v = int32_t(std::lround(s * 8388607.0));
				data.push_back(uint8_t(v));
				data.push_back(uint8_t(v >> 8));
				data.push_back(uint8_t(v >> 16));
			}
		}
	}
	std::vector<uint8_t> out;
	const auto text = [&](const char *t) { out.insert(out.end(), t, t + 4); };
	std::vector<uint8_t> info;
	if (list) {
		const char body[] = "INFOISFT\x0e\0\0\0Lavf58.29.100\0";
		info.assign(body, body + sizeof(body) - 1);
	}
	text("RIFF");
	io::append_u32_le(out, uint32_t(4 + 24 + (list ? 8 + info.size() : 0) + 8 + data.size()));
	text("WAVE");
	text("fmt ");
	io::append_u32_le(out, 16);
	io::append_u16_le(out, real ? 3 : 1);
	io::append_u16_le(out, channels);
	io::append_u32_le(out, rate);
	io::append_u32_le(out, rate * channels * (bits / 8));
	io::append_u16_le(out, uint16_t(channels * (bits / 8)));
	io::append_u16_le(out, bits);
	if (list) {
		text("LIST");
		io::append_u32_le(out, uint32_t(info.size()));
		out.insert(out.end(), info.begin(), info.end());
	}
	text("data");
	io::append_u32_le(out, uint32_t(data.size()));
	out.insert(out.end(), data.begin(), data.end());
	return out;
}

// What the game's loader takes, by its own walk: the minted mono 16-bit tone; not a stereo wave, a 24-bit
// or float one, a LIST ahead of the data, an ADPCM wave with no fact chunk. A wave the game refuses
// converts into one it takes, the samples kept; a wave's facts: the format, the peak and the RMS of a
// sine at 0.5, its picture.
int test_waves() {
	const std::vector<uint8_t> tone = test_io::read_file(repo() + "/fixtures/lwf/tone.wav");
	TEST_EXPECT(wave_retail_check(tone).plays);
	TEST_EXPECT(wave_retail_check(wave_of(1, 8, 11025, 100)).plays);
	const WaveRetailCheck stereo = wave_retail_check(wave_of(2, 16, 44100, 100));
	TEST_EXPECT(!stereo.plays && stereo.why.find("2 channels") != std::string::npos);
	TEST_EXPECT(wave_retail_check(wave_of(1, 24, 48000, 100)).why.find("24-bit") != std::string::npos);
	TEST_EXPECT(wave_retail_check(wave_of(1, 32, 48000, 100, false, true)).why.find("32-bit") != std::string::npos);
	const WaveRetailCheck listed = wave_retail_check(wave_of(1, 16, 22050, 100, true));
	TEST_EXPECT(!listed.plays && listed.why.find("LIST") != std::string::npos);
	// A LIST after the data is never reached.
	std::vector<uint8_t> after = wave_of(1, 16, 22050, 100);
	const char tail[] = "LIST\x04\0\0\0INFO";
	after.insert(after.end(), tail, tail + sizeof(tail) - 1);
	TEST_EXPECT(wave_retail_check(after).plays);
	std::vector<uint8_t> adpcm = wave_of(1, 16, 22050, 100);
	adpcm[20] = 0x11;
	adpcm[34] = 4;
	TEST_EXPECT(wave_retail_check(adpcm).why.find("fact") != std::string::npos);
	TEST_EXPECT(!wave_retail_check(text_bytes("not a wave")).plays);
	// The loader's own buffer, AUD1, is taken unchecked; an AOA1 one is refused at the RIFF compare [orig:
	// Audio_LoadWavFileFromArchive @ 0x7664e2, @ 0x766523]. Its samples read as the mixer reads them: 16-bit
	// where its width byte is 2, else 8-bit [orig: sub_7BD671 @ 0x7bd692].
	std::vector<uint8_t> aud = text_bytes("AUD1");
	io::append_u32_le(aud, 2);
	io::append_u32_le(aud, 32768);
	io::append_u32_le(aud, 2);
	io::append_u16_le(aud, 0x4000);
	io::append_u16_le(aud, 0xC000);
	TEST_EXPECT(wave_retail_check(aud).plays);
	// One ending inside its 16-byte header the runtime's decode refuses (the game reads its count, pitch
	// and width past the bytes), and so does the check.
	const WaveRetailCheck short_aud = wave_retail_check(std::vector<uint8_t>(aud.begin(), aud.begin() + 10));
	TEST_EXPECT(!short_aud.plays && short_aud.why.find("header") != std::string::npos);
	WaveSamples own;
	std::string own_error;
	TEST_EXPECT(decode_wave_source(aud, own, own_error) && own.format.aud1 && own.format.bits == 16 &&
	            own.frames() == 2 && own.samples[0] == 0.5f && own.samples[1] == -0.5f &&
	            wave_format_words(own.format) == "16-bit AUD1, mono, 22050 Hz");
	aud[12] = 1;
	TEST_EXPECT(decode_wave_source(aud, own, own_error) && own.format.bits == 8 && own.frames() == 2);
	std::vector<uint8_t> aoa = aud;
	aoa[1] = 'O';
	TEST_EXPECT(!wave_retail_check(aoa).plays && !decode_wave_source(aoa, own, own_error));
	// Converted: mono 16-bit, the rate kept, then resampled.
	const std::vector<uint8_t> source = wave_of(2, 24, 48000, 4800, true);
	std::vector<uint8_t> converted;
	std::string error;
	TEST_EXPECT(convert_wave(source, WaveConversion(), converted, error) && wave_retail_check(converted).plays);
	WaveSamples back;
	TEST_EXPECT(decode_wave_source(converted, back, error) && back.channels == 1 && back.rate == 48000 &&
	            back.format.bits == 16 && back.frames() == 4800);
	WaveConversion halved;
	halved.rate = 24000;
	halved.bits = "8";
	TEST_EXPECT(convert_wave(source, halved, converted, error) && decode_wave_source(converted, back, error) &&
	            back.frames() == 2400 && back.format.bits == 8 && back.rate == 24000);
	// No sample: refused before the writer, which holds at least one (the loader steps over an empty data
	// chunk and walks past the file's end [orig: Audio_LoadWavFileFromArchive @ 0x76659b..0x7665a5]).
	TEST_EXPECT(!convert_wave(wave_of(1, 16, 22050, 0), WaveConversion(), converted, error) &&
	            error == "it holds no sample");
	TEST_EXPECT(!convert_wave(wave_of(2, 8, 22050, 0), halved, converted, error) && error == "it holds no sample");
	// An IMA ADPCM source of rate 0 is refused as a PCM one is, kept or resampled (no division by its rate).
	std::vector<uint8_t> rateless = wave_of(1, 16, 22050, 100);
	rateless[20] = 0x11;
	rateless[34] = 4;
	rateless[32] = 36;
	rateless[33] = 0;
	for (size_t i = 24; i < 32; ++i) rateless[i] = 0;
	TEST_EXPECT(!decode_wave_source(rateless, back, error) && error.find("IMA ADPCM, mono, 0 Hz") != std::string::npos);
	TEST_EXPECT(!convert_wave(rateless, WaveConversion(), converted, error));
	TEST_EXPECT(!convert_wave(rateless, halved, converted, error));
	// The facts.
	const WaveFacts facts = wave_facts(source, 16);
	TEST_EXPECT(facts.read && !facts.retail.plays && facts.format.channels == 2 && facts.format.bits == 24 &&
	            std::fabs(facts.seconds - 0.1) < 1e-6 && std::fabs(facts.peak - 0.5f) < 0.01f &&
	            std::fabs(facts.rms - 0.3536f) < 0.01f && facts.envelope.size() == 16);
	TEST_EXPECT(wave_format_words(facts.format) == "24-bit PCM, stereo, 48000 Hz");
	TEST_EXPECT(std::fabs(wave_seconds(source) - 0.1) < 1e-6 && wave_seconds(text_bytes("not a wave")) == 0.0);
	// What a conversion changes, in words, and the bits it writes.
	WaveSamples stereo_source;
	TEST_EXPECT(decode_wave_source(source, stereo_source, error));
	TEST_EXPECT(wave_bits_written(stereo_source, WaveConversion()) == 16 && wave_bits_written(stereo_source, halved) == 8);
	TEST_EXPECT(wave_conversion_words(stereo_source, halved) ==
	            "2 channels mixed to mono, 24-bit PCM written as 8-bit PCM, 48000 Hz resampled to 24000 Hz");
	// A trim keeps the frames from start to end; a gain scales each sample, clamped to full scale; the peak a
	// normalise divides by is the kept channel's.
	WaveConversion trimmed;
	trimmed.start = 1000;
	trimmed.end = 1600;
	TEST_EXPECT(convert_wave(source, trimmed, converted, error) && decode_wave_source(converted, back, error) &&
	            back.frames() == 600 && back.rate == 48000);
	TEST_EXPECT(std::fabs(wave_conversion_peak(source, trimmed) - 0.5f) < 0.01f);
	WaveConversion louder = trimmed;
	louder.gain = 1.0f / wave_conversion_peak(source, trimmed);
	WaveFacts loud;
	TEST_EXPECT(convert_wave(source, louder, converted, error) && (loud = wave_facts(converted)).read &&
	            loud.peak > 0.99f && loud.retail.plays);
	// Clamped, never wrapped: every sample the gain takes past full scale is written at full scale, its sign
	// kept (a wrap would turn it over), every other one as scaled.
	louder.gain = 4.0f;
	TEST_EXPECT(convert_wave(source, louder, converted, error) && decode_wave_source(converted, back, error) &&
	            back.frames() == 600 && back.format.bits == 16);
	size_t clamped = 0, kept_sign = 0, scaled = 0, unclamped = 0;
	for (size_t i = 0; i < back.frames() && back.frames() == 600; ++i) {
		const size_t f = 1000 + i;
		const float mix = (stereo_source.samples[f * 2] + stereo_source.samples[f * 2 + 1]) * 0.5f;
		const float want = mix * 4.0f;
		if (std::fabs(want) > 1.0f) {
			++clamped;
			kept_sign += std::fabs(back.samples[i]) > 0.999f && (back.samples[i] > 0.0f) == (want > 0.0f) ? 1 : 0;
		} else {
			++unclamped;
			scaled += std::fabs(back.samples[i] - want) < 0.001f ? 1 : 0;
		}
	}
	TEST_EXPECT(clamped > 0 && kept_sign == clamped && unclamped > 0 && scaled == unclamped);
	TEST_EXPECT(wave_conversion_words(stereo_source, louder) ==
	            "2 channels mixed to mono, 24-bit PCM written as 16-bit PCM, frames 1000..1600 of 4800 kept, scaled by "
	            "4.00 (+12.04 dB)");
	WaveConversion past = trimmed;
	past.start = 5000;
	past.end = 0;
	TEST_EXPECT(!convert_wave(source, past, converted, error) && error == "the trim keeps no sample");
	TEST_EXPECT(wave_conversion_peak(source, past) < 0.0f);
	past.start = 0;
	past.gain = -1.0f;
	TEST_EXPECT(!convert_wave(source, past, converted, error));
	std::printf("waves: the loader's walk, a conversion it takes, the facts\n");
	return 0;
}

// Every shipped wave the game's loader takes, by the check's own walk, but one: DSkid.wav, the 16-bit
// stereo wave game.lwf's IMP_TMBL_DSKID plays, which the loader's channel test refuses [orig:
// Audio_LoadWavFileFromArchive @ 0x7666db], so retail plays nothing for that set (D-SND-33). The
// runtime's decode, handed the bytes as the VFS reads them (BFC1 undone), refuses the same one.
int test_retail_waves() {
	const std::string assets = retail::assets();
	if (assets.empty()) {
		retail::skip_leg("OPENNOVA_JO_ASSETS (every shipped wave through the loader's walk)");
		return 0;
	}
	size_t waves = 0;
	std::vector<std::string> refused, undecoded;
	std::error_code ec;
	for (const auto &entry : std::filesystem::directory_iterator(io::os_path(assets), ec)) {
		if (strutil::to_lower(entry.path().extension().string()) != ".wav") continue;
		std::vector<uint8_t> bytes = test_io::read_file(io::utf8_path(entry.path()));
		const WaveRetailCheck check = wave_retail_check(bytes);
		++waves;
		const std::string name = strutil::to_lower(io::utf8_path(entry.path().filename()));
		if (!check.plays) refused.push_back(name);
		WavPcm pcm;
		std::string error;
		if (!bfc1::bfc1_unpack(bytes) || !wav_decode_pcm16(bytes.data(), bytes.size(), pcm, error))
			undecoded.push_back(name);
	}
	std::printf("retail: %zu waves, %zu the check refuses, %zu the decode refuses\n", waves, refused.size(),
	            undecoded.size());
	TEST_EXPECT(waves > 100 && refused == std::vector<std::string>({"dskid.wav"}));
	TEST_EXPECT(undecoded == std::vector<std::string>({"dskid.wav"}));
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	int failed = 0;
	failed += test_waves();
	failed += test_retail_waves();
	if (failed == 0) std::printf("wav_source: all tests passed\n");
	return failed == 0 ? 0 : 1;
}
