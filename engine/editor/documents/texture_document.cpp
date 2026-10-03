#include <editor/documents/texture_document.h>

#include <utility>

namespace opennova::editor {

namespace {

using io::JsonValue;
using io::json_number;
using io::json_string;

} // namespace

bool TextureDocument::changes_since(uint64_t load_generation, uint64_t revision, ChangeSet &out) const {
	// One state per load: the one a caller read is this one exactly when it read this load.
	if (load_generation != this->load_generation() || revision != 0) return false;
	out = RasterChanges();
	return true;
}

SerializeResult TextureDocument::serialize() const {
	// No edit is taken: what a Save would write is what the document read.
	SerializeResult out;
	out.text.assign(bytes_.begin(), bytes_.end());
	return out;
}

std::unique_ptr<DocumentBase> TextureDocument::snapshot() const {
	return std::make_unique<TextureDocument>(*this);
}

bool TextureDocument::apply_edits(const std::vector<Edit> &edits, Diagnostic &error) {
	(void)edits;
	error = make_finding(CoreFinding::DocumentPayload, DiagnosticSeverity::Error,
	                     "A texture is read only in the editor for now: it takes no edit.", path());
	return false;
}

bool TextureDocument::read_source(const std::vector<uint8_t> &decoded, bool adopt, std::vector<SourceIssue> &issues,
                                  Diagnostic &error) {
	(void)issues;
	(void)error;
	// Every file reads: what the game cannot load is said by its image (why it does not load), never a
	// document that does not open.
	if (!adopt) return true;
	bytes_ = decoded;
	image_ = decode_texture(path(), bytes_);
	return true;
}

std::unique_ptr<DocumentBase> make_texture_document() {
	return std::make_unique<TextureDocument>();
}

std::vector<Diagnostic> validate_texture_file(const DocumentBase &document) {
	(void)document;
	return {};
}

const std::vector<FieldSchema> &texture_fields(NodeKind kind) {
	(void)kind;
	static const std::vector<FieldSchema> none;
	return none;
}

FindingTable texture_finding_codes() {
	return {};
}

JsonValue texture_image_json(const TextureImage &image) {
	JsonValue out = JsonValue::make_object();
	out.set("reader", json_string(texture_reader_token(image.reader)));
	out.set("loads", JsonValue::make_bool(image.loads));
	out.set("refusal", json_string(image.refusal));
	out.set("decoded", JsonValue::make_bool(image.decoded));
	out.set("undecoded", json_string(image.undecoded));
	out.set("blank", JsonValue::make_bool(image.blank));
	out.set("width", json_number(double(image.width())));
	out.set("height", json_number(double(image.height())));
	JsonValue levels = JsonValue::make_array();
	for (const TextureLevel &level : image.levels) {
		JsonValue entry = JsonValue::make_object();
		entry.set("width", json_number(double(level.width)));
		entry.set("height", json_number(double(level.height)));
		levels.push(std::move(entry));
	}
	out.set("levels", std::move(levels));
	out.set("game_levels", json_number(double(image.game_levels)));
	out.set("palette_size", json_number(double(image.palette_size())));
	out.set("alpha", json_string(texture_alpha_token(image.alpha)));
	out.set("upside_down", JsonValue::make_bool(image.upside_down));
	JsonValue facts = JsonValue::make_array();
	for (const TextureFact &fact : image.facts) {
		JsonValue entry = JsonValue::make_object();
		entry.set("key", json_string(fact.key));
		entry.set("label", json_string(fact.label));
		entry.set("words", json_string(fact.words));
		facts.push(std::move(entry));
	}
	out.set("facts", std::move(facts));
	return out;
}

JsonValue texture_content_json(const DocumentBase &document) {
	const auto *texture = dynamic_cast<const TextureDocument *>(&document);
	if (!texture || !texture->image()) return JsonValue::make_null();
	return texture_image_json(*texture->image());
}

} // namespace opennova::editor
