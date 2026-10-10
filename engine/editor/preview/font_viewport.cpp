// The font viewport (font_viewport.h): the font open at its path laid out by the game's text engine, or a page of
// it with its glyphs' rects; its canvas half, which names and selects the glyph under the pointer.
#include <editor/preview/font_viewport.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <tuple>
#include <utility>

#include <base/io/cp1252.h>
#include <editor/documents/font_document.h>
#include <editor/preview/canvas_half.h>
#include <editor/preview/viewport_device.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>

namespace opennova::editor {

using io::JsonValue;
using io::json_number;
using io::json_string;

namespace {

// The size a headless Shell draws the picture at.
constexpr ViewportState kHeadlessSize{640, 360};

bool read_options(const JsonValue &json, FontViewportOptions &options, std::string &error) {
	if (!json.is_object()) {
		error = "options is an object: {text, color, page, zoom, bold, italic, underline}.";
		return false;
	}
	FontViewportOptions read = options;
	for (const auto &[key, value] : json.object) {
		if (key == "text") {
			if (!value.is_string()) {
				error = "options.text is a text.";
				return false;
			}
			read.text = value.string;
		} else if (key == "color") {
			if (!value.is_number() || value.number < 0 || value.number > double(0xFFFFFF) || value.number != std::floor(value.number)) {
				error = "options.color is 0xRRGGBB, a whole number from 0 to 16777215.";
				return false;
			}
			read.color = uint32_t(value.number);
		} else if (key == "page" || key == "zoom") {
			const int least = key == "page" ? -1 : 1, most = key == "page" ? int(fnt::FNT_MAX_PAGES) - 1 : kFontZoomMost;
			if (!value.is_number() || value.number != std::floor(value.number) || value.number < least || value.number > most) {
				error = "options." + key + " is a whole number from " + std::to_string(least) + " to " + std::to_string(most) +
				        (key == "page" ? " (-1 the text)." : ".");
				return false;
			}
			(key == "page" ? read.page : read.zoom) = int(value.number);
		} else if (key == "bold" || key == "italic" || key == "underline") {
			if (!value.is_bool()) {
				error = "options." + key + " is true or false.";
				return false;
			}
			(key == "bold" ? read.bold : key == "italic" ? read.italic : read.underline) = value.boolean;
		} else {
			error = "A font viewport's options have no \"" + key + "\" (text, color, page, zoom, bold, italic, underline).";
			return false;
		}
	}
	options = read;
	return true;
}

std::string byte_words(uint8_t byte) {
	char hex[8];
	std::snprintf(hex, sizeof(hex), "0x%02X", byte);
	if (byte == 0x20) return std::string(hex) + " space";
	if (fnt::fnt_byte_is_nonprinting(byte)) return hex;
	return std::string(hex) + " " + cp1252_to_utf8(std::string(1, char(byte)));
}

const FontDocument *font_document(const ViewportInput &input) {
	return input.document ? dynamic_cast<const FontDocument *>(input.document) : nullptr;
}

// The font viewport's half of a canvas: the picture fills the canvas; a click selects the glyph under the pointer,
// whose words are the hover tip; the glyph hovered and the one selected are outlined (on a page, every glyph's rect
// dimly).
class FontCanvas final : public CanvasHalf {
public:
	const CanvasGesture &gesture() const override { return gesture_; }
	void follow(const ViewportModel &viewport, const ViewportContext &context, CanvasRequests &out) override {
		viewport_ = static_cast<const FontViewport *>(&viewport);
		selected_ = SIZE_MAX;
		const NodeAddress &primary = context.input.view.documents.selection.primary;
		if (context.input.view.documents.active == viewport.path() && primary.kind == node_kind(FontKind::Glyph))
			if (const FontDocument *font = font_document(context.input); font && font->font_row()) {
				const std::vector<RecordIds> &ids = font->font_row()->ids.lists[0];
				for (size_t i = 0; i < ids.size(); ++i)
					if (ids[i].id == primary.child) selected_ = i;
			}
		CanvasSubject subject;
		subject.path = viewport.path();
		gesture_.frame(subject, out);
	}
	void input(const ViewportContext &context, const CanvasInput &in, CanvasRequests &out) override {
		if (!viewport_) return;
		if (in.pressed && !in.middle) {
			CanvasSubject subject;
			subject.path = viewport_->path();
			gesture_.press(subject, in.screen, out);
			FontPictureGlyph glyph;
			if (viewport_->glyph_at(in.mouse.x, in.mouse.y, glyph))
				if (const NodeAddress at = viewport_->glyph_address(context.input, glyph.glyph); at.row)
					out.request(request::select_record(viewport_->path(), at));
		}
		if (gesture_.pressed() && !in.down) gesture_.release(out);
	}
	void end(CanvasRequests &out) override { gesture_.end(out); }
	void end_frame(CanvasRequests &out) override { gesture_.end_frame(out); }
	OverlayList shapes(const ViewportContext &, const CanvasInput &in) const override {
		OverlayList list;
		if (!viewport_) return list;
		const float zoom = float(viewport_->options().zoom);
		const auto box = [&](const FontPictureGlyph &g, OverlayRole role, float width) {
			list.rect({g.x0 * zoom, g.y0 * zoom}, {g.x1 * zoom, g.y1 * zoom}, role, width);
		};
		if (viewport_->shown_page() >= 0)
			for (const FontPictureGlyph &g : viewport_->glyphs()) box(g, OverlayRole::Normal, 1.0f);
		for (const FontPictureGlyph &g : viewport_->glyphs())
			if (g.glyph == selected_) box(g, OverlayRole::Selected, 2.0f);
		FontPictureGlyph hovered;
		if (in.hovered && viewport_->glyph_at(in.mouse.x, in.mouse.y, hovered)) box(hovered, OverlayRole::Hover, 1.0f);
		return list;
	}
	CanvasCursor cursor(const ViewportContext &, const CanvasInput &) const override { return CanvasCursor::Default; }
	std::string hover_tip(const ViewportContext &, const CanvasInput &in) const override {
		FontPictureGlyph glyph;
		if (!viewport_ || !in.hovered || !viewport_->glyph_at(in.mouse.x, in.mouse.y, glyph)) return std::string();
		return viewport_->glyph_words(glyph.glyph);
	}

private:
	CanvasGesture gesture_;
	const FontViewport *viewport_ = nullptr;
	size_t selected_ = SIZE_MAX;
};

} // namespace

const char *font_view_status_token(FontViewStatus status) {
	switch (status) {
	case FontViewStatus::NoFont: return "no_font";
	case FontViewStatus::Unwritable: return "unwritable";
	case FontViewStatus::Ready: return "ready";
	}
	return "no_font";
}

bool FontViewportOptions::operator==(const FontViewportOptions &o) const {
	return text == o.text && color == o.color && page == o.page && zoom == o.zoom && bold == o.bold && italic == o.italic &&
	       underline == o.underline;
}

JsonValue font_options_json(const FontViewportOptions &options) {
	JsonValue out = JsonValue::make_object();
	out.set("text", json_string(options.text));
	out.set("color", json_number(double(options.color)));
	out.set("page", json_number(options.page));
	out.set("zoom", json_number(options.zoom));
	out.set("bold", JsonValue::make_bool(options.bold));
	out.set("italic", JsonValue::make_bool(options.italic));
	out.set("underline", JsonValue::make_bool(options.underline));
	return out;
}

std::string font_options_change(const FontViewportOptions &options) {
	return viewport_change(ViewportKind::Font, "options", font_options_json(options));
}

FontPicture::~FontPicture() {
	font.pages = nullptr; // the texels are shared, never the font's own
}

const uint8_t *FontPicture::page(uint32_t index) const {
	if (!texels || index >= font.num_pages || size_t(index + 1) * fnt::FNT_TEXTURE_SIZE > texels->size()) return nullptr;
	return texels->data() + size_t(index) * fnt::FNT_TEXTURE_SIZE;
}

FontViewport::FontViewport(std::string path) : ViewportModel(ViewportKind::Font, std::move(path), kHeadlessSize) {}

std::unique_ptr<ViewportModel> FontViewport::make(const std::string &path) { return std::make_unique<FontViewport>(path); }

int FontViewport::shown_page() const {
	if (!picture_ || options_.page < 0) return -1;
	return std::min(options_.page, int(picture_->font.num_pages) - 1);
}

ViewportStatus FontViewport::status() const {
	switch (reason_) {
	case FontViewStatus::Ready: return ViewportStatus::Ready;
	case FontViewStatus::Unwritable: return ViewportStatus::Failed;
	case FontViewStatus::NoFont: break;
	}
	return ViewportStatus::Empty;
}

std::string FontViewport::message() const {
	switch (reason_) {
	case FontViewStatus::NoFont: return "No font is open here.";
	case FontViewStatus::Unwritable: return "The game could not draw this font: " + detail_ + ".";
	case FontViewStatus::Ready: break;
	}
	return std::string();
}

std::string FontViewport::caption() const {
	if (!picture_) return std::string();
	const int page = shown_page();
	return page >= 0 ? " - page " + std::to_string(page + 1) + " of " + std::to_string(picture_->font.num_pages) : " - text";
}

std::unique_ptr<CanvasHalf> FontViewport::make_canvas() const { return std::make_unique<FontCanvas>(); }

bool FontViewport::glyph_at(float x, float y, FontPictureGlyph &out) const {
	const float zoom = float(std::max(options_.zoom, 1));
	const float gx = x / zoom, gy = y / zoom;
	// The last drawn wins, as on the picture.
	for (auto it = glyphs_.rbegin(); it != glyphs_.rend(); ++it)
		if (gx >= it->x0 && gx < it->x1 && gy >= it->y0 && gy < it->y1) {
			out = *it;
			return true;
		}
	return false;
}

std::string FontViewport::glyph_words(size_t glyph) const {
	if (!picture_ || glyph >= fnt::FNT_GLYPH_COUNT) return std::string();
	const FontGlyph g{picture_->font.glyphs[glyph].page, picture_->font.glyphs[glyph].uv};
	const FontGlyphRect rect = font_glyph_rect(g);
	FontRow header;
	header.design_width = picture_->font.design_width;
	header.spacing = picture_->font.glyph_spacing;
	return byte_words(uint8_t(fnt::FNT_FIRST_CHAR + glyph)) + ": page " + std::to_string(g.page) + ", " +
	       std::to_string(rect.x) + ", " + std::to_string(rect.y) + ", " + std::to_string(rect.width) + " x " +
	       std::to_string(rect.height) + ", advance " + std::to_string(font_glyph_advance(header, g));
}

NodeAddress FontViewport::glyph_address(const ViewportInput &input, size_t glyph) const {
	const FontDocument *font = font_document(input);
	const FontRow *row = font ? font->font_row() : nullptr;
	if (!row || row->ids.lists.empty() || glyph >= row->ids.lists[0].size()) return NodeAddress();
	return {row->id, node_kind(FontKind::Glyph), row->ids.lists[0][glyph].id};
}

ViewportHit FontViewport::hit(const ViewportContext &context, float x, float y) const {
	ViewportHit out;
	out.current = current(context.input);
	FontPictureGlyph glyph;
	if (!glyph_at(x, y, glyph)) return out;
	out.index = int(glyph.glyph);
	out.id = out.current ? glyph_address(context.input, glyph.glyph).child : 0;
	out.name = glyph_words(glyph.glyph);
	out.kind = "glyph";
	return out;
}

bool FontViewport::handle_point(const ViewportContext &, NodeId, const std::string &, float &, float &,
                                std::string &error) const {
	error = "A font's picture has no handles: a glyph's rect is set in its record.";
	return false;
}

bool FontViewport::drag(const ViewportContext &, const ViewportDrag &, CanvasRequests &, std::string &error) const {
	error = "Nothing is dragged in a font's picture: a glyph's rect is set in its record.";
	return false;
}

bool FontViewport::command(const ViewportContext &, const std::string &name, const std::vector<NodeId> &, CanvasRequests &,
                           std::string &error) const {
	error = "A font's picture has no command \"" + name + "\".";
	return false;
}

bool FontViewport::click_frame(const ViewportContext &context, SelectMode mode, int &width, int &height,
                               std::string &error) const {
	if (mode != SelectMode::Replace) {
		error = "A font's picture selects one glyph at a time: no Shift or Ctrl click.";
		return false;
	}
	if (!current(context.input)) {
		error = "The font's picture is not the document as it stands.";
		return false;
	}
	width = context.width;
	height = context.height;
	return true;
}

JsonValue FontViewport::options_json() const { return font_options_json(options_); }

JsonValue FontViewport::body_json(const ViewportInput &) const {
	JsonValue out = JsonValue::make_object();
	if (!picture_) return out;
	const fnt::fnt_font_t &font = picture_->font;
	out.set("pages", json_number(double(font.num_pages)));
	out.set("design_width", json_number(double(font.design_width)));
	out.set("scale", json_number(double(fnt::fnt_design_scale(font.design_width))));
	out.set("spacing", json_number(double(font.glyph_spacing)));
	hud::GameFont engine;
	engine.set_font(&font);
	out.set("line_height", json_number(double(engine.line_height(1.0f))));
	out.set("page_shown", json_number(shown_page()));
	JsonValue measured = JsonValue::make_object();
	measured.set("width", json_number(double(measured_w_)));
	measured.set("height", json_number(double(measured_h_)));
	out.set("measured", std::move(measured));
	out.set("quads", json_number(double(run_.quads.size())));
	out.set("drawn", drawn_);
	return out;
}

bool FontViewport::report_(const ViewportDeviceReport &report) {
	const auto quads = [](const JsonValue &drawn) {
		const JsonValue *member = drawn.get("quads");
		return member ? member->number : -1.0;
	};
	const bool moved = quads(drawn_) != quads(report.drawn);
	drawn_ = report.drawn;
	return moved;
}

JsonValue FontViewport::items_json(const ViewportInput &) const {
	JsonValue out = JsonValue::make_array();
	const float zoom = float(options_.zoom);
	for (const FontPictureGlyph &g : glyphs_) {
		JsonValue item = JsonValue::make_object();
		const uint8_t byte = uint8_t(fnt::FNT_FIRST_CHAR + g.glyph);
		item.set("glyph", json_number(double(g.glyph)));
		item.set("byte", json_number(double(byte)));
		item.set("words", json_string(byte_words(byte)));
		JsonValue rect = JsonValue::make_array();
		for (const float v : {g.x0 * zoom, g.y0 * zoom, g.x1 * zoom, g.y1 * zoom}) rect.array.push_back(json_number(v));
		item.set("rect", std::move(rect));
		out.array.push_back(std::move(item));
	}
	return out;
}

void FontViewport::lay_out_() {
	++layout_serial_;
	run_ = hud::GameFontRun();
	glyphs_.clear();
	measured_w_ = measured_h_ = 0;
	width_ = height_ = 0.0f;
	if (!picture_) return;
	const fnt::fnt_font_t &font = picture_->font;
	const int page = shown_page();
	if (page >= 0) {
		// A page: its glyphs' rects at the page's texels, from the picture's margin.
		for (size_t i = 0; i < fnt::FNT_GLYPH_COUNT; ++i) {
			if (font.glyphs[i].page != uint32_t(page)) continue;
			const FontGlyphRect rect = font_glyph_rect({font.glyphs[i].page, font.glyphs[i].uv});
			if (rect.width <= 0 || rect.height <= 0) continue;
			glyphs_.push_back({i, kFontPictureMargin + float(rect.x), kFontPictureMargin + float(rect.y),
			                   kFontPictureMargin + float(rect.x + rect.width), kFontPictureMargin + float(rect.y + rect.height)});
		}
		width_ = height_ = float(fnt::FNT_TEXTURE_WIDTH) + 2.0f * kFontPictureMargin;
		return;
	}
	// The text in the game's code page (a character it has not a '?'), laid out by the game's own text engine at
	// scale 1 [orig: CGameFont_MeasureText @ 0x674e70; CGameFont_DrawText @ 0x6752c0], its colour halved as every
	// caller halves it before the page's MODULATE2X doubles it back [orig: the HUD wrappers @ 0x5804c0..0x58096a;
	// CFontCache_DrawTextScaled @ 0x6531e7..0x6531eb] (D-HUD-51).
	std::string bytes;
	if (!utf8_to_cp1252(options_.text, bytes)) {
		bytes.clear();
		for (const char c : options_.text) bytes.push_back(static_cast<unsigned char>(c) < 0x80 ? c : '?');
	}
	hud::GameFont engine;
	engine.set_font(&font);
	uint32_t flags = 0;
	if (options_.bold) flags |= hud::kFontStyleBold;
	if (options_.italic) flags |= hud::kFontStyleItalic;
	if (options_.underline) flags |= hud::kFontStyleUnderline;
	const uint32_t halved = 0xFF000000u | ((options_.color >> 1) & 0x7F7F7Fu);
	hud::GameFontState state;
	state.bold = options_.bold;
	state.italic = options_.italic;
	state.underline = options_.underline;
	state.color = state.original_color = halved;
	engine.measure(bytes.c_str(), 1.0f, 1.0f, &measured_w_, &measured_h_, nullptr);
	run_ = engine.layout(bytes.c_str(), kFontPictureMargin, kFontPictureMargin, 1.0f, 1.0f, flags, halved, &state);
	// Each quad's glyph, by its page and its coordinates (the first glyph of them: two of one rect draw alike).
	std::map<std::tuple<uint32_t, float, float, float>, size_t> by_rect;
	for (size_t i = fnt::FNT_GLYPH_COUNT; i-- > 0;)
		by_rect[{font.glyphs[i].page, font.glyphs[i].uv.u0, font.glyphs[i].uv.v0, font.glyphs[i].uv.u1}] = i;
	for (const hud::GameFontQuad &quad : run_.quads) {
		const auto found = by_rect.find({quad.page, quad.u0, quad.v0, quad.u1});
		if (found == by_rect.end()) continue;
		const float x0 = std::min(quad.x_top_left, quad.x_bottom_left), x1 = std::max(quad.x_top_right, quad.x_bottom_right);
		glyphs_.push_back({found->second, x0, quad.y_top, x1, quad.y_bottom});
	}
	width_ = std::max(float(measured_w_), run_.width) + 2.0f * kFontPictureMargin;
	height_ = std::max(float(measured_h_), run_.height) + 2.0f * kFontPictureMargin;
}

ViewportAction FontViewport::follow_(const ViewportInput &input, PreviewClock &) {
	const FontDocument *font = font_document(input);
	if (!font || !font->font_row()) {
		const bool had = picture_ != nullptr;
		picture_.reset();
		built_texels_.reset();
		run_ = hud::GameFontRun();
		glyphs_.clear();
		reason_ = FontViewStatus::NoFont;
		detail_.clear();
		shown_none();
		return had ? ViewportAction::Clear : ViewportAction::Keep;
	}
	const bool moved = input.change != ChangeClass::None || !picture_ || options_ != laid_out_;
	if (!moved && reason_ != FontViewStatus::NoFont) {
		shown(*font);
		return ViewportAction::Keep;
	}
	ViewportAction action = ViewportAction::Update;
	if (input.change != ChangeClass::None || !picture_ || reason_ != FontViewStatus::Ready) {
		fnt::fnt_font_t made{};
		std::string why;
		if (!font->font(made, why)) {
			picture_.reset();
			built_texels_.reset();
			run_ = hud::GameFontRun();
			glyphs_.clear();
			reason_ = FontViewStatus::Unwritable;
			detail_ = why;
			shown(*font);
			return ViewportAction::Clear;
		}
		auto picture = std::make_shared<FontPicture>();
		picture->font = made;
		picture->font.pages = nullptr;
		fnt::fnt_free(&made); // the copy's own texels; the picture draws the document's
		picture->texels = font->font_row()->texels;
		const bool texels_moved = picture->texels != built_texels_ || picture->font.num_pages != built_pages_;
		picture_ = std::move(picture);
		if (texels_moved || reason_ != FontViewStatus::Ready) action = ViewportAction::Rebuild;
		built_texels_ = picture_->texels;
		built_pages_ = picture_->font.num_pages;
	}
	reason_ = FontViewStatus::Ready;
	detail_.clear();
	laid_out_ = options_;
	lay_out_();
	shown(*font);
	return action;
}

bool FontViewport::takes_(const std::string &member) const { return member == "options"; }

bool FontViewport::check_(const JsonValue &json, std::string &error) const {
	FontViewportOptions options = options_;
	if (const JsonValue *member = json.get("options"); member && !read_options(*member, options, error)) return false;
	return true;
}

void FontViewport::apply_(const JsonValue &json, PreviewClock &) {
	std::string error;
	if (const JsonValue *member = json.get("options")) read_options(*member, options_, error);
}

} // namespace opennova::editor
