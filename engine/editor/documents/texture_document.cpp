#include <editor/documents/texture_document.h>

#include <iterator>
#include <utility>

#include <formats/pcx/pcx_io.h>
#include <formats/tga/tga_read.h>
#include <runtime/renderer/device_texture.h>

namespace opennova::editor {

namespace {

using io::JsonValue;
using io::json_number;
using io::json_string;
using F = TextureFinding;

// The TGA header fields a file's header holds, as the reader's rules read them.
tga::TgaHeader tga_fields(const TextureHeader &header) {
	tga::TgaHeader out;
	out.image_type = header.tga_type;
	out.bits = header.tga_bits;
	out.descriptor = header.tga_descriptor;
	out.colour_map_type = header.tga_map_type;
	out.map_length = header.tga_map_length;
	out.map_entry_bits = header.tga_map_entry_bits;
	return out;
}

// The PCX header fields a file's header holds.
PcxHeader pcx_fields(const TextureHeader &header) {
	PcxHeader out;
	out.bits = header.pcx_bits;
	out.width = int(header.width);
	out.height = int(header.height);
	out.planes = header.pcx_planes;
	out.bytes_per_line = header.pcx_bytes_per_line;
	return out;
}

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
	{F::NotRead, code("texture.not_read", FindingFix::SetAsideUnread)},
	{F::TgaTruncated, code("texture.tga_truncated")},
	{F::PcxShortRows, code("texture.pcx_short_rows")},
	{F::DdsNotPowerOfTwo, code("texture.dds_not_pow2")},
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
		// What the game's TGA reader makes of each image type (tga::tga_retail_form, its witnesses).
		const uint8_t type = header.tga_type, bits = header.tga_bits;
		const std::string form = "image type " + std::to_string(type) + " at " + std::to_string(bits) + " bits";
		const tga::TgaRetailForm read = tga::tga_retail_form(tga_fields(header));
		if (read == tga::TgaRetailForm::Unset) {
			add(F::TgaUnfilled, DiagnosticSeverity::Error,
			    "The game's TGA reader has no case for " + form +
			            ": it leaves the texels as the buffer held them, so the game draws whatever memory held. Save "
			            "it as a 24- or 32-bit true-colour TGA.");
		} else if (read == tga::TgaRetailForm::Zeroed) {
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
		// The texels read from byte 18 plus the ID's length whatever the colour map holds
		// (tga::tga_colour_map_misread).
		if (tga::tga_colour_map_misread(tga_fields(header)))
			add(F::TgaColourMapSkipped, DiagnosticSeverity::Warning,
			    "It carries a colour map of " + std::to_string(header.tga_map_length) +
			            " entries, which the game's TGA reader does not skip: it reads the texels from the map's start, "
			            "shifted. Save it without a colour map.");
		// The reader copies every texel its header names with no bound on the file (tga::tga_reads_past_end).
		if (header.tga_short)
			add(F::TgaTruncated, DiagnosticSeverity::Error,
			    std::string("It ends before its texels do: the game's TGA reader ") +
			            (type == 10 ? "decodes its run-length packets" : "copies its " + std::to_string(header.width) + " x " +
			                                                               std::to_string(header.height) + " texels") +
			            " past the end of the file, into whatever memory follows it. Save it again whole.");
		break;
	}
	case TextureReader::Pcx:
		// The game's PCX reader takes 8 bits a plane alone (any other depth fails the load), reads three
		// planes as 24-bit colour and any other count as 8-bit indices, and writes each decoded row of an
		// indexed image's bytes-a-line into a buffer of the image's width (pcx_row_fit, its witnesses).
		if (header.pcx_bits != 8) {
			add(F::Unloadable, DiagnosticSeverity::Warning,
			    "The game cannot load it: its PCX reader takes 8 bits a plane alone (this one is " +
			            std::to_string(header.pcx_bits) + " bits a plane).");
		} else if (pcx_row_fit(pcx_fields(header)) == PcxRowFit::Overrun) {
			const uint32_t extra = header.pcx_bytes_per_line - header.width;
			add(F::PcxOverrun, DiagnosticSeverity::Error,
			    "Its rows hold " + std::to_string(header.pcx_bytes_per_line) + " bytes for " + std::to_string(header.width) +
			            " texels: the game's PCX reader writes each row's " + std::to_string(extra) +
			            (extra == 1 ? " extra byte" : " extra bytes") +
			            " into the next row and the last row's past its buffer. Save it with rows of exactly its width (its "
			            "bytes a line " + std::to_string(header.width) + ").");
		} else if (pcx_row_fit(pcx_fields(header)) == PcxRowFit::Short) {
			add(F::PcxShortRows, DiagnosticSeverity::Warning,
			    "Its rows hold " + std::to_string(header.pcx_bytes_per_line) + " bytes for " + std::to_string(header.width) +
			            " texels: the game's PCX reader writes " + std::to_string(header.pcx_bytes_per_line) +
			            " texels a row and leaves the last " + std::to_string(header.width - header.pcx_bytes_per_line) +
			            " of each as its buffer held them. Save it with rows of exactly its width.");
		}
		break;
	case TextureReader::Dds: {
		// The DDS reader's texture is made at D3DX_DEFAULT sides, each the image's rounded up to a power of
		// two, the image in its top-left corner, transparent black past it (renderer::d3dx_default_texture_side,
		// its witnesses; render-material-re D-RMAT-18).
		const uint32_t width = renderer::d3dx_default_texture_side(header.width);
		const uint32_t height = renderer::d3dx_default_texture_side(header.height);
		if (width != header.width || height != header.height)
			add(F::DdsNotPowerOfTwo, DiagnosticSeverity::Warning,
			    "Its sides, " + std::to_string(header.width) + " x " + std::to_string(header.height) +
			            ", are not powers of two: the game's DDS reader makes a texture of " + std::to_string(width) + " x " +
			            std::to_string(height) +
			            ", the image in its top-left corner and transparent black past it, which the texture's coordinates "
			            "reach. Save it with sides that are powers of two.");
		break;
	}
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
