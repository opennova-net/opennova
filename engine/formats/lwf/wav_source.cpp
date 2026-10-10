#include <formats/lwf/wav_source.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <base/io/le.h>
#include <formats/bfc1/bfc1.h>
#include <formats/lwf/wav_pcm.h>

namespace opennova::lwf {

namespace {

uint32_t u32_at(const std::vector<uint8_t> &b, size_t at) { return at + 4 <= b.size() ? io::read_u32_le(b.data() + at) : 0; }
uint16_t u16_at(const std::vector<uint8_t> &b, size_t at) { return at + 2 <= b.size() ? io::read_u16_le(b.data() + at) : 0; }
bool tag_at(const std::vector<uint8_t> &b, size_t at, const char *tag) {
	return at + 4 <= b.size() && std::memcmp(b.data() + at, tag, 4) == 0;
}
std::string tag_text(const std::vector<uint8_t> &b, size_t at) {
	std::string out;
	for (size_t i = 0; i < 4 && at + i < b.size(); ++i) {
		const char c = char(b[at + i]);
		out += (c >= 0x20 && c < 0x7F) ? c : '?';
	}
	return out;
}

// The bytes as the loader holds them: a BFC1 file decompressed first [orig: AudioFile_DecompressBFC, called
// @ 0x7664cb]; a BFC1 file that does not decompress is held as stored.
std::vector<uint8_t> as_loaded(const std::vector<uint8_t> &bytes) {
	if (!bfc1::bfc1_is_bfc1(bytes.data(), bytes.size())) return bytes;
	uint32_t size = 0;
	if (bfc1::bfc1_uncompressed_size(bytes.data(), bytes.size(), &size) != 0) return bytes;
	std::vector<uint8_t> out(size);
	size_t out_size = out.size();
	if (bfc1::bfc1_decompress(bytes.data(), bytes.size(), out.empty() ? nullptr : out.data(), &out_size) != 0)
		return bytes;
	out.resize(out_size);
	return out;
}

WaveRetailCheck refused(std::string why) {
	WaveRetailCheck out;
	out.why = std::move(why);
	return out;
}

} // namespace

WaveRetailCheck wave_retail_check(const std::vector<uint8_t> &bytes) {
	const std::vector<uint8_t> data = as_loaded(bytes);
	// An AOA1 buffer is the loader's own form, copied as it is [orig: @ 0x7664e7].
	if (tag_at(data, 0, "AOA1")) {
		WaveRetailCheck ok;
		ok.plays = true;
		return ok;
	}
	// The loader's walk, as it walks (wav_pcm.h's wave_loader_walk, which the runtime's decode walks too).
	const WaveLoaderWalk walk = wave_loader_walk(data.data(), data.size());
	switch (walk.refusal) {
		case WaveRefusal::None: break;
		case WaveRefusal::NotRiffWave:
			return refused("It is neither a RIFF WAVE nor an AOA1 buffer, which the game's loader refuses [orig: "
			               "Audio_LoadWavFileFromArchive @ 0x76653b].");
		case WaveRefusal::EndsBeforeData:
			return refused("The file ends before its data: the game's loader reads past it for a data chunk.");
		case WaveRefusal::SecondFmt:
			return refused("It holds two fmt chunks ahead of its data, which the game's loader refuses [orig: "
			               "Audio_LoadWavFileFromArchive @ 0x766589].");
		case WaveRefusal::ChunkPastMost:
			return refused(walk.after_list
			                       ? "A LIST chunk (the metadata many programs write) comes before its data: the game's "
			                         "loader steps into it, reads its list type as a chunk's size and refuses the file "
			                         "[orig: Audio_LoadWavFileFromArchive @ 0x766570, @ 0x7665a3]."
			                       : "A '" + tag_text(data, walk.chunk) + "' chunk of " + std::to_string(walk.chunk_size) +
			                                 " bytes comes before its data; the game's loader refuses any chunk there "
			                                 "past 2048 bytes [orig: Audio_LoadWavFileFromArchive @ 0x7665a3].");
		case WaveRefusal::TwelveChunks:
			return refused("Twelve chunks or more come before its data, which the game's loader gives up at [orig: "
			               "Audio_LoadWavFileFromArchive @ 0x7665b2].");
		case WaveRefusal::NoFmt:
			return refused("No fmt chunk comes before its data, which the game's loader refuses [orig: "
			               "Audio_LoadWavFileFromArchive @ 0x7665cb].");
		case WaveRefusal::Bits:
			return refused("Its samples are " + std::to_string(walk.bits) +
			               "-bit; the game's loader takes 8-bit and 16-bit PCM and 4-bit IMA ADPCM alone [orig: "
			               "Audio_LoadWavFileFromArchive @ 0x76675f].");
		case WaveRefusal::Channels:
			return refused("It has " + std::to_string(walk.channels) +
			               " channels; the game's loader takes a mono wave alone [orig: "
			               "Audio_LoadWavFileFromArchive @ 0x7665e3, @ 0x7666db, @ 0x76676a].");
		case WaveRefusal::NoFact:
			return refused("Its 4-bit samples have no fact chunk ahead of the data, which the game's loader refuses [orig: "
			               "Audio_LoadWavFileFromArchive @ 0x766772].");
		case WaveRefusal::NotImaAdpcm:
			return refused("Its 4-bit samples are not IMA ADPCM, the one 4-bit form the game's loader takes [orig: "
			               "Audio_LoadWavFileFromArchive @ 0x76677d].");
	}
	WaveRetailCheck ok;
	ok.plays = true;
	return ok;
}

std::string wave_format_words(const WaveFormat &format) {
	std::string kind;
	if (format.aoa1) kind = std::to_string(format.bits) + "-bit AOA1";
	else if (format.tag == kWaveTagImaAdpcm) kind = "IMA ADPCM";
	else if (format.tag == kWaveTagFloat) kind = std::to_string(format.bits) + "-bit float";
	else if (format.tag == kWaveTagPcm || format.tag == kWaveTagExtensible) kind = std::to_string(format.bits) + "-bit PCM";
	else kind = "format " + std::to_string(format.tag);
	const std::string channels = format.channels == 1 ? "mono" : format.channels == 2 ? "stereo"
	                                                                                   : std::to_string(format.channels) + " channels";
	return kind + ", " + channels + ", " + std::to_string(format.rate) + " Hz";
}

bool decode_wave_source(const std::vector<uint8_t> &bytes, WaveSamples &out, std::string &error) {
	out = WaveSamples();
	const std::vector<uint8_t> data = as_loaded(bytes);
	const auto through_game = [&](WaveFormat format) {
		WavPcm pcm;
		if (!wav_decode_pcm16_lenient(data.data(), data.size(), pcm, error)) return false;
		if (pcm.sample_rate == 0) {
			// Refused as the PCM forms below are: no rate to keep and none to resample from.
			format.rate = 0;
			format.channels = pcm.channels;
			error = "its samples are " + wave_format_words(format) + ", which this decoder does not read";
			return false;
		}
		out.format = format;
		out.rate = pcm.sample_rate;
		out.channels = pcm.channels;
		out.format.rate = pcm.sample_rate;
		out.format.channels = pcm.channels;
		out.samples.resize(pcm.pcm16.size() / 2);
		for (size_t i = 0; i < out.samples.size(); ++i)
			out.samples[i] = float(int16_t(io::read_u16_le(pcm.pcm16.data() + 2 * i))) / 32768.0f;
		return true;
	};
	if (tag_at(data, 0, "AOA1")) {
		WaveFormat format;
		format.aoa1 = true;
		format.bits = data.size() > 12 ? uint16_t(data[12] * 8) : 0;
		return through_game(format);
	}
	if (!tag_at(data, 0, "RIFF") || !tag_at(data, 8, "WAVE")) {
		error = "it is neither a RIFF WAVE nor an AOA1 buffer";
		return false;
	}
	// The chunks as the RIFF form lays them (each padded to an even size).
	size_t at = 12, fmt = 0, fmt_size = 0, data_at = 0, data_size = 0;
	bool has_fmt = false, has_data = false;
	while (at + 8 <= data.size()) {
		const uint32_t size = u32_at(data, at + 4);
		if (tag_at(data, at, "fmt ") && !has_fmt) {
			has_fmt = true;
			fmt = at + 8;
			fmt_size = size;
		} else if (tag_at(data, at, "data") && !has_data) {
			has_data = true;
			data_at = at + 8;
			data_size = std::min<size_t>(size, data.size() - data_at);
		}
		at += 8 + size_t(size) + (size & 1);
	}
	if (!has_fmt || fmt_size < 16 || fmt + 16 > data.size() || !has_data) {
		error = has_fmt ? "it has no data chunk" : "it has no fmt chunk";
		return false;
	}
	WaveFormat format;
	format.tag = u16_at(data, fmt);
	format.channels = u16_at(data, fmt + 2);
	format.rate = u32_at(data, fmt + 4);
	format.block_align = u16_at(data, fmt + 12);
	format.bits = u16_at(data, fmt + 14);
	if (format.tag == kWaveTagImaAdpcm) return through_game(format);
	uint16_t sample_tag = format.tag;
	if (format.tag == kWaveTagExtensible && fmt_size >= 40 && fmt + 26 <= data.size()) sample_tag = u16_at(data, fmt + 24);
	const uint16_t bytes_per = uint16_t(format.bits / 8);
	const bool pcm = sample_tag == kWaveTagPcm && (format.bits == 8 || format.bits == 16 || format.bits == 24 || format.bits == 32);
	const bool real = sample_tag == kWaveTagFloat && (format.bits == 32 || format.bits == 64);
	if ((!pcm && !real) || format.channels == 0 || format.rate == 0) {
		error = "its samples are " + wave_format_words(format) + ", which this decoder does not read";
		return false;
	}
	out.format = format;
	out.rate = format.rate;
	out.channels = format.channels;
	const size_t count = data_size / bytes_per;
	out.samples.resize(count - count % format.channels);
	const uint8_t *p = data.data() + data_at;
	for (size_t i = 0; i < out.samples.size(); ++i, p += bytes_per) {
		float v = 0.0f;
		if (real && format.bits == 32) {
			float f;
			std::memcpy(&f, p, 4);
			v = f;
		} else if (real) {
			double d;
			std::memcpy(&d, p, 8);
			v = float(d);
		} else if (format.bits == 8) {
			v = (float(p[0]) - 128.0f) / 128.0f;
		} else if (format.bits == 16) {
			v = float(int16_t(io::read_u16_le(p))) / 32768.0f;
		} else if (format.bits == 24) {
			const int32_t s = int32_t(uint32_t(p[0]) << 8 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 24) >> 8;
			v = float(s) / 8388608.0f;
		} else {
			v = float(int32_t(io::read_u32_le(p))) / 2147483648.0f;
		}
		out.samples[i] = std::isfinite(v) ? std::clamp(v, -1.0f, 1.0f) : 0.0f;
	}
	return true;
}

WaveFacts wave_facts(const std::vector<uint8_t> &bytes, size_t bins) {
	WaveFacts facts;
	facts.retail = wave_retail_check(bytes);
	WaveSamples wave;
	if (!decode_wave_source(bytes, wave, facts.error)) return facts;
	facts.read = true;
	facts.format = wave.format;
	facts.frames = wave.frames();
	facts.seconds = wave.rate ? double(facts.frames) / double(wave.rate) : 0.0;
	double sum = 0.0;
	for (const float s : wave.samples) {
		facts.peak = std::max(facts.peak, std::fabs(s));
		sum += double(s) * double(s);
	}
	facts.rms = wave.samples.empty() ? 0.0f : float(std::sqrt(sum / double(wave.samples.size())));
	facts.envelope.assign(facts.frames ? std::min<size_t>(bins, size_t(facts.frames)) : 0, 0.0f);
	for (size_t frame = 0; frame < facts.frames; ++frame) {
		const size_t bin = size_t(frame * facts.envelope.size() / facts.frames);
		for (uint16_t c = 0; c < wave.channels; ++c)
			facts.envelope[bin] = std::max(facts.envelope[bin], std::fabs(wave.samples[frame * wave.channels + c]));
	}
	return facts;
}

double wave_seconds(const std::vector<uint8_t> &bytes) {
	WaveSamples wave;
	std::string error;
	if (!decode_wave_source(bytes, wave, error) || !wave.rate) return 0.0;
	return double(wave.frames()) / double(wave.rate);
}

int wave_bits_written(const WaveSamples &source, const WaveConversion &conversion) {
	if (conversion.bits == "8") return 8;
	if (conversion.bits == "16") return 16;
	return source.format.tag == kWaveTagPcm && source.format.bits == 8 && !source.format.aoa1 ? 8 : 16;
}

namespace {

// The one channel a conversion makes of `wave`: the mix, or the one taken, its frames from start to end.
std::vector<float> conversion_channel(const WaveSamples &wave, const WaveConversion &conversion) {
	const uint64_t frames = wave.frames();
	const uint64_t end = conversion.end && conversion.end < frames ? conversion.end : frames;
	const uint64_t start = conversion.start < end ? conversion.start : end;
	std::vector<float> mono(size_t(end - start));
	const uint16_t channels = wave.channels;
	for (size_t i = 0; i < mono.size(); ++i) {
		const size_t f = size_t(start) + i;
		if (conversion.channels == "left") mono[i] = wave.samples[f * channels];
		else if (conversion.channels == "right") mono[i] = wave.samples[f * channels + (channels > 1 ? 1 : 0)];
		else {
			float sum = 0.0f;
			for (uint16_t c = 0; c < channels; ++c) sum += wave.samples[f * channels + c];
			mono[i] = sum / float(channels);
		}
	}
	return mono;
}

} // namespace

float wave_conversion_peak(const std::vector<uint8_t> &source, const WaveConversion &conversion) {
	WaveSamples wave;
	std::string error;
	if (!decode_wave_source(source, wave, error)) return -1.0f;
	const std::vector<float> mono = conversion_channel(wave, conversion);
	if (mono.empty()) return -1.0f;
	float peak = 0.0f;
	for (const float s : mono) peak = std::max(peak, std::fabs(s));
	return std::min(peak, 1.0f);
}

bool convert_wave(const std::vector<uint8_t> &source, const WaveConversion &conversion, std::vector<uint8_t> &out,
                  std::string &error) {
	WaveSamples wave;
	if (!decode_wave_source(source, wave, error)) return false;
	if (wave.frames() == 0) {
		error = "it holds no sample";
		return false;
	}
	if (!(conversion.gain >= 0.0f) || !std::isfinite(conversion.gain)) {
		error = "its gain is no number of 0 or more";
		return false;
	}
	// One channel: the mix, or the one taken, its frames from start to end.
	std::vector<float> mono = conversion_channel(wave, conversion);
	if (mono.empty()) {
		error = "the trim keeps no sample";
		return false;
	}
	// The rate: kept, or resampled by straight lines between the source's samples.
	const uint32_t rate = conversion.rate ? conversion.rate : wave.rate;
	std::vector<float> samples;
	if (rate == wave.rate) {
		samples = std::move(mono);
	} else {
		const size_t frames = std::max<size_t>(1, size_t(std::llround(double(mono.size()) * rate / wave.rate)));
		samples.resize(frames);
		for (size_t i = 0; i < frames; ++i) {
			const double at = double(i) * wave.rate / rate;
			const size_t i0 = std::min(size_t(at), mono.size() - 1), i1 = std::min(i0 + 1, mono.size() - 1);
			const float t = float(at - double(i0));
			samples[i] = mono[i0] * (1.0f - t) + mono[i1] * t;
		}
	}
	const int bits = wave_bits_written(wave, conversion);
	std::vector<uint8_t> data;
	data.reserve(samples.size() * size_t(bits / 8));
	for (const float s : samples) {
		const float v = std::clamp(s * conversion.gain, -1.0f, 1.0f);
		if (bits == 8) data.push_back(uint8_t(std::clamp(std::lround(v * 127.0f) + 128, 0L, 255L)));
		else io::append_u16_le(data, uint16_t(int16_t(std::clamp(std::lround(v * 32767.0f), -32768L, 32767L))));
	}
	// The plain RIFF the game's loader reads, through the format's writer.
	return wav_write_pcm_mono(data.data(), data.size(), rate, uint16_t(bits), out, error);
}

std::string wave_conversion_words(const WaveSamples &source, const WaveConversion &conversion) {
	std::vector<std::string> parts;
	if (source.channels > 1)
		parts.push_back(conversion.channels == "left"    ? "the left channel taken"
		                : conversion.channels == "right" ? "the right channel taken"
		                                                 : std::to_string(source.channels) + " channels mixed to mono");
	const int bits = wave_bits_written(source, conversion);
	const bool same_samples = source.format.tag == kWaveTagPcm && source.format.bits == bits;
	if (!same_samples) parts.push_back(wave_format_words(source.format).substr(0, wave_format_words(source.format).find(',')) +
	                                   " written as " + std::to_string(bits) + "-bit PCM");
	if (conversion.rate && conversion.rate != source.rate)
		parts.push_back(std::to_string(source.rate) + " Hz resampled to " + std::to_string(conversion.rate) + " Hz");
	// The trim (the frames conversion_channel keeps) and the gain.
	const uint64_t frames = source.frames();
	const uint64_t end = conversion.end && conversion.end < frames ? conversion.end : frames;
	const uint64_t start = conversion.start < end ? conversion.start : end;
	if (start > 0 || end < frames)
		parts.push_back("frames " + std::to_string(start) + ".." + std::to_string(end) + " of " + std::to_string(frames) +
		                " kept");
	if (conversion.gain != 1.0f) {
		char gain[64];
		if (conversion.gain > 0.0f)
			std::snprintf(gain, sizeof(gain), "scaled by %.2f (%+.2f dB)", double(conversion.gain),
			              20.0 * std::log10(double(conversion.gain)));
		else
			std::snprintf(gain, sizeof(gain), "scaled by 0 (silent)");
		parts.push_back(gain);
	}
	std::string out;
	for (size_t i = 0; i < parts.size(); ++i) out += (i ? ", " : "") + parts[i];
	return out;
}

} // namespace opennova::lwf
