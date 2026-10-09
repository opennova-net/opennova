#include <editor/import/wave_source.h>

#include <base/io/strutil.h>
#include <editor/assets/asset_registry.h>
#include <editor/project/project_files.h>

namespace opennova::editor {

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

lwf::WaveConversion wave_conversion_of(const ImportOptions &options) {
	lwf::WaveConversion out;
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
	if (!lwf::convert_wave(context.source(), wave_conversion_of(context.options()), bytes, error)) {
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
	const lwf::WaveRetailCheck check = lwf::wave_retail_check(bytes);
	if (check.plays) return true;
	lwf::WaveSamples source;
	if (!lwf::decode_wave_source(bytes, source, error)) {
		error = name + " is no wave the editor can read: " + error + ".";
		return false;
	}
	const lwf::WaveConversion conversion = wave_conversion_of(ImportOptions());
	std::vector<uint8_t> converted;
	if (!lwf::convert_wave(bytes, conversion, converted, error)) {
		error = name + " could not be converted: " + error + ".";
		return false;
	}
	std::string changed = lwf::wave_conversion_words(source, conversion);
	note = name + " came in as the game cannot play it (" + check.why + ") and was written as " +
	       lwf::wave_format_words({lwf::kWaveTagPcm, 1, source.rate, uint16_t(lwf::wave_bits_written(source, conversion)), 0, false}) +
	       (changed.empty() ? std::string(", its other chunks left out.") : ": " + changed + ".");
	bytes = std::move(converted);
	return true;
}

} // namespace opennova::editor
