#include "authoring/font_viewport_applier.h"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/rect2.hpp>
#include <godot_cpp/variant/vector2.hpp>

#include <algorithm>
#include <cstring>

#include <editor/preview/font_viewport.h>
#include <editor/preview/viewport_device.h>

#include "authoring/preview_backdrop.h"
#include "hud/font_page_glyphs.h"
#include "util/color_convert.h"

namespace godot {

namespace {

// The picture's own grey on Dark, as the texture's.
const Color kBackdrop(0.16f, 0.16f, 0.16f, 1.0f);

} // namespace

FontViewportApplier::FontViewportApplier(SubViewport &viewport) {
	ColorRect *backdrop = memnew(ColorRect);
	backdrop->set_name("Backdrop");
	backdrop_material_ = make_preview_backdrop_canvas(kBackdrop);
	backdrop->set_material(backdrop_material_);
	backdrop->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	backdrop->set_size(Vector2(1.0f, 1.0f));
	viewport.add_child(backdrop);
	backdrop_id_ = backdrop->get_instance_id();
	// The glyphs' item: every glyph run on it draws through the font page's material (font_page_glyphs: MODULATE2X
	// where the run's UVs carry its flag), each texel sampled nearest so a zoomed texel is a square.
	Control *glyphs = memnew(Control);
	glyphs->set_name("Glyphs");
	glyphs->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	glyphs->set_texture_filter(CanvasItem::TEXTURE_FILTER_NEAREST);
	glyph_shader_.instantiate();
	glyph_shader_->set_code(glyph_canvas_shader_code());
	glyph_material_.instantiate();
	glyph_material_->set_shader(glyph_shader_);
	glyphs->set_material(glyph_material_);
	viewport.add_child(glyphs);
	glyphs_id_ = glyphs->get_instance_id();
}

void FontViewportApplier::rebuild(const opennova::editor::ViewportModel &model, const opennova::editor::SessionView &,
		const opennova::editor::PreviewClock &) {
	const auto &font = static_cast<const opennova::editor::FontViewport &>(model);
	clear();
	picture_ = font.picture();
	if (!picture_) return;
	for (uint32_t p = 0; p < picture_->font.num_pages; ++p) {
		Ref<Texture2D> made;
		if (const uint8_t *texels = picture_->page(p)) {
			PackedByteArray bytes;
			bytes.resize(int64_t(opennova::fnt::FNT_TEXTURE_SIZE));
			std::memcpy(bytes.ptrw(), texels, opennova::fnt::FNT_TEXTURE_SIZE);
			const Ref<Image> image = Image::create_from_data(int32_t(opennova::fnt::FNT_TEXTURE_WIDTH),
					int32_t(opennova::fnt::FNT_TEXTURE_HEIGHT), false, Image::FORMAT_RGBA8, bytes);
			if (image.is_valid()) made = ImageTexture::create_from_image(image);
		}
		pages_.push_back(made);
	}
	drawn_ = false;
}

// An Update: the font's glyph table, header or the options moved, its texels and pages not (a Rebuild takes those
// anew). The picture is the viewport's new one over the same texels: held, so the draw follows it.
void FontViewportApplier::update(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &) {
	const auto &font = static_cast<const opennova::editor::FontViewport &>(model);
	const std::shared_ptr<const opennova::editor::FontPicture> &now = font.picture();
	if (now && picture_ && now->texels == picture_->texels && now->font.num_pages == picture_->font.num_pages) picture_ = now;
	drawn_ = false;
}

void FontViewportApplier::clear() {
	pages_.clear();
	picture_.reset();
	drawn_ = false;
	quads_drawn_ = 0;
	if (Control *glyphs = Object::cast_to<Control>(ObjectDB::get_instance(glyphs_id_)))
		RenderingServer::get_singleton()->canvas_item_clear(glyphs->get_canvas_item());
}

void FontViewportApplier::background(opennova::editor::PreviewBackground background) {
	set_preview_backdrop(**backdrop_material_, background);
}

void FontViewportApplier::draw(const opennova::editor::ViewportModel &model) {
	const auto &font = static_cast<const opennova::editor::FontViewport &>(model);
	Control *glyphs = Object::cast_to<Control>(ObjectDB::get_instance(glyphs_id_));
	if (!glyphs) return;
	RenderingServer *rs = RenderingServer::get_singleton();
	const RID item = glyphs->get_canvas_item();
	rs->canvas_item_clear(item);
	quads_drawn_ = 0;
	drawn_serial_ = font.layout_serial();
	drawn_ = true;
	// The pages held are the picture's: its texels and its page count (a picture over others waits for its Rebuild).
	const std::shared_ptr<const opennova::editor::FontPicture> &now = font.picture();
	if (!picture_ || !now || now->texels != picture_->texels || now->font.num_pages != picture_->font.num_pages) return;
	const float zoom = float(std::max(font.options().zoom, 1));
	const int page = font.shown_page();
	if (page >= 0) {
		// A page as its texels are, its alpha over the background.
		if (size_t(page) < pages_.size() && pages_[size_t(page)].is_valid()) {
			const float side = float(opennova::fnt::FNT_TEXTURE_WIDTH) * zoom;
			const float margin = opennova::editor::kFontPictureMargin * zoom;
			rs->canvas_item_add_texture_rect(item, Rect2(margin, margin, side, side), pages_[size_t(page)]->get_rid());
		}
		return;
	}
	// The text: each run of glyphs on one page drawn with that page, through its stage (the run's UVs flagged
	// where the page's material runs MODULATE2X on the device), at the zoom.
	std::vector<opennova::hud::GameFontQuad> quads = font.run().quads;
	for (opennova::hud::GameFontQuad &q : quads) {
		q.x_top_left *= zoom;
		q.x_top_right *= zoom;
		q.x_bottom_left *= zoom;
		q.x_bottom_right *= zoom;
		q.y_top *= zoom;
		q.y_bottom *= zoom;
	}
	const Vector2 flag = font_page_runs_modulate2x() ? Vector2(0.0f, kGlyphCanvasUvFlag) : Vector2();
	for (size_t begin = 0; begin < quads.size();) {
		size_t end = begin + 1;
		while (end < quads.size() && quads[end].page == quads[begin].page) ++end;
		const uint32_t run_page = quads[begin].page;
		if (run_page < pages_.size() && pages_[run_page].is_valid()) {
			GlyphRunArrays arrays;
			append_glyph_quads(quads, begin, end, flag, arrays);
			rs->canvas_item_add_triangle_array(item, arrays.indices, arrays.points, arrays.colors, arrays.uvs,
					PackedInt32Array(), PackedFloat32Array(), pages_[run_page]->get_rid());
			quads_drawn_ += int(end - begin);
		}
		begin = end;
	}
	// The underline pass: untextured, in the colour the drawer doubles itself.
	for (const opennova::hud::GameFontUnderline &line : font.run().underlines)
		rs->canvas_item_add_line(item, Vector2(line.x0 * zoom, line.y * zoom), Vector2(line.x1 * zoom, line.y * zoom),
				opennova::color_from_argb(line.color), zoom);
}

void FontViewportApplier::apply(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &,
		opennova::editor::ViewportDeviceReport &report) {
	const auto &font = static_cast<const opennova::editor::FontViewport &>(model);
	if (!drawn_ || font.layout_serial() != drawn_serial_) draw(model);
	// What it drew, for the viewport's body (a GUT device test reads it): the glyph quads and the pages it holds.
	report.drawn = opennova::io::JsonValue::make_object();
	report.drawn.set("quads", opennova::io::JsonValue::make_number(double(quads_drawn_)));
	report.drawn.set("pages", opennova::io::JsonValue::make_number(double(pages_.size())));
}

void FontViewportApplier::tick(const opennova::editor::ViewportModel &, const opennova::editor::PreviewClock &) {}

void FontViewportApplier::resize(int width, int height) {
	width_ = std::max(width, 1);
	height_ = std::max(height, 1);
	if (ColorRect *backdrop = Object::cast_to<ColorRect>(ObjectDB::get_instance(backdrop_id_)))
		backdrop->set_size(Vector2(float(width_), float(height_)));
	if (Control *glyphs = Object::cast_to<Control>(ObjectDB::get_instance(glyphs_id_)))
		glyphs->set_size(Vector2(float(width_), float(height_)));
}

} // namespace godot
