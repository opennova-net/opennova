#include <editor/documents/texture_document.h>

#include <iterator>
#include <utility>

#include <formats/configfile/config_file.h>
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

// An indexed PCX whose rows run past its width, as the game's 32-bit PCX reader writes its last row: its bytes a
// line of 4-byte texels from that row's start in a buffer of the image's 4 * width * height bytes from the FastMem
// arena [orig: Texture_LoadPCXFromPFF32 @ 0x56EA30, the buffer @ 0x56EB1E, the row writes @ 0x56EDC0, @ 0x56EDDB],
// whose blocks are rounded up to 64 bytes [orig: FastMem_Alloc @ 0x7697B0, @ 0x7697D7] and followed by a header
// whose first dword is the pool pointer the next allocation and free dereference [orig: sub_7695C0 @ 0x7695D1;
// @ 0x7695C4, @ 0x769664, @ 0x769709]. The bytes the row writes past the block's rounding: where the allocator split
// the free block it took (one more than a header larger than asked, @ 0x769812..0x769819) the next block's header
// starts there, and a texel (0xFF its top byte) on its pointer faults the game's next FastMem call; a block it gave
// whole holds up to a header's 36 bytes more of its own, so only the bytes past those reach a header whatever the
// block (pcx_overrun_past_any_block). Within the rounding each row's extra texels land on the next row's start,
// which its own row then writes: the image reads right.
uint64_t pcx_overrun_past_block(const TextureHeader &header) {
	const uint64_t texels = uint64_t(header.width) * header.height * 4;
	const uint64_t slack = (64 - texels % 64) % 64;
	const uint64_t spill = (uint64_t(header.pcx_bytes_per_line) - header.width) * 4;
	return spill > slack ? spill - slack : 0;
}
uint64_t pcx_overrun_past_any_block(const TextureHeader &header) {
	const uint64_t past = pcx_overrun_past_block(header);
	return past > configfile::kFastMemHeaderBytes ? past - configfile::kFastMemHeaderBytes : 0;
}

