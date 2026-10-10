// The wave document (wave_document.h): a .wav held as its bytes, read as the game's loader reads it, its whole-wave
// operations written through the engine's converter, its one finding.
#include <editor/documents/wave_document.h>

#include <cmath>
#include <cstdio>
#include <optional>

#include <base/io/strutil.h>
#include <editor/project/project_files.h>

namespace opennova::editor {

using io::JsonValue;
using io::json_number;
using io::json_string;

namespace {

constexpr const char *kOperationTokens[] = {"trim", "normalise"};
static_assert(std::size(kOperationTokens) == size_t(WaveOperationKind::kCount), "a token per wave operation");

std::string seconds_words(double seconds) {
	char text[32];
	std::snprintf(text, sizeof(text), "%.3f s", seconds);
	return text;
}

bool real_of(const std::string &text, double &out) {
	const std::optional<float> parsed = strutil::parse_float(strutil::trim(text));
	if (!parsed || !std::isfinite(*parsed)) return false;
	out = double(*parsed);
	return true;
}

} // namespace

const char *wave_operation_token(WaveOperationKind kind) {
	const size_t index = size_t(kind);
	return index < std::size(kOperationTokens) ? kOperationTokens[index] : "";
}

bool wave_operation_kind(const std::string &token, WaveOperationKind &out) {
	for (size_t i = 0; i < std::size(kOperationTokens); ++i)
		if (strutil::iequals(token, kOperationTokens[i]) || (i == 1 && strutil::iequals(token, "normalize"))) {
			out = WaveOperationKind(i);
			return true;
		}
	return false;
}

bool apply_wave_operation(const std::vector<uint8_t> &bytes, const WaveOperation &operation, std::vector<uint8_t> &out,
                          std::string &words, std::string &why) {
	lwf::WaveSamples source;
	if (!lwf::decode_wave_source(bytes, source, why)) return false;
	if (source.frames() == 0 || source.rate == 0) {
		why = "it holds no sample";
		return false;
	}
	// The form the game takes: one channel (the mix), the source's 8 bits kept and anything else 16, its rate kept.
	lwf::WaveConversion conversion;
	const double seconds = double(source.frames()) / double(source.rate);
	if (operation.kind == WaveOperationKind::Trim) {
		double start = 0.0, end = seconds;
		for (const auto &[key, value] : operation.params) {
			double number = 0.0;
			if ((key != "start" && key != "end") || !real_of(value, number) || number < 0.0) {
				why = "a trim takes start and end, each seconds from 0 ('" + key + "' is " +
				      (key == "start" || key == "end" ? "no such number)" : "none of them)");
				return false;
			}
			(key == "start" ? start : end) = number;
		}
		conversion.start = uint64_t(std::llround(start * double(source.rate)));
		conversion.end = uint64_t(std::llround(std::min(end, seconds) * double(source.rate)));
		if (conversion.end <= conversion.start) {
			why = "the trim keeps no sample: its end (" + seconds_words(end) + ") is not past its start (" + seconds_words(start) + ")";
			return false;
		}
		if (!lwf::convert_wave(bytes, conversion, out, why)) return false;
		words = "Trimmed to " + seconds_words(start) + " .. " + seconds_words(std::min(end, seconds)) + " of " + seconds_words(seconds);
		return true;
	}
	double peak = 1.0;
	for (const auto &[key, value] : operation.params)
		if (key != "peak" || !real_of(value, peak) || peak <= 0.0 || peak > 1.0) {
			why = "a normalise takes peak, the loudest sample's level, more than 0 and at most 1";
			return false;
		}
	const float loudest = lwf::wave_conversion_peak(bytes, conversion);
	if (loudest <= 0.0f) {
		why = "it is silent: no sample to scale";
		return false;
	}
	conversion.gain = float(peak / double(loudest));
	if (!lwf::convert_wave(bytes, conversion, out, why)) return false;
	char gain[48];
	std::snprintf(gain, sizeof(gain), "%.2f dB", 20.0 * std::log10(double(conversion.gain)));
	words = "Normalised to a peak of " + std::to_string(int(std::lround(peak * 100.0))) + "% (" + gain + ")";
	return true;
}

// --- the document ----------------------------------------------------------------------------------

const std::vector<uint8_t> &WaveDocument::bytes() const {
	static const std::vector<uint8_t> none;
	return versions_.empty() ? none : *versions_[cursor_].bytes;
}

const lwf::WaveFacts &WaveDocument::facts() const {
	if (!facts_) facts_ = std::make_shared<const lwf::WaveFacts>(lwf::wave_facts(bytes(), kWavePictureBins));
	return *facts_;
}

bool WaveDocument::dirty() const { return revision() != saved_revision_; }

uint64_t WaveDocument::revision() const { return versions_.empty() ? 0 : versions_[cursor_].revision; }

size_t WaveDocument::history_bytes() const {
	size_t total = 0;
	for (const Version &version : versions_) total += version.bytes->size();
	return total;
}

bool WaveDocument::changes_since(uint64_t load_generation, uint64_t revision, ChangeSet &out) const {
	if (load_generation != this->load_generation() || revision != this->revision()) return false;
	out = RasterChanges();
	return true;
}

SerializeResult WaveDocument::serialize() const {
	SerializeResult out;
	out.text.assign(bytes().begin(), bytes().end());
	return out;
}

std::unique_ptr<DocumentBase> WaveDocument::snapshot() const { return std::make_unique<WaveDocument>(*this); }

bool WaveDocument::apply_edits(const std::vector<Edit> &edits, Diagnostic &error) {
	// One batch, one step: its last wave (each a whole file).
	const WaveBytesEdit *wave = nullptr;
	for (const Edit &edit : edits) {
		const auto *made = edit.operation == EditOperation::Apply ? dynamic_cast<const WaveBytesEdit *>(edit.payload.get()) : nullptr;
		if (!made) {
			error = make_finding(CoreFinding::DocumentPayload, DiagnosticSeverity::Error,
			                     "A wave takes a whole wave (a wave operation), no other edit.", path());
			return false;
		}
		wave = made;
	}
	if (!wave) return true;
	versions_.resize(cursor_ + 1);
	versions_.push_back({std::make_shared<const std::vector<uint8_t>>(wave->bytes), next_revision_++});
	cursor_ = versions_.size() - 1;
	while (versions_.size() > budget_.min_steps + 1 && history_bytes() > budget_.bytes) {
		versions_.erase(versions_.begin());
		--cursor_;
	}
	moved();
	return true;
}

void WaveDocument::undo_step() {
	if (cursor_ == 0) return;
	--cursor_;
	moved();
}

void WaveDocument::redo_step() {
	if (cursor_ + 1 >= versions_.size()) return;
	++cursor_;
	moved();
}

void WaveDocument::on_saved() { saved_revision_ = revision(); }

bool WaveDocument::read_source(const std::vector<uint8_t> &decoded, bool adopt, std::vector<SourceIssue> &, Diagnostic &) {
	// Every file reads: what the game cannot play is said by its finding, never a document that does not open.
	if (!adopt) return true;
	versions_.assign(1, Version{std::make_shared<const std::vector<uint8_t>>(decoded), 0});
	cursor_ = 0;
	next_revision_ = 1;
	saved_revision_ = 0;
	moved();
	return true;
}

std::unique_ptr<DocumentBase> make_wave_document() { return std::make_unique<WaveDocument>(); }

// The one place a wave the game's loader refuses is found: its own walk [orig: Audio_LoadWavFileFromArchive @
// 0x766480] (lwf::wave_retail_check), a warning (the game plays nothing for it and goes on, so it never blocks a
// build).
std::vector<Diagnostic> validate_wave_file(const DocumentBase &document) {
	std::vector<Diagnostic> out;
	const auto *wave = dynamic_cast<const WaveDocument *>(&document);
	if (!wave) return out;
	const lwf::WaveRetailCheck check = lwf::wave_retail_check(wave->bytes());
	if (!check.plays)
		out.push_back(make_finding(CoreFinding::AssetWaveUnplayable, DiagnosticSeverity::Warning,
		                           "The game cannot play " + basename_of(document.path()) + ": " + check.why +
		                                   " Import it again from its source, or trim or normalise it here: either writes it "
		                                   "as the game plays it.",
		                           document.path()));
	return out;
}

const std::vector<FieldSchema> &wave_fields(NodeKind) {
	static const std::vector<FieldSchema> none;
	return none;
}

FindingTable wave_finding_codes() { return {nullptr, 0}; }

JsonValue wave_content_json(const DocumentBase &document) {
	const auto *wave = dynamic_cast<const WaveDocument *>(&document);
	if (!wave) return JsonValue::make_null();
	const lwf::WaveFacts &facts = wave->facts();
	JsonValue out = JsonValue::make_object();
	out.set("read", JsonValue::make_bool(facts.read));
	if (!facts.read) {
		out.set("error", json_string(facts.error));
		return out;
	}
	out.set("format", json_string(lwf::wave_format_words(facts.format)));
	out.set("channels", json_number(double(facts.format.channels)));
	out.set("bits", json_number(double(facts.format.bits)));
	out.set("rate", json_number(double(facts.format.rate)));
	out.set("frames", json_number(double(facts.frames)));
	out.set("seconds", json_number(facts.seconds));
	out.set("peak", json_number(double(facts.peak)));
	out.set("rms", json_number(double(facts.rms)));
	out.set("plays", JsonValue::make_bool(facts.retail.plays));
	out.set("why", json_string(facts.retail.why));
	JsonValue picture = JsonValue::make_array();
	for (const float v : facts.envelope) picture.array.push_back(json_number(double(v)));
	out.set("picture", std::move(picture));
	return out;
}

} // namespace opennova::editor
