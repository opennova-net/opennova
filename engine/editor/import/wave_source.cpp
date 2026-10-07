#include <editor/import/wave_source.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include <base/io/le.h>
#include <base/io/strutil.h>
#include <base/vfs/vfs_decode.h>
#include <editor/assets/asset_registry.h>
#include <editor/project/project_files.h>
#include <formats/lwf/wav_pcm.h>

namespace opennova::editor {

namespace {

constexpr uint16_t kTagPcm = 1;
constexpr uint16_t kTagFloat = 3;
constexpr uint16_t kTagImaAdpcm = 0x11;
constexpr uint16_t kTagExtensible = 0xFFFE;
// The loader's limits ahead of the data [orig: Audio_LoadWavFileFromArchive @ 0x766480]: a chunk past this
// size refused (@ 0x7665a3), and the walk given up at this many chunks (@ 0x7665b2).
constexpr uint32_t kChunkMost = 0x800;
constexpr int kChunksMost = 12;

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
// @ 0x7664cb].
std::vector<uint8_t> as_loaded(const std::vector<uint8_t> &bytes) {
	std::vector<uint8_t> out = bytes;
	if (!vfs_decode_payload(out)) out = bytes;
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
	if (!tag_at(data, 0, "RIFF") || !tag_at(data, 8, "WAVE"))
		return refused("It is neither a RIFF WAVE nor an AOA1 buffer, which the game's loader refuses [orig: "
		               "Audio_LoadWavFileFromArchive @ 0x76653b].");
	// The loader's walk, as it walks: a nested RIFF header stepped over, LIST stepped into by its id alone,
	// every other chunk ahead of the data stepped over by its size, unpadded.
	size_t at = 12, fmt = 0, fact = 0;
	bool has_fmt = false, has_fact = false, after_list = false;
	for (int count = 0;;) {
		if (at + 8 > data.size())
			return refused("The file ends before its data: the game's loader reads past it for a data chunk.");
		const uint32_t size = u32_at(data, at + 4);
		if (tag_at(data, at, "RIFF")) {
			at += 12;
		} else if (tag_at(data, at, "LIST")) {
			at += 4;
			after_list = true;
		} else {
			if (tag_at(data, at, "data") && size != 0) break;
			if (tag_at(data, at, "fmt ")) {
				if (has_fmt)
					return refused("It holds two fmt chunks ahead of its data, which the game's loader refuses [orig: "
					               "Audio_LoadWavFileFromArchive @ 0x766589].");
				has_fmt = true;
				fmt = at;
			} else if (tag_at(data, at, "fact")) {
				has_fact = true;
				fact = at;
			}
			if (size > kChunkMost)
				return refused(after_list
				                       ? "A LIST chunk (the metadata many programs write) comes before its data: the game's "
				                         "loader steps into it, reads its list type as a chunk's size and refuses the file "
				                         "[orig: Audio_LoadWavFileFromArchive @ 0x766570, @ 0x7665a3]."
				                       : "A '" + tag_text(data, at) + "' chunk of " + std::to_string(size) +
				                                 " bytes comes before its data; the game's loader refuses any chunk there "
				                                 "past 2048 bytes [orig: Audio_LoadWavFileFromArchive @ 0x7665a3].");
			after_list = false;
			at += size_t(size) + 8;
		}
		if (++count >= kChunksMost)
			return refused("Twelve chunks or more come before its data, which the game's loader gives up at [orig: "
			               "Audio_LoadWavFileFromArchive @ 0x7665b2].");
	}
	(void)fact;
	if (!has_fmt || fmt + 24 > data.size())
		return refused("No fmt chunk comes before its data, which the game's loader refuses [orig: "
		               "Audio_LoadWavFileFromArchive @ 0x7665cb].");
	const uint8_t bits = data[fmt + 22];
	const uint16_t channels = u16_at(data, fmt + 10), tag = u16_at(data, fmt + 8);
	if (bits != 8 && bits != 16 && bits != 4)
		return refused("Its samples are " + std::to_string(bits) + "-bit; the game's loader takes 8-bit and 16-bit PCM and "
		               "4-bit IMA ADPCM alone [orig: Audio_LoadWavFileFromArchive @ 0x76675f].");
	if (channels != 1)
		return refused("It has " + std::to_string(channels) + " channels; the game's loader takes a mono wave alone [orig: "
		               "Audio_LoadWavFileFromArchive @ 0x7665e3, @ 0x7666db, @ 0x76676a].");
	if (bits == 4 && !has_fact)
		return refused("Its 4-bit samples have no fact chunk ahead of the data, which the game's loader refuses [orig: "
		               "Audio_LoadWavFileFromArchive @ 0x766772].");
	if (bits == 4 && tag != kTagImaAdpcm)
		return refused("Its 4-bit samples are not IMA ADPCM, the one 4-bit form the game's loader takes [orig: "
		               "Audio_LoadWavFileFromArchive @ 0x76677d].");
	WaveRetailCheck ok;
	ok.plays = true;
	return ok;
}

std::string wave_format_words(const WaveFormat &format) {
	std::string kind;
	if (format.aoa1) kind = std::to_string(format.bits) + "-bit AOA1";
	else if (format.tag == kTagImaAdpcm) kind = "IMA ADPCM";
	else if (format.tag == kTagFloat) kind = std::to_string(format.bits) + "-bit float";
	else if (format.tag == kTagPcm || format.tag == kTagExtensible) kind = std::to_string(format.bits) + "-bit PCM";
	else kind = "format " + std::to_string(format.tag);
	const std::string channels = format.channels == 1 ? "mono" : format.channels == 2 ? "stereo"
	                                                                                   : std::to_string(format.channels) + " channels";
	return kind + ", " + channels + ", " + std::to_string(format.rate) + " Hz";
}

bool decode_wave_source(const std::vector<uint8_t> &bytes, WaveSamples &out, std::string &error) {
	out = WaveSamples();
	const std::vector<uint8_t> data = as_loaded(bytes);
	const auto through_game = [&](WaveFormat format) {
		lwf::WavPcm pcm;
		if (!lwf::wav_decode_pcm16(data.data(), data.size(), pcm, error)) return false;
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
	if (format.tag == kTagImaAdpcm) return through_game(format);
	uint16_t sample_tag = format.tag;
	if (format.tag == kTagExtensible && fmt_size >= 40 && fmt + 26 <= data.size()) sample_tag = u16_at(data, fmt + 24);
	const uint16_t bytes_per = uint16_t(format.bits / 8);
	const bool pcm = sample_tag == kTagPcm && (format.bits == 8 || format.bits == 16 || format.bits == 24 || format.bits == 32);
	const bool real = sample_tag == kTagFloat && (format.bits == 32 || format.bits == 64);
	if ((!pcm && !real) || format.channels == 0 || format.rate == 0) {
		error = "its samples are " + wave_format_words(format) + ", which the editor does not read";
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

namespace {

// The bits a conversion writes.
int bits_written(const WaveSamples &source, const WaveConversion &conversion) {
	if (conversion.bits == "8") return 8;
	if (conversion.bits == "16") return 16;
	return source.format.tag == kTagPcm && source.format.bits == 8 && !source.format.aoa1 ? 8 : 16;
}

} // namespace

bool convert_wave(const std::vector<uint8_t> &source, const WaveConversion &conversion, std::vector<uint8_t> &out,
                  std::string &error) {
	WaveSamples wave;
	if (!decode_wave_source(source, wave, error)) return false;
	if (wave.frames() == 0) {
		error = "it holds no sample";
		return false;
	}
	// One channel: the mix, or the one taken.
	std::vector<float> mono(wave.frames());
	const uint16_t channels = wave.channels;
	for (size_t f = 0; f < mono.size(); ++f) {
		if (conversion.channels == "left") mono[f] = wave.samples[f * channels];
		else if (conversion.channels == "right") mono[f] = wave.samples[f * channels + (channels > 1 ? 1 : 0)];
		else {
			float sum = 0.0f;
			for (uint16_t c = 0; c < channels; ++c) sum += wave.samples[f * channels + c];
			mono[f] = sum / float(channels);
		}
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
	const int bits = bits_written(wave, conversion);
	std::vector<uint8_t> data;
	data.reserve(samples.size() * size_t(bits / 8));
	for (const float s : samples) {
		const float v = std::clamp(s, -1.0f, 1.0f);
		if (bits == 8) data.push_back(uint8_t(std::clamp(std::lround(v * 127.0f) + 128, 0L, 255L)));
		else io::append_u16_le(data, uint16_t(int16_t(std::clamp(std::lround(v * 32767.0f), -32768L, 32767L))));
	}
	// The plain RIFF the game's loader reads, through the format's writer.
	return lwf::wav_write_pcm_mono(data.data(), data.size(), rate, uint16_t(bits), out, error);
}

std::string wave_conversion_words(const WaveSamples &source, const WaveConversion &conversion) {
	std::vector<std::string> parts;
	if (source.channels > 1)
		parts.push_back(conversion.channels == "left"    ? "the left channel taken"
		                : conversion.channels == "right" ? "the right channel taken"
		                                                 : std::to_string(source.channels) + " channels mixed to mono");
	const int bits = bits_written(source, conversion);
	const bool same_samples = source.format.tag == kTagPcm && source.format.bits == bits;
	if (!same_samples) parts.push_back(wave_format_words(source.format).substr(0, wave_format_words(source.format).find(',')) +
	                                   " written as " + std::to_string(bits) + "-bit PCM");
	if (conversion.rate && conversion.rate != source.rate)
		parts.push_back(std::to_string(source.rate) + " Hz resampled to " + std::to_string(conversion.rate) + " Hz");
	std::string out;
	for (size_t i = 0; i < parts.size(); ++i) out += (i ? ", " : "") + parts[i];
	return out;
}

namespace {

bool rate_form(const std::string &value) {
	const std::optional<int> rate = strutil::parse_int(value);
	return rate && *rate >= 4000 && *rate <= 96000;
}

bool name_form(const std::string &value) {
	return strutil::ends_with_icase(value, ".wav") && value.find_first_of("/\\:") == std::string::npos &&
	       logical_name_fits_archive(value);
}

} // namespace

const std::vector<ImportOptionRow> &wave_import_option_rows() {
	static const std::vector<ImportOptionRow> rows = [] {
		std::vector<ImportOptionRow> out;
		ImportOptionRow channels;
		channels.key = "channels";
		channels.label = "Channels";
		channels.words = "The game plays a mono wave alone: the source's channels mixed to one, or one of them taken.";
		channels.values = {{"mono", "the channels mixed to one"},
		                   {"left", "the first channel alone"},
		                   {"right", "the second channel alone"}};
		channels.fallback = "mono";
		out.push_back(channels);
		ImportOptionRow bits;
		bits.key = "bits";
		bits.label = "Samples";
		bits.words = "The game plays 8-bit and 16-bit PCM: kept 8-bit where the source is 8-bit PCM, else 16-bit.";
		bits.values = {{"keep", "8-bit where the source is 8-bit PCM, else 16-bit"},
		               {"16", "16-bit PCM"},
		               {"8", "8-bit PCM (half the memory, more noise)"}};
		bits.fallback = "keep";
		out.push_back(bits);
		ImportOptionRow rate;
		rate.key = "rate";
		rate.label = "Rate";
		rate.words = "The samples per second: kept (the game plays any rate), or resampled.";
		rate.values = {{"keep", "the source's rate"},
		               {"11025", "11025 Hz"},
		               {"22050", "22050 Hz (most of the game's own waves)"},
		               {"44100", "44100 Hz (the game's mixing rate)"}};
		rate.forms = {"<4000..96000>"};
		rate.accepts = rate_form;
		rate.fallback = "keep";
		out.push_back(rate);
		ImportOptionRow name;
		name.key = "name";
		name.label = "Name";
		name.words = "The file the import writes, what the banks name: left out, the source's stem and .wav.";
		name.forms = {"<name>.wav"};
		name.accepts = name_form;
		name.keeps_case = true;
		out.push_back(name);
		return out;
	}();
	return rows;
}

WaveConversion wave_conversion_of(const ImportOptions &options) {
	WaveConversion out;
	const auto value = [&](const char *key) {
		const auto found = options.find(key);
		return found == options.end() ? std::string() : strutil::to_lower(found->second);
	};
	if (const std::string channels = value("channels"); !channels.empty()) out.channels = channels;
	if (const std::string bits = value("bits"); !bits.empty()) out.bits = bits;
	if (const std::string rate = value("rate"); !rate.empty() && rate != "keep")
		if (const std::optional<int> parsed = strutil::parse_int(rate)) out.rate = uint32_t(*parsed);
	return out;
}

bool run_wave_import(ImportContext &context, ImportProduct &out) {
	std::vector<uint8_t> bytes;
	std::string error;
	if (!convert_wave(context.source(), wave_conversion_of(context.options()), bytes, error)) {
		out.diagnostics.push_back(make_finding(CoreFinding::ImportWave, DiagnosticSeverity::Error,
		                                       context.source_name() + " is no wave the editor can read: " + error + ".",
		                                       context.source_name()));
		return false;
	}
	const auto name = context.options().find("name");
	const std::string stem = utf8_of(path_of(context.source_name()).stem());
	out.outputs.push_back({name != context.options().end() && !name->second.empty() ? name->second : stem + ".wav",
	                       std::move(bytes)});
	return true;
}

bool prepare_authored_wave(const std::string &name, std::vector<uint8_t> &bytes, std::string &note, std::string &error) {
	note.clear();
	const WaveRetailCheck check = wave_retail_check(bytes);
	if (check.plays) return true;
	WaveSamples source;
	if (!decode_wave_source(bytes, source, error)) {
		error = name + " is no wave the editor can read: " + error + ".";
		return false;
	}
	const WaveConversion conversion = wave_conversion_of(ImportOptions());
	std::vector<uint8_t> converted;
	if (!convert_wave(bytes, conversion, converted, error)) {
		error = name + " could not be converted: " + error + ".";
		return false;
	}
	std::string changed = wave_conversion_words(source, conversion);
	note = name + " came in as the game cannot play it (" + check.why + ") and was written as " +
	       wave_format_words({kTagPcm, 1, source.rate, uint16_t(bits_written(source, conversion)), 0, false}) +
	       (changed.empty() ? std::string(", its other chunks left out.") : ": " + changed + ".");
	bytes = std::move(converted);
	return true;
}

} // namespace opennova::editor
