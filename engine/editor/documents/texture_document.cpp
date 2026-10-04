#include <editor/documents/texture_document.h>

#include <iterator>
#include <utility>

namespace opennova::editor {

namespace {

using io::JsonValue;
using io::json_number;
using io::json_string;
using F = TextureFinding;

constexpr FindingCodeRow code(const char *token, FindingFix fixes = FindingFix::None) {
	FindingCodeRow row;
	row.token = token;
	row.fixes = fixes;
	return row;
}

constexpr FindingCodeEntry<F> kFindingEntries[] = {
	{F::Unloadable, code("texture.unloadable")},
	{F::TgaUnfilled, code("texture.tga_unfilled")},
	{F::TgaZeroed, code("texture.tga_zeroed")},
	{F::TgaUpsideDown, code("texture.tga_upside_down", FindingFix::TextureRows)},
	{F::TgaColourMapSkipped, code("texture.tga_colour_map_skipped")},
	{F::PcxOverrun, code("texture.pcx_overrun")},
	{F::NotRead, code("texture.not_read")},
};
static_assert(std::size(kFindingEntries) == size_t(F::kCount), "a row per texture finding");
static_assert(finding_entries_well_formed(kFindingEntries), "the texture findings in their enum's order, a token each");
constexpr auto kFindingRows = finding_rows(kFindingEntries, FindingGroup::Textures);
static_assert(finding_rows_well_formed(kFindingRows), "every row of the table takes its group");

} // namespace

const FindingCodeRow &finding_code(TextureFinding code) {
	return kFindingRows[static_cast<size_t>(code)];
}

const std::vector<uint8_t> &TextureDocument::bytes() const {
	static const std::vector<uint8_t> none;
	return versions_.empty() ? none : *versions_[cursor_].bytes;
}

const std::shared_ptr<const TextureImage> &TextureDocument::image() const {
	if (loaded_ && !image_) image_ = decode_texture(path(), bytes());
	return image_;
}

bool TextureDocument::dirty() const { return revision() != saved_revision_; }

uint64_t TextureDocument::revision() const { return versions_.empty() ? 0 : versions_[cursor_].revision; }

size_t TextureDocument::history_bytes() const {
	size_t total = 0;
	for (const Version &version : versions_) total += version.bytes->size();
	return total;
}

bool TextureDocument::changes_since(uint64_t load_generation, uint64_t revision, ChangeSet &out) const {
	// The state a caller read is this one, or another whole image (its sides perhaps others): the caller
	// takes everything as changed.
	if (load_generation != this->load_generation() || revision != this->revision()) return false;
	out = RasterChanges();
	return true;
}

SerializeResult TextureDocument::serialize() const {
	SerializeResult out;
	out.text.assign(bytes().begin(), bytes().end());
	return out;
}

std::unique_ptr<DocumentBase> TextureDocument::snapshot() const {
	return std::make_unique<TextureDocument>(*this);
}

void TextureDocument::moved() { image_.reset(); }

bool TextureDocument::apply_edits(const std::vector<Edit> &edits, Diagnostic &error) {
	// One batch, one step: its last image (each a whole file).
	const TextureImageEdit *image = nullptr;
	for (const Edit &edit : edits) {
		const auto *made = edit.operation == EditOperation::Apply ? dynamic_cast<const TextureImageEdit *>(edit.payload.get()) : nullptr;
		if (!made) {
			error = make_finding(CoreFinding::DocumentPayload, DiagnosticSeverity::Error,
			                     "A texture takes a whole image (a texture operation), no other edit.", path());
			return false;
		}
		image = made;
	}
	if (!image) return true;
	// The versions after the current go (a redo branch), the new one is the current, and past the budget the
	// oldest go, never the one before the current.
	versions_.resize(cursor_ + 1);
	versions_.push_back({std::make_shared<const std::vector<uint8_t>>(image->bytes), next_revision_++});
	cursor_ = versions_.size() - 1;
	while (versions_.size() > budget_.min_steps + 1 && history_bytes() > budget_.bytes) {
		versions_.erase(versions_.begin());
		--cursor_;
	}
	moved();
	return true;
}

void TextureDocument::undo_step() {
	if (cursor_ == 0) return;
	--cursor_;
	moved();
}

void TextureDocument::redo_step() {
	if (cursor_ + 1 >= versions_.size()) return;
	++cursor_;
	moved();
}

void TextureDocument::on_saved() { saved_revision_ = revision(); }

bool TextureDocument::read_source(const std::vector<uint8_t> &decoded, bool adopt, std::vector<SourceIssue> &issues,
                                  Diagnostic &error) {
	(void)issues;
	(void)error;
	// Every file reads: what the game cannot load is said by its image (why it does not load), never a
	// document that does not open.
	if (!adopt) return true;
	versions_.assign(1, Version{std::make_shared<const std::vector<uint8_t>>(decoded), 0});
	cursor_ = 0;
	next_revision_ = 1;
	saved_revision_ = 0;
	loaded_ = true;
	moved();
	return true;
}

std::unique_ptr<DocumentBase> make_texture_document() {
	return std::make_unique<TextureDocument>();
}

std::vector<Diagnostic> validate_texture_file(const DocumentBase &document) {
	std::vector<Diagnostic> out;
	const auto *texture = dynamic_cast<const TextureDocument *>(&document);
	if (!texture) return out;
	const auto add = [&](TextureFinding code, DiagnosticSeverity severity, const std::string &message) {
		out.push_back(make_finding(finding_code(code), severity, message, document.path()));
	};
	const TextureHeader header = texture_header(document.path(), texture->bytes());
	if (!header.read) {
		add(F::Unloadable, DiagnosticSeverity::Warning, "The game cannot load it: " + header.refusal);
		return out;
	}
	switch (header.reader) {
	case TextureReader::Tga: {
		// What the game's TGA reader makes of each image type [orig: CTerrainTileData_LoadTGAFromArchive @
		// 0x56E570, the switch @ 0x56E6C2], whose decode the menus' reader repeats [orig: CUIImage_LoadTGA @
		// 0x6647D0].
		const uint8_t type = header.tga_type, bits = header.tga_bits;
		const std::string form = "image type " + std::to_string(type) + " at " + std::to_string(bits) + " bits";
		const bool known = type == 1 || type == 2 || type == 3 || type == 9 || type == 10 || type == 11;
		if (!known || (type == 3 && bits != 8)) {
			add(F::TgaUnfilled, DiagnosticSeverity::Error,
			    "The game's TGA reader has no case for " + form +
			            ": it leaves the texels as the buffer held them, so the game draws whatever memory held. Save "
			            "it as a 24- or 32-bit true-colour TGA.");
		} else if (type == 9 || type == 11 || ((type == 2 || type == 10) && bits != 24 && bits != 32) ||
		           (type == 1 && header.tga_map_entry_bits != 24)) {
			add(F::TgaZeroed, DiagnosticSeverity::Warning,
			    "The game's TGA reader zeroes " +
			            (type == 1 ? "a colour-mapped TGA whose map is not of 24-bit entries" : form) +
			            ": the game draws it transparent black. Save it as a 24- or 32-bit true-colour TGA.");
		}
		// Rows always taken bottom up, the descriptor's origin bit unread [orig: @ 0x56E995..0x56E9EA; the
		// menus' @ 0x66499D; the particles' @ 0x5F7FE8].
		if (header.tga_descriptor & 0x20)
			add(F::TgaUpsideDown, DiagnosticSeverity::Warning,
			    "Its rows are stored top first (its header's origin bit), but the game's TGA reader takes every file "
			    "bottom up: the game draws it upside down. Save it with the bottom row first.");
		// The texels read from byte 18 plus the ID's length whatever the colour map holds [orig: @ 0x56E6BA].
		if ((type == 2 || type == 3 || type == 10) && header.tga_map_type != 0 && header.tga_map_length > 0)
			add(F::TgaColourMapSkipped, DiagnosticSeverity::Warning,
			    "It carries a colour map of " + std::to_string(header.tga_map_length) +
			            " entries, which the game's TGA reader does not skip: it reads the texels from the map's start, "
			            "shifted. Save it without a colour map.");
		break;
	}
	case TextureReader::Pcx:
		// The game's PCX reader takes 8 bits a plane alone (any other depth fails the load), reads three
		// planes as 24-bit colour and any other count as 8-bit indices, and writes each decoded row of an
		// indexed image's bytes-a-line into a buffer of the image's width [orig: Texture_LoadPCXFromPFF32 @
		// 0x56EA30, the depth test @ 0x56EABC, the 24-bit path @ 0x56EB31, the rows @ 0x56ED70..0x56EDFC].
		if (header.pcx_bits != 8) {
			add(F::Unloadable, DiagnosticSeverity::Warning,
			    "The game cannot load it: its PCX reader takes 8 bits a plane alone (this one is " +
			            std::to_string(header.pcx_bits) + " bits a plane).");
		} else if (header.pcx_planes != 3 && header.pcx_bytes_per_line != header.width) {
			add(F::PcxOverrun, DiagnosticSeverity::Error,
			    "Its rows hold " + std::to_string(header.pcx_bytes_per_line) + " bytes for " + std::to_string(header.width) +
			            " texels (an odd width): the game's PCX reader writes each row's extra byte into the next row and "
			            "the last past its buffer. Make its width even.");
		}
		break;
	case TextureReader::Dds:
	case TextureReader::Png:
	case TextureReader::None: break;
	}
	return out;
}

const std::vector<FieldSchema> &texture_fields(NodeKind kind) {
	(void)kind;
	static const std::vector<FieldSchema> none;
	return none;
}

FindingTable texture_finding_codes() {
	return {kFindingRows.data(), kFindingRows.size()};
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
