#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <editor/preview/viewport_model.h>
#include <formats/fnt/fnt.h>
#include <runtime/hud/game_font.h>

namespace opennova::editor {

// What a font viewport shows, and why not (round S23 lane A): the kind's reason.
enum class FontViewStatus : uint8_t {
	NoFont,     // no font is open at its path
	Unwritable, // the font cannot be made (a glyph on a page it lacks): the game could not draw it
	Ready,
};
// "no_font", "unwritable", "ready".
const char *font_view_status_token(FontViewStatus status);

// What the picture shows of the font (its options, each a SetViewport): `text`, drawn as the game draws text
// (its lines split at a line break, its bytes the game's code page), in `color` (0xRRGGBB, the colour a caller
// asks for: halved as every caller halves it and doubled back by the page's MODULATE2X), `bold`, `italic` and
// `underline` (the drawer's styles); or, with `page` 0 and up, that page of the font with its glyphs' rects. Each
// game pixel `zoom` picture pixels (1 to 8), texel for texel.
struct FontViewportOptions {
	std::string text = "The quick brown fox jumps over the lazy dog.\n0123456789 !?.,:;'\"()[]<>+-*/=%&#@";
	uint32_t color = 0xFFFFFF;
	int page = -1;
	int zoom = 2;
	bool bold = false;
	bool italic = false;
	bool underline = false;
	bool operator==(const FontViewportOptions &other) const;
	bool operator!=(const FontViewportOptions &other) const { return !(*this == other); }
};
inline constexpr int kFontZoomMost = 8;
// The picture's margin round the text or the page, game pixels.
inline constexpr float kFontPictureMargin = 8.0f;
io::JsonValue font_options_json(const FontViewportOptions &options);
// The change a SetViewport makes to set a font viewport's options to `options`.
std::string font_options_change(const FontViewportOptions &options);

// The font the picture draws: its header and glyph table, and its pages' texels (shared with the document).
struct FontPicture {
	fnt::fnt_font_t font{}; // pages null: the texels are `texels`
	std::shared_ptr<const std::vector<uint8_t>> texels;
	FontPicture() = default;
	FontPicture(const FontPicture &) = delete;
	FontPicture &operator=(const FontPicture &) = delete;
	~FontPicture();
	const uint8_t *page(uint32_t index) const;
};

// One glyph the picture drew, in the picture's game pixels (before the zoom): the glyph's index in the table (its
// byte less 0x20) and its box.
struct FontPictureGlyph {
	size_t glyph = 0;
	float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
};

// A font's viewport (round S23 lane A; ViewportKind::Font, the Main role of the font type): its text laid out by
// the game's own text engine (runtime/hud GameFont: the measurer's and the drawer's walk, the styles, the format
// tags, the page passes [orig: CGameFont_MeasureText @ 0x674e70; CGameFont_DrawText @ 0x6752c0]) and drawn by the
// Shell's device as the game draws a font's page, MODULATE2X(TEXTURE, DIFFUSE) [orig: GameFont_LoadFromBlob @
// 0x674825, mode 0x651] over the caller's halved colour (D-HUD-51), sampled texel for texel; or a page of it with
// its glyphs' rects. It follows the font open at its path, as it stands: an edit of a glyph's rect or the spacing
// lays the text out again. A point of the picture names the glyph under it, and a click selects that glyph's
// record.
class FontViewport final : public ViewportModel {
public:
	explicit FontViewport(std::string path);
	static std::unique_ptr<ViewportModel> make(const std::string &path);

	FontViewStatus view_status() const { return reason_; }
	const FontViewportOptions &options() const { return options_; }
	// The font drawn (null unless Ready), the glyph quads of the text as the game lays it out (in game pixels from the
	// picture's top left, its colour the halved diffuse), and the page shown (-1 the text).
	const std::shared_ptr<const FontPicture> &picture() const { return picture_; }
	const hud::GameFontRun &run() const { return run_; }
	int shown_page() const;
	// The glyphs drawn, in the picture's game pixels: the text's (each quad's glyph by its page and coordinates), or
	// every glyph of the page shown with a rect.
	const std::vector<FontPictureGlyph> &glyphs() const { return glyphs_; }
	// The size of what is drawn, game pixels (the text's measure or the page's side, and the margins).
	float picture_width() const { return width_; }
	float picture_height() const { return height_; }
	// A serial that moves each time the picture is laid out again (what the device draws anew).
	uint64_t layout_serial() const { return layout_serial_; }
	// The glyph under picture pixel (x, y) (zoomed): false for none.
	bool glyph_at(float x, float y, FontPictureGlyph &out) const;
	// A glyph's words: "0x41 A: page 0, 10, 20, 7 x 12, advance 6".
	std::string glyph_words(size_t glyph) const;
	// The glyph record of a glyph's index in the document at the path (0 for none).
	NodeAddress glyph_address(const ViewportInput &input, size_t glyph) const;

	ViewportStatus status() const override;
	const char *reason() const override { return font_view_status_token(reason_); }
	std::string message() const override;
	const std::string &detail() const override { return detail_; }
	std::string caption() const override;
	const char *units() const override { return "pixels"; }
	ViewportLayout layout() const override { return ViewportLayout(); }
	std::unique_ptr<CanvasHalf> make_canvas() const override;
	// The glyph under the point: index its place in the table, id its record, name its words, kind "glyph".
	ViewportHit hit(const ViewportContext &context, float x, float y) const override;
	bool handle_point(const ViewportContext &context, NodeId id, const std::string &handle, float &x, float &y,
	                  std::string &error) const override;
	bool drag(const ViewportContext &context, const ViewportDrag &drag, CanvasRequests &out,
	          std::string &error) const override;
	bool command(const ViewportContext &context, const std::string &name, const std::vector<NodeId> &ids,
	             CanvasRequests &out, std::string &error) const override;
	bool click_frame(const ViewportContext &context, SelectMode mode, int &width, int &height,
	                 std::string &error) const override;
	io::JsonValue options_json() const override;
	// The font's facts (its pages, design width and scale, spacing, line height), the page shown, the text's
	// measured size as the game measures it, and what its device drew last (`drawn`: its glyph quads and pages).
	io::JsonValue body_json(const ViewportInput &input) const override;
	// The glyphs drawn: each {glyph, byte, character, rect [x0, y0, x1, y1] in picture pixels}.
	io::JsonValue items_json(const ViewportInput &input) const override;

protected:
	ViewportAction follow_(const ViewportInput &input, PreviewClock &clock) override;
	bool takes_(const std::string &member) const override;
	bool check_(const io::JsonValue &json, std::string &error) const override;
	void apply_(const io::JsonValue &json, PreviewClock &clock) override;
	bool report_(const ViewportDeviceReport &report) override;

private:
	// The text laid out again, or the page's glyphs listed, over the font drawn.
	void lay_out_();

	FontViewStatus reason_ = FontViewStatus::NoFont;
	std::string detail_;
	FontViewportOptions options_;
	FontViewportOptions laid_out_;
	std::shared_ptr<const FontPicture> picture_;
	hud::GameFontRun run_;
	std::vector<FontPictureGlyph> glyphs_;
	float width_ = 0.0f, height_ = 0.0f;
	uint64_t layout_serial_ = 0;
	int measured_w_ = 0, measured_h_ = 0;
	// The texels the device holds (a Rebuild when they change; the glyph table's and the options' changes an Update).
	std::shared_ptr<const std::vector<uint8_t>> built_texels_;
	uint32_t built_pages_ = 0;
	io::JsonValue drawn_; // the device's last report of what it drew
};

} // namespace opennova::editor
