// The font document (font_document.h): the font's table over its header, glyph table and pages, its parse through
// the engine's reader and its save through the engine's writer from the row alone, and its findings. What the game
// does with each value is docs/fonts/fnt-re.md's.
#include "font_document.h"

#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <base/io/cp1252.h>
#include <editor/assets/asset_kinds.h>
#include <editor/documents/source_issue_findings.h>
#include <editor/model/staged_rows.h>

namespace opennova::editor {

using namespace opennova::fnt;

namespace {

constexpr NodeKind kFont = node_kind(FontKind::Font);
constexpr NodeKind kGlyph = node_kind(FontKind::Glyph);

FontRow &font_of(const RecordHandle &r) { return r.as<FontRow>(); }
FontGlyph &glyph_of(const RecordHandle &r) { return r.as<FontGlyph>(); }

FieldSchema schema_of(const char *id, FieldType type, const char *label, const char *description) {
	FieldSchema schema;
	schema.id = id;
	schema.type = type;
	schema.label = label;
	schema.description = description;
	return schema;
}

FieldSchema ranged(FieldSchema schema, double min, double max) {
	schema.ranged = true;
	schema.min = min;
	schema.max = max;
	schema.step = 1;
	return schema;
}

bool whole_of(const Value &value, int64_t &out) {
	if (const auto *whole = std::get_if<int64_t>(&value)) {
		out = *whole;
		return true;
	}
	if (const auto *real = std::get_if<double>(&value); real && std::isfinite(*real) && *real == std::floor(*real)) {
		out = int64_t(*real);
		return true;
	}
	return false;
}

bool set_whole(int64_t &out, const Value &value, int64_t min, int64_t max, const char *what, std::string &error) {
	int64_t number = 0;
	if (!whole_of(value, number) || number < min || number > max) {
		error = std::string(what) + " is a whole number from " + std::to_string(min) + " to " + std::to_string(max) + ".";
		return false;
	}
	out = number;
	return true;
}

// The byte a glyph's index is (the table runs from the space [orig: GameFont_LoadFromBlob @ 0x674740]).
uint8_t byte_of_glyph(size_t index) { return uint8_t(FNT_FIRST_CHAR + index); }

// A glyph's rect field in texels: each coordinate the texel over the page's side (fnt_pixels_to_uv).
LabelledField rect_field(const char *id, const char *label, const char *description, int FontGlyphRect::*member) {
	FieldSchema schema = ranged(schema_of(id, FieldType::Integer, label, description), 0, double(FNT_TEXTURE_WIDTH));
	schema.unit = "texels";
	schema.group = "Rect";
	return LabelledField{schema,
	                     {[member](const RecordHandle &r, Value &out) {
		                      return out = int64_t(font_glyph_rect(glyph_of(r)).*member), true;
	                      },
	                      [member](const RecordHandle &r, const Value &v, std::string &e) {
		                      int64_t number = 0;
		                      if (!set_whole(number, v, 0, FNT_TEXTURE_WIDTH, "A glyph's rect", e)) return false;
		                      FontGlyph &glyph = glyph_of(r);
		                      FontGlyphRect rect = font_glyph_rect(glyph);
		                      rect.*member = int(number);
		                      if (rect.x + rect.width > int(FNT_TEXTURE_WIDTH) || rect.y + rect.height > int(FNT_TEXTURE_HEIGHT)) {
			                      e = "A glyph's rect stays on its page, 256 texels a side.";
			                      return false;
		                      }
		                      fnt_pixels_to_uv(rect.x, rect.y, rect.x + rect.width, rect.y + rect.height, &glyph.uv);
		                      return true;
	                      }}};
}

RecordTable make_table() {
	using RF = LabelledField;
	// --- the font --------------------------------------------------------------------------------
	TableKind font(RecordKindRow{kFont, "font", "Font", "", true});
	font.field(RF{ranged(schema_of("design_width", FieldType::Integer, "Design width",
	                               "The screen width the font is drawn for: the game draws its glyphs at 800 / this of "
	                               "their texels, 800 drawing them texel for texel [orig: GameFont_LoadFromBlob @ "
	                               "0x674740, this+4844 = 800.0 / it], never testing it (D-FNT-1)."),
	                     1, 65535),
	              {[](const RecordHandle &r, Value &out) { return out = int64_t(font_of(r).design_width), true; },
	               [](const RecordHandle &r, const Value &v, std::string &e) {
		               int64_t number = 0;
		               if (!set_whole(number, v, 1, 65535, "The design width", e)) return false;
		               font_of(r).design_width = uint32_t(number);
		               return true;
	               }}});
	font.field(RF{ranged(schema_of("spacing", FieldType::Integer, "Spacing",
	                               "Each glyph steps the pen on by its width and this less one, at the font's scale; the "
	                               "measured width leaves the last step's out [orig: CGameFont_MeasureText @ 0x674e70, "
	                               "this+356; CGameFont_DrawText @ 0x6752c0] (D-FNT-3). The shipped fonts say 0, -2 or -3."),
	                     -256, 256),
	              {[](const RecordHandle &r, Value &out) { return out = int64_t(font_of(r).spacing), true; },
	               [](const RecordHandle &r, const Value &v, std::string &e) {
		               int64_t number = 0;
		               if (!set_whole(number, v, -256, 256, "The spacing", e)) return false;
		               font_of(r).spacing = int32_t(number);
		               return true;
	               }}});
	{
		FieldSchema pages = schema_of("pages", FieldType::Integer, "Pages",
				"The font's pages of 256 x 256 texels, each a texture the game makes [orig: GameFont_LoadFromBlob @ "
				"0x674740, the page count at +8]: what an import of a glyph sheet packs.");
		pages.read_only = true;
		font.field(RF{pages, {[](const RecordHandle &r, Value &out) { return out = int64_t(font_of(r).pages), true; }}});
		FieldSchema line = schema_of("line_height", FieldType::Integer, "Line height",
				"A line of the font's text is the space's height, at the font's scale [orig: CGameFont_DrawText @ "
				"0x6752c0; CGameFont_GetCharExtent @ 0x674e2c..0x674e49].");
		line.read_only = true;
		line.unit = "pixels";
		font.field(RF{line, {[](const RecordHandle &r, Value &out) {
			              const FontRow &f = font_of(r);
			              const FontGlyphRect space = f.glyphs.empty() ? FontGlyphRect() : font_glyph_rect(f.glyphs[0]);
			              return out = int64_t(float(space.height) * fnt_design_scale(f.design_width)), true;
		              }}});
	}
	TableList glyphs;
	glyphs.spec = Document::CollectionSpec{kGlyph, "Glyphs", "", true, Applicability::Reads, FNT_GLYPH_COUNT};
	glyphs.spec.first_number = FNT_FIRST_CHAR;
	glyphs.ops = vector_list<FontRow, FontGlyph>(kGlyph, [](FontRow &f) -> std::vector<FontGlyph> & { return f.glyphs; });
	font.list(std::move(glyphs));

	// --- a glyph ----------------------------------------------------------------------------------
	TableKind glyph(RecordKindRow{kGlyph, "glyph", "Glyph", "", false});
	{
		FieldSchema page = ranged(schema_of("page", FieldType::Integer, "Page",
		                                    "The page the glyph is drawn from [orig: CGameFont_DrawText @ 0x6752c0, one pass "
		                                    "a page]."),
		                          0, FNT_MAX_PAGES - 1);
		glyph.field(RF{page,
		               {[](const RecordHandle &r, Value &out) { return out = int64_t(glyph_of(r).page), true; },
		                [](const RecordHandle &r, const Value &v, std::string &e) {
			                int64_t number = 0;
			                if (!set_whole(number, v, 0, FNT_MAX_PAGES - 1, "A glyph's page", e)) return false;
			                glyph_of(r).page = uint32_t(number);
			                return true;
		                }}});
	}
	glyph.field(rect_field("x", "Left", "The glyph's left edge on its page.", &FontGlyphRect::x));
	glyph.field(rect_field("y", "Top", "The glyph's top edge on its page.", &FontGlyphRect::y));
	glyph.field(rect_field("width", "Width",
	                       "The glyph's width, which is its advance: the format has no other [orig: CGameFont_MeasureText @ "
	                       "0x674e70]. A width of 0 draws nothing and steps the pen back by one less than the spacing.",
	                       &FontGlyphRect::width));
	glyph.field(rect_field("height", "Height",
	                       "The glyph's height; a line of text is the space's [orig: CGameFont_DrawText @ 0x6752c0].",
	                       &FontGlyphRect::height));
	return RecordTable({std::move(font), std::move(glyph)});
}

const FontRow &font_row_of(const Node &node) { return static_cast<const FontRow &>(node); }

size_t glyph_index(const FontRow &font, NodeId id) {
	if (font.ids.lists.empty()) return SIZE_MAX;
	const std::vector<RecordIds> &ids = font.ids.lists[0];
	for (size_t i = 0; i < ids.size(); ++i)
		if (ids[i].id == id) return i;
	return SIZE_MAX;
}

std::string byte_words(uint8_t byte) {
	char hex[8];
	std::snprintf(hex, sizeof(hex), "0x%02X", byte);
	std::string out = hex;
	if (!fnt_byte_is_nonprinting(byte) && byte != 0x20) out += " " + cp1252_to_utf8(std::string(1, char(byte)));
	else if (byte == 0x20) out += " space";
	return out;
}

} // namespace

const RecordTable &font_table() {
	static const RecordTable table = make_table();
	return table;
}

bool is_font_kind(AssetKind kind) { return asset_kind_row(kind).document == DocumentTypeId::Font; }

FontGlyphRect font_glyph_rect(const FontGlyph &glyph) {
	int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
	fnt_uv_to_pixels(&glyph.uv, &x0, &y0, &x1, &y1);
	return {x0, y0, x1 - x0, y1 - y0};
}

int font_glyph_advance(const FontRow &font, const FontGlyph &glyph) {
	// [orig: CGameFont_GetCharExtent @ 0x674de4..0x674e25]: floor(((u1 - u0) * 256 + spacing - 1) * scale + 0.5).
	const float width = (glyph.uv.u1 - glyph.uv.u0) * float(FNT_TEXTURE_WIDTH);
	return int(std::floor((width + float(font.spacing) - 1.0f) * fnt_design_scale(font.design_width) + 0.5f));
}

// --- the row -------------------------------------------------------------------------------------

FontRow::FontRow() { kind = kFont; }

RecordHandle FontRow::record() const { return {kFont, const_cast<FontRow *>(this)}; }

// The texels are shared and never edited: a step holds them once, so they count for nothing here.
size_t FontRow::footprint() const { return sizeof(*this) + footprint_of(glyphs) + ids_footprint(); }

// --- the document ----------------------------------------------------------------------------------

const FontRow *FontDocument::font_row() const {
	for (const auto &node : rows())
		if (node && node->kind == kFont) return &font_row_of(*node);
	return nullptr;
}

std::string FontDocument::record_title(const NodeAddress &address) const {
	const Node *node = row(address.row);
	if (!node) return std::string();
	if (!address.child) return "Font";
	const size_t index = glyph_index(font_row_of(*node), address.child);
	return index == SIZE_MAX ? record_name(address) : byte_words(byte_of_glyph(index));
}

bool FontDocument::font(fnt_font_t &out, std::string &why) const {
	const FontRow *row = font_row();
	if (!row || !row->texels) {
		why = "the font holds no page";
		return false;
	}
	if (fnt_init_blank(&out, row->pages, row->spacing) != FNT_OK) {
		why = "the font's page count is past the format's 16";
		return false;
	}
	out.design_width = row->design_width;
	out.keep_page_rgb = 1; // the texels as given, every shipped font's colour kept (fnt.h)
	for (size_t i = 0; i < FNT_GLYPH_COUNT && i < row->glyphs.size(); ++i) {
		out.glyphs[i].page = row->glyphs[i].page;
		out.glyphs[i].uv = row->glyphs[i].uv;
	}
	std::memcpy(out.pages, row->texels->data(), std::min(row->texels->size(), size_t(row->pages) * FNT_TEXTURE_SIZE));
	const fnt_error_t valid = fnt_validate(&out);
	if (valid != FNT_OK) {
		why = valid == FNT_ERR_INVALID_GLYPH_PAGE ? "a glyph is on a page the font lacks" : fnt_error_string(valid);
		fnt_free(&out);
		return false;
	}
	return true;
}

bool FontDocument::parse(const std::vector<uint8_t> &bytes, std::vector<std::shared_ptr<Node>> &rows,
                         std::shared_ptr<const FileState> &, std::vector<SourceIssue> &issues, Diagnostic &error) {
	if (!is_font_kind(kind())) {
		error = make_finding(CoreFinding::DocumentKind, DiagnosticSeverity::Error, "This file is not a font.", path());
		return false;
	}
	fnt_font_t read{};
	const fnt_error_t parsed = fnt_parse(bytes.data(), bytes.size(), &read);
	if (parsed != FNT_OK) {
		error = make_finding(CoreFinding::DocumentParse, DiagnosticSeverity::Error,
		                     std::string("The font could not be read: ") + fnt_error_string(parsed) + ".", path());
		return false;
	}
	auto row = std::make_shared<FontRow>();
	row->design_width = read.design_width;
	row->spacing = read.glyph_spacing;
	row->pages = read.num_pages;
	row->glyphs.resize(FNT_GLYPH_COUNT);
	for (size_t i = 0; i < FNT_GLYPH_COUNT; ++i) row->glyphs[i] = {read.glyphs[i].page, read.glyphs[i].uv};
	row->texels = std::make_shared<const std::vector<uint8_t>>(read.pages, read.pages + size_t(read.num_pages) * FNT_TEXTURE_SIZE);
	fnt_free(&read);
	// What the game's loader never reads and a save writes as nothing: the header's words past the spacing, bytes past
	// the last page [orig: GameFont_LoadFromBlob @ 0x674740].
	bool header_words = false;
	for (size_t i = 16; i < FNT_HEADER_SIZE && i < bytes.size(); ++i) header_words |= bytes[i] != 0;
	if (header_words)
		issues.push_back({false, 0, std::string(), std::string(),
		                  "The header's last four words hold values the game never reads: a save writes them as 0."});
	const size_t size = fnt_calculate_file_size(row->pages);
	if (bytes.size() > size)
		issues.push_back({false, 0, std::string(), std::string(),
		                  std::to_string(bytes.size() - size) +
		                          " bytes follow the last page, which the game never reads: a save leaves them out."});
	shape(*row);
	rows.push_back(std::move(row));
	return true;
}

SerializeResult FontDocument::serialize() const {
	SerializeResult result;
	fnt_font_t made{};
	std::string why;
	if (!font(made, why)) {
		result.issues.push_back({true, 0, std::string(), std::string(), "The font could not be written: " + why + "."});
		return result;
	}
	std::vector<uint8_t> bytes(fnt_calculate_file_size(made.num_pages));
	size_t written = 0;
	const fnt_error_t error = fnt_write(&made, bytes.data(), bytes.size(), &written);
	fnt_free(&made);
	if (error != FNT_OK || written != bytes.size()) {
		result.issues.push_back(
		        {true, 0, std::string(), std::string(), std::string("The font could not be written: ") + fnt_error_string(error) + "."});
		return result;
	}
	result.text.assign(bytes.begin(), bytes.end());
	return result;
}

std::string FontDocument::save_words() const {
	return "A save writes the font from its header, its glyph table and its pages as read: each glyph's rect as its texels "
	       "over 256, the header's unread words 0.";
}

std::shared_ptr<Node> FontDocument::make_node(NodeKind, NodeId, const std::vector<std::shared_ptr<const Node>> &,
                                              std::string &error) {
	error = "A font keeps its one row and its 224 glyphs; an import of a glyph sheet makes a font anew.";
	return nullptr;
}

bool FontDocument::accept_step(const EditStep &, const StagedRows &staged, StepRefusal &refusal) const {
	if (staged.rows().size() == 1) return true;
	refusal.message = "A font keeps its one row.";
	return false;
}

// --- the findings ------------------------------------------------------------------------------------

namespace {

constexpr FindingCodeEntry<FontFinding> kFindingEntries[] = {
	{ FontFinding::InvalidInput, { "font.invalid_input", FindingFix::None, nullptr, true } },
	{ FontFinding::IgnoredInput, { "font.ignored_input", FindingFix::Rewrite, kRewriteDropsIgnoredInput } },
	// The drawer binds the glyph's page from the font's table past its pages [orig: CGameFont_DrawText @ 0x6752c0]:
	// what it draws then is no texture of the font's; the writer refuses it.
	{ FontFinding::Unserializable, { "font.unserializable", FindingFix::None, nullptr, true } },
	// The page is sampled clamped [orig: GameFont_LoadFromBlob @ 0x674804, flags 0x140001]: the edge's texels
	// stretched; no refusal witnessed, listed.
	{ FontFinding::GlyphOutside, listed_code("font.glyph_outside") },
	{ FontFinding::GlyphHeight, listed_code("font.glyph_height") },
};
static_assert(std::size(kFindingEntries) == static_cast<size_t>(FontFinding::kCount), "every FontFinding has exactly one row");
static_assert(finding_entries_well_formed(kFindingEntries), "the font's rows follow FontFinding's order, each token its own");
constexpr auto kFindingRows = finding_rows(kFindingEntries, FindingGroup::Fonts);
static_assert(finding_rows_well_formed(kFindingRows), "every row of the table takes its group");

} // namespace

const FindingCodeRow &finding_code(FontFinding code) { return kFindingRows[static_cast<size_t>(code)]; }

FindingTable font_finding_codes() { return {kFindingRows.data(), kFindingRows.size()}; }

std::vector<Diagnostic> validate_font_file(const DocumentBase &document) {
	std::vector<Diagnostic> findings;
	const auto *font_document = dynamic_cast<const FontDocument *>(&document);
	if (!font_document) return findings;
	source_issue_findings(*font_document, finding_code(FontFinding::InvalidInput), finding_code(FontFinding::IgnoredInput),
	                      findings);
	if (document.blocked()) return findings;
	const FontRow *font = font_document->font_row();
	if (!font || font->ids.lists.empty()) return findings;
	const std::vector<RecordIds> &ids = font->ids.lists[0];
	const auto add = [&](size_t index, DiagnosticSeverity severity, FontFinding code, const char *field,
	                     const std::string &message) {
		Diagnostic d = make_finding(code, severity, message, document.path(), field);
		d.row_id = font->id;
		d.child_id = index < ids.size() ? ids[index].id : 0;
		d.record_kind = kGlyph;
		d.record = byte_words(byte_of_glyph(index));
		findings.push_back(std::move(d));
	};
	const int line = font->glyphs.empty() ? 0 : font_glyph_rect(font->glyphs[0]).height;
	for (size_t i = 0; i < font->glyphs.size() && i < FNT_GLYPH_COUNT; ++i) {
		const FontGlyph &glyph = font->glyphs[i];
		const FontGlyphRect rect = font_glyph_rect(glyph);
		const uint8_t byte = byte_of_glyph(i);
		if (glyph.page >= font->pages)
			add(i, DiagnosticSeverity::Error, FontFinding::Unserializable, "page",
			    byte_words(byte) + " is on page " + std::to_string(glyph.page) + ", which the font lacks (" +
			            std::to_string(font->pages) + " pages): the game draws it from no page of the font [orig: "
			            "CGameFont_DrawText @ 0x6752c0], so the font is not saved until it names one.");
		if (rect.x + rect.width > int(FNT_TEXTURE_WIDTH) || rect.y + rect.height > int(FNT_TEXTURE_HEIGHT) ||
		    glyph.uv.u0 < 0.0f || glyph.uv.v0 < 0.0f)
			add(i, DiagnosticSeverity::Warning, FontFinding::GlyphOutside, "width",
			    byte_words(byte) + "'s rect reaches past its page: the game samples the page clamped [orig: "
			                       "GameFont_LoadFromBlob @ 0x674804], so the page's edge stretches over the rest.");
		if (!fnt_byte_is_nonprinting(byte) && rect.height != 0 && rect.height != line)
			add(i, DiagnosticSeverity::Info, FontFinding::GlyphHeight, "height",
			    byte_words(byte) + " is " + std::to_string(rect.height) + " texels tall and the space " + std::to_string(line) +
			            ": a line of text is the space's height [orig: CGameFont_DrawText @ 0x6752c0], so it reaches "
			            "into the next or stops short.");
	}
	return findings;
}

} // namespace opennova::editor