constexpr FindingCodeEntry<F> kFindingEntries[] = {
	{F::Unloadable, code("texture.unloadable")},
	{F::TgaUnfilled, listed_code("texture.tga_unfilled")},
	{F::TgaZeroed, code("texture.tga_zeroed")},
	{F::TgaUpsideDown, code("texture.tga_upside_down", FindingFix::TextureRows)},
	{F::TgaColourMapSkipped, code("texture.tga_colour_map_skipped")},
	// An error of it alone gates: the last row written past any block the allocator gives (pcx_overrun_past_any_block);
	// past the rounding alone, or within it, a warning.
	{F::PcxOverrun,
	 game_fails_code("texture.pcx_overrun",
	                 "the game's PCX reader writes the image's last row past its buffer's block, over the next block's "
	                 "pool pointer, which the game's next allocation dereferences: the game faults loading it [orig: "
	                 "Texture_LoadPCXFromPFF32 @ 0x56EA30, @ 0x56EDC0; FastMem_Alloc @ 0x7697B0, @ 0x769664]")},
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

TextureDocument::TextureDocument(HistoryBudget budget) : RasterDocument(&pick_texture_codec, budget) {}

const std::vector<uint8_t> &TextureDocument::bytes() const {
	static const std::vector<uint8_t> none;
	if (!loaded_) return none;
	if (bytes_ && bytes_revision_ == revision() && bytes_generation_ == load_generation()) return *bytes_;
	bytes_revision_ = revision();
	bytes_generation_ = load_generation();
	// The texels and form it read (a stroke undone shares the tiles it read): the file as read.
	const RasterState &now = state();
	bool as_read = now.side == read_state_.side && now.planes.size() == read_state_.planes.size();
	for (size_t i = 0; as_read && i < now.planes.size(); ++i) as_read = now.planes[i].shares_tiles_with(read_state_.planes[i]);
	if (as_read) {
		bytes_ = read_bytes_;
		return *bytes_;
	}
	// A whole step's state (a texture operation's): the operation's bytes as it wrote them.
	for (const auto &[whole, written] : whole_bytes_) {
		bool same = written && now.side == whole.side && now.planes.size() == whole.planes.size();
		for (size_t i = 0; same && i < now.planes.size(); ++i) same = now.planes[i].shares_tiles_with(whole.planes[i]);
		if (same) {
			bytes_ = written;
			return *bytes_;
		}
	}
	auto written = std::make_shared<std::vector<uint8_t>>();
	std::string error;
	if (!write_texture_state(now, *written, error)) written->clear();
	bytes_ = std::move(written);
	return *bytes_;
}

const std::shared_ptr<const TextureImage> &TextureDocument::image() const {
	if (!loaded_) return image_;
	if (!image_ || image_revision_ != revision() || image_generation_ != load_generation()) {
		image_ = painted_image_();
		if (!image_) image_ = decode_texture(path(), bytes());
		image_revision_ = revision();
		image_generation_ = load_generation();
	}
	return image_;
}

std::shared_ptr<const TextureImage> TextureDocument::painted_image_() const {
	const RasterState &now = state();
	// A paint keeps the form it read (its side); a whole step (a texture operation) makes another, decoded from its bytes.
	if (!read_bytes_ || !texture_paintable(now) || now.side != read_state_.side) return nullptr;
	if (!read_image_ || read_image_generation_ != load_generation()) {
		read_image_ = decode_texture(path(), *read_bytes_);
		read_image_generation_ = load_generation();
	}
	if (!read_image_ || !read_image_->decoded || read_image_->levels.size() != 1) return nullptr;
	// The texels and form it read (a stroke undone shares the tiles it read): the image read.
	bool as_read = now.side == read_state_.side && now.planes.size() == read_state_.planes.size();
	for (size_t i = 0; as_read && i < now.planes.size(); ++i) as_read = now.planes[i].shares_tiles_with(read_state_.planes[i]);
	if (as_read) return read_image_;
	const RasterPlane &plane = now.planes.front();
	const int w = plane.width(), h = plane.height();
	if (uint32_t(w) != read_image_->width() || uint32_t(h) != read_image_->height()) return nullptr;
	const TextureSide &side = *texture_side(now);
	std::vector<uint8_t> rgba, indices, luminance;
	if (!texture_rgba_rect(now, 0, 0, w, h, rgba)) return nullptr;
	if (plane.format() == RasterFormat::U8) {
		indices.resize(size_t(w) * size_t(h));
		plane.copy_rect(0, 0, w, h, indices.data());
		// A palette PNG's transparency, as its reader expands it.
		if (side.form == TextureSide::Form::Png && side.png_colour == 3)
			for (size_t i = 0; i < indices.size(); ++i)
				rgba[i * 4 + 3] = indices[i] < side.png_alpha.size() ? side.png_alpha[indices[i]] : 255;
		// An 8-bit PCX's archive alpha: each palette entry's luminance, (85 * (r + g + b)) >> 8 in 16 bits
		// (formats/pcx decode_pcx_luminance_alpha), its rows of its width (a paintable form's).
		if (side.form == TextureSide::Form::Pcx8) {
			uint8_t lum[256];
			for (size_t e = 0; e < 256; ++e) {
				const uint16_t sum = uint16_t(side.palette[e * 3] + side.palette[e * 3 + 1] + side.palette[e * 3 + 2]);
				lum[e] = uint8_t(uint16_t(85u * sum) >> 8);
			}
			luminance.resize(indices.size());
			for (size_t i = 0; i < indices.size(); ++i) luminance[i] = lum[indices[i]];
		}
	}
	return retexel_texture(*read_image_, std::move(rgba), std::move(indices), std::move(luminance));
}

void TextureDocument::on_saved() {
	RasterDocument::on_saved();
	saved_state_ = state();
}

SerializeResult TextureDocument::serialize() const {
	SerializeResult out;
	out.text.assign(bytes().begin(), bytes().end());
	return out;
}

std::unique_ptr<DocumentBase> TextureDocument::snapshot() const {
	return std::make_unique<TextureDocument>(*this);
}

bool TextureDocument::apply_edits(const std::vector<Edit> &edits, Diagnostic &error) {
	// A whole image (a texture operation's bytes) is read by the texture's reader into a whole change; texels
	// painted go to the raster as they are.
	std::vector<Edit> raster;
	raster.reserve(edits.size());
	std::vector<std::shared_ptr<TextureWholeEdit>> wholes;
	for (const Edit &edit : edits) {
		const auto *image = edit.operation == EditOperation::Apply ? dynamic_cast<const TextureImageEdit *>(edit.payload.get()) : nullptr;
		if (!image) {
			raster.push_back(edit);
			continue;
		}
		auto whole = std::make_shared<TextureWholeEdit>();
		whole->state = read_texture_state(path(), image->bytes);
		whole->bytes = std::make_shared<const std::vector<uint8_t>>(image->bytes);
		wholes.push_back(whole);
		Edit made = edit;
		made.payload = std::move(whole);
		raster.push_back(std::move(made));
	}
	if (!RasterDocument::apply_edits(raster, error)) return false;
	// The states the whole steps made, each with the bytes it was read from (bytes(): a save writes them as they are).
	constexpr size_t kWholeKept = 16;
	for (const auto &whole : wholes) {
		whole_bytes_.emplace_back(whole->state, whole->bytes);
		if (whole_bytes_.size() > kWholeKept) whole_bytes_.erase(whole_bytes_.begin());
	}
	return true;
}

bool TextureDocument::read_source(const std::vector<uint8_t> &decoded, bool adopt, std::vector<SourceIssue> &issues,
                                  Diagnostic &error) {
	if (!RasterDocument::read_source(decoded, adopt, issues, error)) return false;
	if (!adopt) return true;
	read_bytes_ = std::make_shared<const std::vector<uint8_t>>(decoded);
	read_state_ = state();
	saved_state_ = state();
	whole_bytes_.clear();
	loaded_ = true;
	bytes_.reset();
	image_.reset();
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
			// The reader allocates the texels, has no case for the form, and returns the texture with its texels
			// unset; the game goes on [orig: CTerrainTileData_LoadTGAFromArchive @ 0x56E570, the allocation
			// @ 0x56E694, the switch @ 0x56E6D2 whose default @ 0x56E984 goes to the row flip @ 0x56E995]: it skips
			// the image's contents, no file refused, so the finding refuses no build.
			add(F::TgaUnfilled, DiagnosticSeverity::Warning,
			    "The game's TGA reader has no case for " + form +
			            ": it loads the texture with its texels left as memory held them and goes on, drawing whatever "
			            "that memory was. Save it as a 24- or 32-bit true-colour TGA.");
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
		// Listed (S26 A4): the readers read on past the file's bytes, no fault witnessed [orig:
		// CTerrainTileData_LoadTGAFromArchive @ 0x56E7A5; CTextureData_LoadTGA @ 0x5F7D9D; CUIImage_LoadTGA @
		// 0x664A8D].
		if (header.tga_short)
			add(F::TgaTruncated, DiagnosticSeverity::Error,
			    std::string("It ends before its texels do: the game's TGA reader ") +
			            (type == 10 ? "decodes its run-length packets" : "copies its " + std::to_string(header.width) + " x " +
			                                                               std::to_string(header.height) + " texels") +
			            " on past the end of the file, out of whatever memory follows it. Save it again whole.");
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
			const uint64_t past = pcx_overrun_past_block(header);
			const uint64_t past_any = pcx_overrun_past_any_block(header);
			add(F::PcxOverrun, past_any ? DiagnosticSeverity::Error : DiagnosticSeverity::Warning,
			    "Its rows hold " + std::to_string(header.pcx_bytes_per_line) + " bytes for " + std::to_string(header.width) +
			            " texels: the game's PCX reader writes each row's " + std::to_string(extra) +
			            (extra == 1 ? " extra texel" : " extra texels") + " into the next row, which that row writes again, and the last row's " +
			            (past_any ? "past its buffer's block by " + std::to_string(past) +
			                                " bytes, past any block the allocator gives it (one it does not split holds 36 "
			                                "bytes more), over the next block's header: the game faults loading it."
			             : past ? std::to_string(past) + " bytes past the spare bytes its buffer's block rounds up to: over "
			                                                "the next block's header where the allocator split the block it "
			                                                "gives (the game then faults), into the block's own spare bytes "
			                                                "where it gives one whole (36 bytes more at most)."
			                    : std::string("into the spare bytes its buffer's block rounds up to.")) +
			            " Save it with rows of exactly its width (its bytes a line " + std::to_string(header.width) + ").");
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
