#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <editor/assets/asset_registry.h>
#include <editor/model/finding_code_row.h>
#include <editor/model/table_document.h>
#include <formats/fnt/fnt.h>

namespace opennova::editor {

// A font (ADR 0046, round S23 lane A): a `.fnt`, the bitmap font the menus and the HUD write with, read and written
// through the engine's own reader and writer (fnt::fnt_parse, fnt::fnt_write: the writer from scratch, ADR 0003)
// as the game's loader reads it (docs/fonts/fnt-re.md) [orig: GameFont_LoadFromBlob @ 0x674740]: one row, the font,
// its design width (the game draws it at 800 / it of its texels), its spacing (the advance term each glyph's width
// takes, less one) and its pages of 256 x 256 texels; and its 224 glyphs, the bytes 0x20..0xFF in order, each the
// page it is on and its rect there in texels, which is its advance [orig: CGameFont_MeasureText @ 0x674e70;
// CGameFont_DrawText @ 0x6752c0]. A glyph's rect is held as the file's four coordinates (the texel over 256), so
// every shipped font writes back byte for byte; a rect set in texels writes each coordinate as its texel over 256.
// The pages' texels are what the font draws: an import of a glyph sheet makes them (import/font_import); the
// document keeps them as read, the colour of each texel as given (fnt_font_t::keep_page_rgb).

enum class FontKind : NodeKind { Font = 0, Glyph = 1 };
constexpr NodeKind node_kind(FontKind kind) { return static_cast<NodeKind>(kind); }

struct FontGlyph {
	uint32_t page = 0;
	fnt::fnt_uv_t uv{};
};

// The font row: its header words, its glyph table and its pages' texels (shared: no edit of the document changes
// them, so a step of its history holds them once).
struct FontRow : TableRow {
	uint32_t design_width = fnt::FNT_DEFAULT_DESIGN_WIDTH;
	int32_t spacing = 0;
	uint32_t pages = 1;
	std::vector<FontGlyph> glyphs; // fnt::FNT_GLYPH_COUNT, byte 0x20 first
	std::shared_ptr<const std::vector<uint8_t>> texels;

	FontRow();
	std::shared_ptr<Node> clone() const override { return std::make_shared<FontRow>(*this); }
	std::string name() const override { return "Font"; }
	RecordHandle record() const override;
	size_t footprint() const override;
};

const RecordTable &font_table();

class FontDocument : public TableDocument {
public:
	const RecordTable &table() const override { return font_table(); }
	static const std::vector<FieldSchema> &schema(NodeKind kind) { return font_table().fields(kind); }
	// A glyph by its byte and character ("0x41 A").
	std::string record_title(const NodeAddress &address) const override;
	SerializeResult serialize() const override;
	std::unique_ptr<DocumentBase> snapshot() const override { return std::make_unique<FontDocument>(*this); }
	std::string save_words() const override;

	const FontRow *font_row() const;
	// The font as the engine holds it, made from the row (what serialize() writes and the preview draws): false
	// where it cannot be made (a glyph on a page the font lacks). The caller frees it (fnt::fnt_free).
	bool font(fnt::fnt_font_t &out, std::string &why) const;

protected:
	bool parse(const std::vector<uint8_t> &bytes, std::vector<std::shared_ptr<Node>> &rows,
	           std::shared_ptr<const FileState> &state, std::vector<SourceIssue> &issues,
	           Diagnostic &error) override;
	std::shared_ptr<Node> make_node(NodeKind kind, NodeId id, const std::vector<std::shared_ptr<const Node>> &rows,
	                                std::string &error) override;
	bool accept_step(const EditStep &step, const StagedRows &rows, StepRefusal &refusal) const override;
};

bool is_font_kind(AssetKind kind);

// A glyph's rect in texels on its page, as the game reads its coordinates (fnt_uv_to_pixels: each times 256,
// truncated).
struct FontGlyphRect {
	int x = 0, y = 0, width = 0, height = 0;
};
FontGlyphRect font_glyph_rect(const FontGlyph &glyph);
// A glyph's advance in the game's pixels at the font's design scale: its width and the spacing less one, rounded
// [orig: CGameFont_GetCharExtent @ 0x674de4..0x674e25].
int font_glyph_advance(const FontRow &font, const FontGlyph &glyph);

// The font type's validator (DocumentType::validate_file): its source findings (input a save leaves out), a glyph
// on a page the font lacks (the save refuses it), one reaching past its page, one of another height than the space
// (a line is the space's height).
std::vector<Diagnostic> validate_font_file(const DocumentBase &document);

enum class FontFinding { InvalidInput, IgnoredInput, Unserializable, GlyphOutside, GlyphHeight, kCount };
const FindingCodeRow &finding_code(FontFinding code);
FindingTable font_finding_codes();

} // namespace opennova::editor
