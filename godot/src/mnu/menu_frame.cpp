#include "mnu/menu_frame.h"
#include "hud/font_page_glyphs.h"
#include "render/d3d9_raster_device.h"
#include "util/color_convert.h"
#include "mnu/menu_draw_list_stats.h"
#include "util/string_convert.h"

#include <runtime/menu/menu_assets.h>
#include <runtime/menu/menu_state_frame.h>
#include <runtime/menu/options_policy.h>
#include <runtime/renderer/texture_load_rules.h>

#include "mnu/mns_stylesheet.h"
#include "mnu/mnu_document.h"
#include "resource_index/resource_root.h"
#include "rtxt/rtxt_string_file.h"
#include "util/texture_path_resolver.h"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/rect2.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <string>

using namespace godot;
using namespace opennova::fnt;
using opennova::to_std;

// The EDIT_RESULT_* re-exports track menu/menu_edit.h EditKeyResult; pin the
// documented GDScript contract (0 none / 1 changed / 2 commit) so an engine
// enum reorder cannot silently change the bound values.
static_assert(MenuFrame::EDIT_RESULT_NONE == 0);
static_assert(MenuFrame::EDIT_RESULT_CHANGED == 1);
static_assert(MenuFrame::EDIT_RESULT_COMMIT == 2);

namespace {

// The menu items' shader is the shared canvas glyph shader (hud/font_page_glyphs:
// glyph_canvas_shader_code): a glyph run flagged -16 on UV.y draws through its
// font page's material, every other command at texel x vertex colour.

int positive_mod(int value, int divisor) {
	const int result = value % divisor;
	return result < 0 ? result + divisor : result;
}

// A bilinear sample of a WRAP-addressed texture at a UV: the four texels around
// (u * W - 0.5, v * H - 0.5), their indices wrapped.
Color sample_bilinear_wrap(const Ref<Image> &image, float u, float v) {
	const int width = image->get_width();
	const int height = image->get_height();
	const float px = u * static_cast<float>(width) - 0.5f;
	const float py = v * static_cast<float>(height) - 0.5f;
	const float fx0 = std::floor(px);
	const float fy0 = std::floor(py);
	const int x0 = positive_mod(static_cast<int>(fx0), width);
	const int y0 = positive_mod(static_cast<int>(fy0), height);
	const int x1 = (x0 + 1) % width;
	const int y1 = (y0 + 1) % height;
	const float fx = px - fx0;
	const float fy = py - fy0;
	const Color top = image->get_pixel(x0, y0).lerp(
			image->get_pixel(x1, y0), fx);
	const Color bottom = image->get_pixel(x0, y1).lerp(
			image->get_pixel(x1, y1), fx);
	return top.lerp(bottom, fy);
}

} // namespace

MenuFrame::MenuFrame() = default;

MenuFrame::~MenuFrame() {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (overlay_canvas_item_.is_valid()) {
		rs->free_rid(overlay_canvas_item_);
	}
	if (slot_canvas_item_.is_valid()) {
		rs->free_rid(slot_canvas_item_);
	}
	if (overlay_upper_canvas_item_.is_valid()) {
		rs->free_rid(overlay_upper_canvas_item_);
	}
}

void MenuFrame::ensure_overlay_canvas_item_() {
	if (overlay_canvas_item_.is_valid()) {
		return;
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	overlay_canvas_item_ = rs->canvas_item_create();
	rs->canvas_item_set_parent(overlay_canvas_item_, get_canvas_item());
	// One z above the frame and every sibling child it draws in front of.
	rs->canvas_item_set_z_as_relative_to_parent(overlay_canvas_item_, true);
	rs->canvas_item_set_z_index(overlay_canvas_item_, 1);
	slot_canvas_item_ = rs->canvas_item_create();
	rs->canvas_item_set_parent(slot_canvas_item_, get_canvas_item());
	rs->canvas_item_set_z_as_relative_to_parent(slot_canvas_item_, true);
	rs->canvas_item_set_z_index(slot_canvas_item_, 2);
	rs->canvas_item_set_visible(slot_canvas_item_, false);
	overlay_upper_canvas_item_ = rs->canvas_item_create();
	rs->canvas_item_set_parent(overlay_upper_canvas_item_, get_canvas_item());
	rs->canvas_item_set_z_as_relative_to_parent(overlay_upper_canvas_item_, true);
	rs->canvas_item_set_z_index(overlay_upper_canvas_item_, 3);
	// Every item the draw list paints on runs the font pages' material on its
	// glyph runs (the slot item is its companion's, which binds its own).
	glyph_shader_.instantiate();
	glyph_shader_->set_code(glyph_canvas_shader_code());
	glyph_material_.instantiate();
	glyph_material_->set_shader(glyph_shader_);
	for (const RID &item : { get_canvas_item(), overlay_canvas_item_, overlay_upper_canvas_item_ }) {
		rs->canvas_item_set_material(item, glyph_material_->get_rid());
	}
}

String MenuFrame::glyph_shader_code() {
	return String(glyph_canvas_shader_code());
}

Array MenuFrame::get_glyph_submissions() {
	Array out;
	if (!configured_ || !is_inside_tree()) {
		return out;
	}
	glyph_record_ = &out;
	_draw();
	glyph_record_ = nullptr;
	queue_redraw();
	return out;
}

void MenuFrame::set_custom_slot_widget(int p_index) {
	if (state_.custom_slot_index == p_index) return;
	state_.custom_slot_index = p_index;
	queue_redraw();
}

RID MenuFrame::get_custom_slot_canvas_item() {
	ensure_overlay_canvas_item_();
	return slot_canvas_item_;
}

// One menu texture file decoded by the reader the engine's extension dispatch picked
// (renderer::TextureLoader::Menu's: the game's TGA reader, D3DX's content sniff for a
// DDS, the PCX reader, Godot's PNG decoder), the retail way (decode_texture_load); the
// dispatch and its witness live at the engine home, menu_assets.h.
bool MenuFrameTextures::decode(const std::string &p_key, opennova::menu::MenuTextureFormat p_format,
		const std::vector<uint8_t> &p_bytes, int &r_width, int &r_height) {
	if (p_bytes.empty()) {
		return false;
	}
	opennova::renderer::TextureLoad load;
	switch (p_format) {
		case opennova::menu::MenuTextureFormat::Tga:
			load.reader = opennova::renderer::TextureReader::Tga;
			break;
		case opennova::menu::MenuTextureFormat::Dds:
			load.reader = opennova::renderer::TextureReader::Dds;
			break;
		case opennova::menu::MenuTextureFormat::Png:
			load.reader = opennova::renderer::TextureReader::Png;
			break;
		case opennova::menu::MenuTextureFormat::Pcx:
			load.reader = opennova::renderer::TextureReader::Pcx;
			break;
		case opennova::menu::MenuTextureFormat::None:
			return false;
	}
	PackedByteArray bytes;
	bytes.resize(static_cast<int64_t>(p_bytes.size()));
	std::memcpy(bytes.ptrw(), p_bytes.data(), p_bytes.size());
	Ref<Image> image = opennova::decode_texture_load(load, bytes);
	if (image.is_null() || image->is_empty()) {
		return false;
	}
	// A menu image draws its top level alone.
	if (image->has_mipmaps()) {
		image->clear_mipmaps();
	}
	Entry entry;
	entry.image = image;
	entry.texture = ImageTexture::create_from_image(image);
	if (entry.texture.is_null()) {
		return false;
	}
	// A menu image draws from its pixels in a power-of-two texture, transparent black
	// past them (engine menu_image_texture_side): a stretched image's last column and
	// row blend with that black as the game's do.
	const int side_w = opennova::menu::menu_image_texture_side(image->get_width());
	const int side_h = opennova::menu::menu_image_texture_side(image->get_height());
	entry.drawn = entry.texture;
	if (side_w != image->get_width() || side_h != image->get_height()) {
		Ref<Image> padded = Image::create(side_w, side_h, false, Image::FORMAT_RGBA8);
		padded->fill(Color(0.0f, 0.0f, 0.0f, 0.0f));
		Ref<Image> source = image;
		if (source->get_format() != Image::FORMAT_RGBA8) {
			source = image->duplicate();
			if (source->is_compressed()) {
				source->decompress();
			}
			source->convert(Image::FORMAT_RGBA8);
		}
		padded->blit_rect(source, Rect2i(0, 0, image->get_width(), image->get_height()),
				Vector2i(0, 0));
		entry.drawn = ImageTexture::create_from_image(padded);
	}
	r_width = image->get_width();
	r_height = image->get_height();
	entries_[p_key] = entry;
	return true;
}

void MenuFrameTextures::release(const std::string &p_key) {
	entries_.erase(p_key);
}

const MenuFrameTextures::Entry *MenuFrameTextures::find(const std::string &p_key) const {
	const auto found = entries_.find(p_key);
	return found != entries_.end() ? &found->second : nullptr;
}

// A font's pages uploaded on its first draw and kept while the loader keeps the font.
const std::vector<Ref<Texture2D>> *MenuFrame::font_pages_(int32_t p_slot) {
	const opennova::menu::MenuFont *font = assets_.font_for_slot(compiler_, p_slot);
	if (font == nullptr || !font->valid) {
		return nullptr;
	}
	auto found = font_pages_by_serial_.find(font->serial);
	if (found == font_pages_by_serial_.end()) {
		std::vector<Ref<Texture2D>> pages(font->font.num_pages);
		for (uint32_t page = 0; page < font->font.num_pages && page < FNT_MAX_PAGES; ++page) {
			pages[page] = font_page_texture(font->font, page);
		}
		found = font_pages_by_serial_.emplace(font->serial, std::move(pages)).first;
	}
	return &found->second;
}

bool MenuFrame::texture_loads(const String &p_name) {
	return files_ != nullptr && !p_name.is_empty() &&
			assets_.texture_loads(opennova::to_std(p_name), *files_, texture_store_);
}

bool MenuFrame::load_texture_ahead(const std::string &p_name, const opennova::FileSource &p_files) {
	return assets_.texture_loads(p_name, p_files, texture_store_);
}

bool MenuFrame::configure(const Ref<MnuDocument> &p_document,
		const String &p_screen_name, const Ref<ResourceRoot> &p_root,
		const Ref<MnsStyleSheet> &p_style, const Ref<RtxtStringFile> &p_override_text) {
	document_ = p_document;
	override_text_ = p_override_text;
	root_files_.set_root(p_root);
	if (document_.is_null()) {
		clear_screen();
		return false;
	}
	const opennova::mnu::Document &doc = document_->get_native();
	const opennova::mnu::Screen *screen = p_screen_name.is_empty()
			? doc.first_screen()
			: doc.find_screen(opennova::to_std(p_screen_name));
	// The stylesheet vars feed the compiler's resolution.
	std::map<std::string, std::string> vars;
	if (p_style.is_valid()) {
		for (const auto &kv : p_style->variables()) {
			vars[kv.first] = kv.second;
		}
	}
	return configure_screen(&doc, screen, root_files_, vars,
			override_text_.is_valid() ? &override_text_->get_native() : nullptr);
}

// What the screen reads, through the engine's loader (the witnesses live at the engine
// homes, menu_screen_inputs.h and menu_frame_assets.h): the string tables its windows
// name, every FONT (no default font: a widget whose FONT did not load draws with its
// ancestors'), and every texture it interned, each kept by (file, stamp) so a configure
// that names the same unchanged files reads nothing again.
bool MenuFrame::configure_screen(const opennova::mnu::Document *p_document,
		const opennova::mnu::Screen *p_screen, const opennova::FileSource &p_files,
		const std::map<std::string, std::string> &p_vars,
		const opennova::rtxt::File *p_override_text) {
	files_ = &p_files;
	configured_ = false;
	state_ = opennova::menu::MenuFrameState{};
	cursor_slot_ = -1;
	click_.reset();
	++configure_count_;
	if (p_document == nullptr) {
		clear_screen();
		return false;
	}
	assets_.configure(compiler_, p_document, p_screen, p_files, texture_store_, p_vars,
			p_override_text);
	adopt_slots_();
	configured_ = p_screen != nullptr;
	queue_redraw();
	return configured_;
}

void MenuFrame::clear_screen() {
	configured_ = false;
	state_ = opennova::menu::MenuFrameState{};
	cursor_slot_ = -1;
	click_.reset();
	++configure_count_;
	assets_.clear(compiler_, texture_store_);
	files_ = nullptr;
	adopt_slots_();
	queue_redraw();
}

// The configured screen's texture slots from what the loader kept, and the composed
// frames and font pages of what it no longer keeps let go.
void MenuFrame::adopt_slots_() {
	texture_keys_ = assets_.slot_keys();
	textures_.assign(texture_keys_.size(), Ref<Texture2D>());
	drawn_textures_.assign(texture_keys_.size(), Ref<Texture2D>());
	texture_images_.assign(texture_keys_.size(), Ref<Image>());
	for (size_t i = 0; i < texture_keys_.size(); ++i) {
		if (const MenuFrameTextures::Entry *entry = texture_store_.find(texture_keys_[i])) {
			textures_[i] = entry->texture;
			drawn_textures_[i] = entry->drawn;
			texture_images_[i] = entry->image;
		}
	}
	for (auto it = frame_texture_cache_.begin(); it != frame_texture_cache_.end();) {
		if (it->second.used + 1 < configure_count_) {
			it = frame_texture_cache_.erase(it);
		} else {
			++it;
		}
	}
	for (auto it = font_pages_by_serial_.begin(); it != font_pages_by_serial_.end();) {
		if (assets_.font_alive(it->first)) {
			++it;
		} else {
			it = font_pages_by_serial_.erase(it);
		}
	}
}

void MenuFrame::reset_loads() {
	compiler_.reset_texture_loads();
}

bool MenuFrame::is_configured() const {
	return configured_;
}

Ref<Texture2D> MenuFrame::cached_frame_texture_(const std::string &p_key) const {
	const auto found = frame_texture_cache_.find(p_key);
	if (found == frame_texture_cache_.end()) {
		return Ref<Texture2D>();
	}
	found->second.used = configure_count_;
	return found->second.texture;
}

void MenuFrame::keep_frame_texture_(const std::string &p_key, const Ref<Texture2D> &p_texture) {
	frame_texture_cache_[p_key] = FrameTexture{ p_texture, configure_count_ };
}

Ref<Texture2D> MenuFrame::texture_for_quad_(
		const opennova::menu::MenuQuad &p_quad) {
	const auto valid_slot = [this](int32_t slot) {
		return slot >= 0 && slot < static_cast<int32_t>(textures_.size()) &&
				textures_[static_cast<size_t>(slot)].is_valid() &&
				texture_images_[static_cast<size_t>(slot)].is_valid();
	};
	if (!valid_slot(p_quad.texture)) {
		return Ref<Texture2D>();
	}
	const bool has_second = p_quad.texture2 != opennova::menu::kMenuTexNone;
	if (has_second && !valid_slot(p_quad.texture2)) {
		return Ref<Texture2D>();
	}
	if (!has_second && !p_quad.tiled) {
		// A menu image: its power-of-two texture (MenuFrameTextures::Entry::drawn).
		const Ref<Texture2D> &drawn = drawn_textures_[static_cast<size_t>(p_quad.texture)];
		return drawn.is_valid() ? drawn : textures_[static_cast<size_t>(p_quad.texture)];
	}

	const Ref<Image> first = texture_images_[static_cast<size_t>(p_quad.texture)];
	if (has_second) {
		return frame_piece_texture_(p_quad, first,
				texture_images_[static_cast<size_t>(p_quad.texture2)]);
	}
	const int first_w = first->get_width();
	const int first_h = first->get_height();
	const int src_x0 = std::clamp(
			static_cast<int>(std::lround(p_quad.u0 * first_w)), 0, first_w);
	const int src_y0 = std::clamp(
			static_cast<int>(std::lround(p_quad.v0 * first_h)), 0, first_h);
	const int src_x1 = std::clamp(
			static_cast<int>(std::lround(p_quad.u1 * first_w)), src_x0, first_w);
	const int src_y1 = std::clamp(
			static_cast<int>(std::lround(p_quad.v1 * first_h)), src_y0, first_h);
	const int src_w = src_x1 - src_x0;
	const int src_h = src_y1 - src_y0;
	if (src_w <= 0 || src_h <= 0) {
		return Ref<Texture2D>();
	}

	if (p_quad.tiled) {
		// CUIElement_DrawTexturedQuadFromRect feeds absolute device coordinates as UVs,
		// so the native-pixel fill pattern is screen-aligned rather than
		// restarting at each window's top-left.
		const int phase_x = positive_mod(
				static_cast<int>(std::floor(p_quad.x0)), src_w);
		const int phase_y = positive_mod(
				static_cast<int>(std::floor(p_quad.y0)), src_h);
		// Keyed by the texture's kept load (its file and stamp), so a tile survives a
		// reconfigure that keeps the texture.
		const std::string key = "tile:" + texture_keys_[static_cast<size_t>(p_quad.texture)] + ":" +
				std::to_string(src_x0) + ":" + std::to_string(src_y0) + ":" +
				std::to_string(src_w) + ":" + std::to_string(src_h) + ":" +
				std::to_string(phase_x) + ":" + std::to_string(phase_y);
		const Ref<Texture2D> cached = cached_frame_texture_(key);
		if (cached.is_valid()) {
			return cached;
		}
		Ref<Image> tile = Image::create(src_w, src_h, false, Image::FORMAT_RGBA8);
		for (int y = 0; y < src_h; ++y) {
			for (int x = 0; x < src_w; ++x) {
				tile->set_pixel(x, y,
						first->get_pixel(src_x0 + (phase_x + x) % src_w,
								src_y0 + (phase_y + y) % src_h));
			}
		}
		const Ref<Texture2D> texture = ImageTexture::create_from_image(tile);
		keep_frame_texture_(key, texture);
		return texture;
	}
	return Ref<Texture2D>();
}

// A frame border piece as Direct3D 9 rasterises it (the MenuQuad contract,
// engine menu/menu_frame.h): one texel per pixel it covers
// (renderer::d3d9_quad_pixels), each at the window coordinate of its centre: the
// stencil at the UV that centre interpolates to, the brush screen-anchored at
// ((x + 0.5) / period_x, (y + 0.5) / period_y), both wrapped and bilinear as the
// game's textures are (created WRAP, LINEAR: flags 0x140000 and 0x40000), then the
// two MODULATE2X stages over the quad's diffuse, each saturated, the alpha the
// product. Drawn 1:1 over the pixels it covers, the image is what the game's
// rasteriser writes there.
Ref<Texture2D> MenuFrame::frame_piece_texture_(const opennova::menu::MenuQuad &p_quad,
		const Ref<Image> &p_stencil, const Ref<Image> &p_brush) {
	const opennova::renderer::PixelRect px =
			opennova::renderer::d3d9_quad_pixels(p_quad.x0, p_quad.y0, p_quad.x1, p_quad.y1);
	const int dst_w = px.x1 - px.x0;
	const int dst_h = px.y1 - px.y0;
	if (dst_w <= 0 || dst_h <= 0 || p_quad.texture2_period_x <= 0.0f ||
			p_quad.texture2_period_y <= 0.0f || p_quad.x1 <= p_quad.x0 || p_quad.y1 <= p_quad.y0) {
		return Ref<Texture2D>();
	}
	const auto bits = [](float value) {
		uint32_t out = 0;
		std::memcpy(&out, &value, sizeof(out));
		return std::to_string(out);
	};
	const std::string key = "piece:" + texture_keys_[static_cast<size_t>(p_quad.texture)] + ":" +
			texture_keys_[static_cast<size_t>(p_quad.texture2)] + ":" + bits(p_quad.x0) + ":" +
			bits(p_quad.y0) + ":" + bits(p_quad.x1) + ":" + bits(p_quad.y1) + ":" + bits(p_quad.u0) +
			":" + bits(p_quad.v0) + ":" + bits(p_quad.u1) + ":" + bits(p_quad.v1) + ":" +
			bits(p_quad.texture2_period_x) + ":" + bits(p_quad.texture2_period_y) + ":" +
			std::to_string(p_quad.color);
	const Ref<Texture2D> cached = cached_frame_texture_(key);
	if (cached.is_valid()) {
		return cached;
	}
	const Color diffuse = opennova::color_from_argb(p_quad.color);
	Ref<Image> composed = Image::create(dst_w, dst_h, false, Image::FORMAT_RGBA8);
	const float span_x = p_quad.x1 - p_quad.x0;
	const float span_y = p_quad.y1 - p_quad.y0;
	for (int y = 0; y < dst_h; ++y) {
		const float window_y = static_cast<float>(px.y0 + y);
		const float v = p_quad.v0 + (p_quad.v1 - p_quad.v0) * (window_y - p_quad.y0) / span_y;
		const float brush_v = (window_y + 0.5f) / p_quad.texture2_period_y;
		for (int x = 0; x < dst_w; ++x) {
			const float window_x = static_cast<float>(px.x0 + x);
			const float u = p_quad.u0 + (p_quad.u1 - p_quad.u0) * (window_x - p_quad.x0) / span_x;
			const Color stencil = sample_bilinear_wrap(p_stencil, u, v);
			const Color brush = sample_bilinear_wrap(p_brush,
					(window_x + 0.5f) / p_quad.texture2_period_x, brush_v);
			const float r0 = std::min(1.0f, 2.0f * stencil.r * diffuse.r);
			const float g0 = std::min(1.0f, 2.0f * stencil.g * diffuse.g);
			const float b0 = std::min(1.0f, 2.0f * stencil.b * diffuse.b);
			composed->set_pixel(x, y,
					Color(std::min(1.0f, 2.0f * r0 * brush.r), std::min(1.0f, 2.0f * g0 * brush.g),
							std::min(1.0f, 2.0f * b0 * brush.b), stencil.a * diffuse.a * brush.a));
		}
	}
	const Ref<Texture2D> texture = ImageTexture::create_from_image(composed);
	keep_frame_texture_(key, texture);
	return texture;
}

opennova::menu::MenuWidgetState &MenuFrame::widget_(int p_index) {
	return opennova::menu::frame_widget(state_, p_index);
}

// The state writes are the engine's (menu_state_frame.h, the headless frame's too): this device
// queues the redraw of what they left.
void MenuFrame::set_widget_shown_override(int p_index, bool p_shown) {
	opennova::menu::frame_set_shown(state_, p_index, p_shown);
	queue_redraw();
}

void MenuFrame::set_native_state(const opennova::menu::MenuFrameState &p_state) {
	state_ = p_state;
	queue_redraw();
}

void MenuFrame::set_widget_disabled(int p_index, bool p_disabled) {
	opennova::menu::frame_set_disabled(state_, p_index, p_disabled);
	queue_redraw();
}

void MenuFrame::set_widget_checked(int p_index, bool p_checked) {
	opennova::menu::frame_set_checked(state_, p_index, p_checked);
	queue_redraw();
}

void MenuFrame::set_widget_focused(int p_index, bool p_focused) {
	opennova::menu::frame_set_focused(state_, p_index, p_focused);
	queue_redraw();
}

void MenuFrame::set_widget_caret(int p_index, int p_caret) {
	opennova::menu::frame_set_caret(state_, p_index, p_caret);
	queue_redraw();
}

void MenuFrame::set_widget_text(int p_index, const String &p_text) {
	opennova::menu::frame_set_text(state_, p_index, opennova::to_std(p_text));
	queue_redraw();
}

void MenuFrame::set_widget_rect(int p_index, const Rect2i &p_rect) {
	opennova::menu::frame_set_rect(state_, p_index, p_rect.position.x, p_rect.position.y,
			p_rect.position.x + p_rect.size.x, p_rect.position.y + p_rect.size.y);
	queue_redraw();
}

void MenuFrame::set_widget_hover_item(int p_index, int p_row) {
	if (opennova::menu::frame_set_hover_item(state_, p_index, p_row)) {
		queue_redraw();
	}
}

int MenuFrame::get_widget_hover_item(int p_index) const {
	const opennova::menu::MenuWidgetState *ws = opennova::menu::find_frame_widget(state_, p_index);
	return ws != nullptr ? ws->hover_item : -1;
}

void MenuFrame::set_widget_selection(int p_index, int p_selected_item,
		int p_hover_item, int p_scroll_row) {
	opennova::menu::frame_set_selection(state_, p_index, p_selected_item, p_hover_item, p_scroll_row);
	queue_redraw();
}

void MenuFrame::set_widget_scroll_range(int p_index, int p_minimum,
		int p_maximum, int p_page,
		int p_value) {
	opennova::menu::frame_set_scroll_range(state_, p_index, p_minimum, p_maximum, p_page, p_value);
	queue_redraw();
}

void MenuFrame::set_widget_popup_open(int p_index, bool p_open) {
	opennova::menu::frame_set_popup_open(state_, p_index, p_open);
	queue_redraw();
}

void MenuFrame::set_widget_items(int p_index,
		const PackedStringArray &p_items) {
	std::vector<std::string> items;
	items.reserve(static_cast<size_t>(p_items.size()));
	for (int64_t i = 0; i < p_items.size(); ++i) {
		items.push_back(opennova::to_std(p_items[i]));
	}
	opennova::menu::frame_set_items(state_, p_index, items);
	queue_redraw();
}

void MenuFrame::set_widget_selected_set(int p_index,
		const PackedInt32Array &p_rows) {
	std::vector<int> rows;
	rows.reserve(static_cast<size_t>(p_rows.size()));
	for (int64_t i = 0; i < p_rows.size(); ++i) {
		rows.push_back(p_rows[i]);
	}
	opennova::menu::frame_set_selected_set(state_, p_index, rows);
	queue_redraw();
}

void MenuFrame::set_widget_table_rows(int p_index,
		const std::vector<opennova::menu::MenuTableRow> &p_rows) {
	opennova::menu::frame_set_table_rows(state_, p_index, p_rows);
	queue_redraw();
}

void MenuFrame::set_widget_table_cells(int p_index, const Array &p_rows) {
	std::vector<opennova::menu::MenuTableRow> rows;
	rows.reserve(static_cast<size_t>(p_rows.size()));
	for (int64_t i = 0; i < p_rows.size(); ++i) {
		const PackedStringArray cells = p_rows[i];
		opennova::menu::MenuTableRow row;
		for (int64_t c = 0; c < cells.size(); ++c) {
			row.cells.push_back(opennova::to_std(cells[c]));
		}
		row.values.push_back(0);
		rows.push_back(std::move(row));
	}
	set_widget_table_rows(p_index, rows);
}

void MenuFrame::set_table_cell_painter(int p_index,
		opennova::menu::MenuTableCellPainter p_painter) {
	compiler_.set_table_cell_painter(p_index, std::move(p_painter));
	queue_redraw();
}

void MenuFrame::set_widget_clip_rect(int p_index, bool p_enabled, const Rect2i &p_rect) {
	opennova::menu::frame_set_clip_rect(state_, p_index, p_enabled, p_rect.position.x, p_rect.position.y,
			p_rect.position.x + p_rect.size.x, p_rect.position.y + p_rect.size.y);
	queue_redraw();
}

void MenuFrame::set_widget_table_columns(int p_index, bool p_installed,
		const std::vector<opennova::menu::MenuTableColumn> &p_columns, int p_sort_column) {
	opennova::menu::frame_set_table_columns(state_, p_index, p_installed, p_columns, p_sort_column);
	queue_redraw();
}

void MenuFrame::set_widget_marquee(int p_index,
		const opennova::menu::MarqueeCredits &p_credits) {
	// The node fonts load the way a FONT does (menu_credits.h).
	for (const opennova::menu::MarqueeCreditNode &node : p_credits.nodes) {
		if (node.text && !node.font.empty() && files_ != nullptr) {
			assets_.add_font(compiler_, node.font, *files_);
		}
	}
	opennova::menu::MenuWidgetState &ws = widget_(p_index);
	ws.marquee = p_credits;
	ws.marquee_reset = true; // fresh content restarts the roll
	queue_redraw();
}

int MenuFrame::widget_count() const {
	return compiler_.widget_count();
}

String MenuFrame::widget_name(int p_index) const {
	return opennova::to_gd(compiler_.widget_name(p_index));
}

int MenuFrame::widget_kind(int p_index) const {
	return compiler_.widget_kind(p_index);
}

String MenuFrame::widget_authored_text(int p_index) const {
	return opennova::cp1252_to_gd(compiler_.widget_authored_text(p_index));
}

bool MenuFrame::is_widget_disabled(int p_index) const {
	return compiler_.widget_disabled(p_index, state_);
}

bool MenuFrame::is_widget_shown(int p_index) const {
	return compiler_.widget_shown(p_index, state_);
}

Rect2 MenuFrame::widget_rect(int p_index) const {
	opennova::mnu::RectEdges rect;
	if (!compiler_.widget_rect(p_index, state_, &rect)) {
		return Rect2();
	}
	return Rect2(static_cast<float>(rect.left), static_cast<float>(rect.top),
			static_cast<float>(rect.right - rect.left),
			static_cast<float>(rect.bottom - rect.top));
}

void MenuFrame::set_mount_widget(int p_index) {
	if (state_.mount_index == p_index) return;
	state_.mount_index = p_index;
	queue_redraw();
}

Rect2 MenuFrame::widget_local_rect(int p_index) const {
	opennova::mnu::RectEdges rect;
	if (!compiler_.widget_local_rect(p_index, state_, &rect)) return Rect2();
	return Rect2(static_cast<float>(rect.left), static_cast<float>(rect.top),
			static_cast<float>(rect.right - rect.left), static_cast<float>(rect.bottom - rect.top));
}

int MenuFrame::item_count(int p_index) const {
	return compiler_.item_count(p_index, state_);
}

String MenuFrame::get_widget_text(int p_index) const {
	const opennova::menu::MenuWidgetState *ws = opennova::menu::find_frame_widget(state_, p_index);
	if (ws != nullptr && ws->has_text) {
		return opennova::cp1252_to_gd(ws->text);
	}
	return widget_authored_text(p_index);
}

int MenuFrame::get_widget_caret(int p_index) const {
	return opennova::menu::frame_widget_caret(state_, p_index);
}

int MenuFrame::hit_test(const Vector2 &p_position) const {
	if (!configured_) {
		return -1;
	}
	const Vector2 scale = design_scale_();
	return compiler_.hit_widget(state_, p_position.x, p_position.y, scale.x,
			scale.y);
}

int MenuFrame::list_row_at(int p_index, const Vector2 &p_position) const {
	if (!configured_) {
		return -1;
	}
	const Vector2 scale = design_scale_();
	return compiler_.list_row_at(p_index, state_, p_position.x, p_position.y,
			scale.x, scale.y);
}

bool MenuFrame::combo_popup_contains(int p_index,
		const Vector2 &p_position) const {
	if (!configured_) {
		return false;
	}
	const Vector2 scale = design_scale_();
	return compiler_.combo_popup_contains(p_index, state_, p_position.x,
			p_position.y, scale.x, scale.y);
}

int MenuFrame::combo_popup_row_at(int p_index,
		const Vector2 &p_position) const {
	if (!configured_) {
		return -1;
	}
	const Vector2 scale = design_scale_();
	return compiler_.combo_popup_row_at(p_index, state_, p_position.x,
			p_position.y, scale.x, scale.y);
}

int MenuFrame::scroll_hit_at(int p_index, const Vector2 &p_position) const {
	if (!configured_) {
		return 0;
	}
	const Vector2 scale = design_scale_();
	return compiler_.scroll_hit_at(p_index, state_, p_position.x, p_position.y,
			scale.x, scale.y);
}

int MenuFrame::spin_arrow_at(int p_index, const Vector2 &p_position) const {
	if (!configured_) {
		return 0;
	}
	const Vector2 scale = design_scale_();
	return compiler_.spin_arrow_at(p_index, state_, p_position.x, p_position.y,
			scale.x, scale.y);
}

std::string MenuFrame::item_display_text(int p_index, int p_row) const {
	if (!configured_) {
		return std::string();
	}
	return compiler_.item_display_text(p_index, state_, p_row);
}

bool MenuFrame::table_hit(int p_index, const Vector2 &p_position, int *r_row,
		int *r_column) const {
	*r_row = -1;
	*r_column = -1;
	if (!configured_) {
		return false;
	}
	const Vector2 scale = design_scale_();
	return compiler_.table_hit(p_index, state_, p_position.x, p_position.y, scale.x, scale.y,
			r_row, r_column);
}

std::string MenuFrame::widget_mnemonic(int p_index) const {
	return configured_ ? compiler_.widget_mnemonic(p_index) : std::string();
}

std::string MenuFrame::widget_string(int p_index, const std::string &p_key) const {
	return configured_ ? compiler_.widget_string(p_index, p_key) : p_key;
}

void MenuFrame::set_open_popup(int p_index) {
	state_.popup_root = p_index;
}

bool MenuFrame::edit_char(int p_index, int p_unicode) {
	if (!configured_) {
		return false;
	}
	// The router's printable filter and the ops both live in engine menu/menu_edit.h (the
	// witnesses ride the engine header), the field's state in menu_state_frame.h.
	const bool changed = opennova::menu::frame_edit_char(compiler_, state_, p_index, p_unicode);
	queue_redraw();
	return changed;
}

int MenuFrame::edit_key(int p_index, int p_key, bool p_shift) {
	if (!configured_) {
		return 0;
	}
	const opennova::menu::EditKeyResult result =
			opennova::menu::frame_edit_key(compiler_, state_, p_index, p_key, p_shift);
	queue_redraw();
	return static_cast<int>(result); // the pinned EDIT_RESULT_* contract
}

Ref<Texture2D> MenuFrame::get_cursor_texture() const {
	if (cursor_slot_ >= 0 &&
			cursor_slot_ < static_cast<int32_t>(textures_.size())) {
		return textures_[static_cast<size_t>(cursor_slot_)];
	}
	return Ref<Texture2D>();
}

int MenuFrame::get_unresolved_asset_count() const {
	return static_cast<int>(assets_.unresolved().size());
}

void MenuFrame::set_time_ms(int64_t p_ms) {
	state_.time_ms = static_cast<uint32_t>(p_ms);
	queue_redraw();
}

int MenuFrame::widget_index(const String &p_name) const {
	return compiler_.widget_index(p_name.utf8().get_data());
}

std::vector<opennova::menu::MenuPumpWindow> MenuFrame::press_mouse(const Vector2 &p_position) {
	if (!is_configured()) {
		return {};
	}
	const Vector2 scale = design_scale_();
	const std::vector<opennova::menu::MenuPumpWindow> reach = compiler_.press_mouse(
			click_, state_, p_position.x, p_position.y, scale.x, scale.y);
	for (const opennova::menu::MenuPumpWindow &window : reach) {
		opennova::menu::MenuFrameCompiler::MouseClaim changed;
		compiler_.press_scroll_window(state_, window, p_position.x, p_position.y, scale.x,
				scale.y, &changed);
		if (changed.scroll_value_changed) {
			emit_signal("scroll_value_changed", changed.scroll_index, changed.scroll_value);
			queue_redraw();
		}
	}
	return reach;
}

int MenuFrame::process_mouse(const Vector2 &p_position, bool p_button_down) {
	if (!is_configured()) {
		return -1;
	}
	const Vector2 scale = design_scale_();
	// The claim honors a press's capture, which the release lets go first; then
	// the click: the claim let go over that was held the sample before
	// (engine menu_click.h, D-MNU-30), a scrollbar window's the scrollbar's own.
	opennova::menu::MenuFrameCompiler::MouseSample sample = compiler_.sample_mouse(
			click_, state_, p_position.x, p_position.y, p_button_down, scale.x, scale.y);
	opennova::menu::MenuFrameCompiler::MouseClaim &claim = sample.claim;
	cursor_slot_ = claim.cursor;
	const opennova::menu::MenuPumpWindow clicked = sample.clicked;
	if (clicked.valid() && !compiler_.click_scroll_window(state_, clicked, &claim)) {
		emit_signal("widget_clicked", clicked.index, clicked.part);
	}
	if (claim.scroll_value_changed) {
		emit_signal("scroll_value_changed", claim.scroll_index,
				claim.scroll_value);
	}
	queue_redraw();
	return claim.hovered;
}

bool MenuFrame::process_popup_mouse(int p_index, const Vector2 &p_position,
		bool p_button_down) {
	if (!is_configured()) {
		return false;
	}
	const Vector2 scale = design_scale_();
	// The dropdown has the mouse: a press it takes holds the capture until the
	// release (engine MenuClickLatch::dropdown_sample).
	click_.dropdown_sample(p_index, p_button_down);
	const opennova::menu::MenuFrameCompiler::MouseClaim claim =
			compiler_.pump_popup_mouse(state_, p_index, p_position.x,
					p_position.y, p_button_down, scale.x, scale.y);
	state_.cursor_x = p_position.x;
	state_.cursor_y = p_position.y;
	if (claim.scroll_value_changed) {
		// The popup row window moved: the new first-visible row.
		emit_signal("scroll_value_changed", claim.scroll_index,
				claim.scroll_value);
	}
	if (claim.scroll_index >= 0) {
		queue_redraw();
	}
	return claim.scroll_index >= 0;
}

bool MenuFrame::process_mouse_wheel(const Vector2 &p_position, int p_steps) {
	if (!is_configured()) {
		return false;
	}
	const Vector2 scale = design_scale_();
	opennova::menu::MenuFrameCompiler::MouseClaim claim;
	if (!compiler_.pump_mouse_wheel(state_, p_position.x, p_position.y,
				p_steps, scale.x, scale.y, &claim)) {
		return false;
	}
	if (claim.scroll_value_changed) {
		// The row window moved: the new first-visible row.
		emit_signal("scroll_value_changed", claim.scroll_index,
				claim.scroll_value);
		queue_redraw();
	}
	return true;
}

void MenuFrame::set_cursor_state(bool p_visible, const Vector2 &p_position) {
	state_.cursor_visible = p_visible;
	state_.cursor_x = p_position.x;
	state_.cursor_y = p_position.y;
	queue_redraw();
}

void MenuFrame::place_cursor(bool p_visible, const Vector2 &p_position) {
	// The claim the pump would make there, stamped with no hover or press written
	// (engine MenuFrameCompiler::claim_at): the picture's windows keep the states
	// they are held in.
	int32_t claim = -1;
	int32_t spin_part = 0;
	if (p_visible && is_configured()) {
		const Vector2 scale = design_scale_();
		const opennova::menu::MenuFrameCompiler::MouseClaim at = compiler_.claim_at(
				state_, p_position.x, p_position.y, scale.x, scale.y, click_.captured());
		claim = at.stamp_index();
		spin_part = at.stamp_part();
	}
	if (state_.cursor_visible == p_visible && state_.cursor_x == p_position.x &&
			state_.cursor_y == p_position.y && state_.cursor_claim == claim &&
			state_.cursor_spin_part == spin_part) {
		return;
	}
	state_.cursor_visible = p_visible;
	state_.cursor_x = p_position.x;
	state_.cursor_y = p_position.y;
	state_.cursor_claim = claim;
	state_.cursor_spin_part = spin_part;
	queue_redraw();
}

Vector2 MenuFrame::design_scale_() const {
	const Vector2 size = get_size();
	if (size.x > 1.0f && size.y > 1.0f) {
		// The scale pair and its witness live at menu/menu_frame.h.
		return Vector2(opennova::menu::menu_scale_x(size.x), opennova::menu::menu_scale_y(size.y));
	}
	return Vector2(1.0f, 1.0f);
}

Ref<MenuDrawListStats> MenuFrame::get_draw_list_stats() {
	Ref<MenuDrawListStats> out;
	out.instantiate();
	int64_t quads = 0;
	int64_t quads_textured = 0;
	int64_t lines = 0;
	int64_t glyphs = 0;
	int64_t widgets = 0;
	if (configured_) {
		const Vector2 scale = design_scale_();
		const opennova::menu::MenuDrawList &list =
				compiler_.compile(state_, scale.x, scale.y);
		quads = static_cast<int64_t>(list.quads.size());
		for (const opennova::menu::MenuQuad &quad : list.quads) {
			if (quad.texture != opennova::menu::kMenuTexNone) {
				++quads_textured;
			}
		}
		lines = static_cast<int64_t>(list.lines.size());
		glyphs = static_cast<int64_t>(list.glyphs.size());
		widgets = list.widgets_drawn;
	}
	out->set_quads(quads);
	out->set_quads_textured(quads_textured);
	out->set_lines(lines);
	out->set_glyphs(glyphs);
	out->set_widgets_drawn(widgets);
	return out;
}

void MenuFrame::_notification(int p_what) {
	if (p_what == NOTIFICATION_RESIZED) {
		queue_redraw();
	}
}

void MenuFrame::_draw() {
	ensure_overlay_canvas_item_();
	RenderingServer *rs = RenderingServer::get_singleton();
	rs->canvas_item_clear(overlay_canvas_item_);
	rs->canvas_item_clear(overlay_upper_canvas_item_);
	if (!configured_) {
		custom_slot_drawn_ = false;
		rs->canvas_item_set_visible(slot_canvas_item_, false);
		return;
	}
	const Vector2 scale = design_scale_();
	const opennova::menu::MenuDrawList &list =
			compiler_.compile(state_, scale.x, scale.y);
	// The marquee reset request is one-shot: the compile above consumed it.
	for (opennova::menu::MenuWidgetState &ws : state_.widgets) {
		ws.marquee_reset = false;
	}
	// Apply the compiler's one painter-order stream across primitive kinds.
	// The original walks screens/children forward and each widget emits its
	// frame, appearance, text, then children in vtable+24 order; batching all
	// glyphs after all quads lets later backgrounds leak earlier text through.
	// The engine-side MenuDrawList witness record in docs/mnu/menu-re.md owns
	// the original address correspondence; this adapter only replays it.
	// Ops before overlay_op_start paint on this Control's own canvas item; the
	// menu-top overlay (open popups + cursor) paints on the z-above child item
	// so frame-child mounts never cover it.
	// The draw list is the original's window coordinates, which Direct3D 9
	// rasterises with pixel centres on the integers: every item draws it under
	// d3d9_screen_to_canvas, half a pixel right and down, a draw-only transform
	// that leaves the Control's input on the original's coordinates
	// (renderer/d3d9_raster.h; the MenuQuad contract, engine menu/menu_frame.h).
	const Transform2D d3d9 = d3d9_screen_to_canvas();
	for (const RID &item : { get_canvas_item(), overlay_canvas_item_, overlay_upper_canvas_item_ }) {
		rs->canvas_item_add_set_transform(item, d3d9);
	}
	// What the device rasterised itself pixel for pixel (a frame piece, the fill's
	// phased tile) sits on whole pixels: drawn back half a pixel, 1:1.
	const float centre = opennova::renderer::kD3d9PixelCentre;
	RID target = get_canvas_item();
	const auto apply_quad = [&](const opennova::menu::MenuQuad &quad) {
		const Rect2 rect(quad.x0, quad.y0, quad.x1 - quad.x0, quad.y1 - quad.y0);
		const Color color = opennova::color_from_argb(quad.color);
		Ref<Texture2D> tex;
		if (quad.texture >= 0 &&
				quad.texture < static_cast<int32_t>(textures_.size())) {
			tex = texture_for_quad_(quad);
		}
		if (tex.is_null()) {
			if (quad.texture == opennova::menu::kMenuTexNone) {
				rs->canvas_item_add_rect(target, rect, color);
			}
			// An unresolved texture draws nothing [orig: every draw is gated
			// on a successful texture load].
			return;
		}
		if (quad.texture2 != opennova::menu::kMenuTexNone) {
			// frame_piece_texture_ rasterised retail's two-stage material over the
			// pixels the piece covers; its colour is in the image.
			const opennova::renderer::PixelRect px =
					opennova::renderer::d3d9_quad_pixels(quad.x0, quad.y0, quad.x1, quad.y1);
			rs->canvas_item_add_texture_rect(target,
					Rect2(px.x0 - centre, px.y0 - centre, px.x1 - px.x0, px.y1 - px.y0),
					tex->get_rid(), false);
		} else if (quad.tiled) {
			// The phased tile repeats from the rect's corner: pixel x shows texel
			// x mod cell, retail's (x + 0.5) / cell (the MenuQuad contract).
			rs->canvas_item_add_texture_rect(target,
					Rect2(rect.position - Vector2(centre, centre), rect.size), tex->get_rid(), true,
					color);
		} else {
			// A menu image: the region in the image's texels, the strip's half
			// texel included, out of its power-of-two texture.
			const Ref<Image> &image = texture_images_[static_cast<size_t>(quad.texture)];
			const Vector2 image_size = image.is_valid()
					? Vector2(image->get_width(), image->get_height())
					: tex->get_size();
			rs->canvas_item_add_texture_rect_region(target, rect, tex->get_rid(),
					Rect2(quad.u0 * image_size.x, quad.v0 * image_size.y,
							(quad.u1 - quad.u0) * image_size.x,
							(quad.v1 - quad.v0) * image_size.y),
					color, false, false);
		}
	};
	const auto apply_line = [&](const opennova::menu::MenuLine &line) {
		// The pixels Direct3D 9's line rule lights, where the engine knows them;
		// any other segment as a one-pixel line on this raster.
		opennova::renderer::PixelRect px;
		const Color color = opennova::color_from_argb(line.color);
		if (opennova::renderer::d3d9_axis_line_pixels(line.x0, line.y0, line.x1, line.y1, &px)) {
			rs->canvas_item_add_rect(target,
					Rect2(px.x0 - centre, px.y0 - centre, px.x1 - px.x0, px.y1 - px.y0), color);
			return;
		}
		rs->canvas_item_add_line(target, Vector2(line.x0, line.y0),
				Vector2(line.x1, line.y1), color, 1.0f);
	};
	const auto apply_font_run =
			[&](const opennova::menu::MenuDrawList::FontRun &run) {
				const std::vector<Ref<Texture2D>> *pages = font_pages_(run.font);
				if (pages == nullptr) {
					return;
				}
				// One triangle array per consecutive font page, through the page's
				// material: its MODULATE2X stage rides the glyph shader's UV.y flag,
				// doubling the text sink's halved colour (hud::kFontPageMaterialWord).
				const Vector2 uv_flag(0.0f,
						font_page_runs_modulate2x() ? kGlyphCanvasUvFlag : 0.0f);
				const size_t end = std::min(list.glyphs.size(),
						static_cast<size_t>(std::max(run.first + run.count, 0)));
				for (size_t first = static_cast<size_t>(std::max(run.first, 0)); first < end;) {
					const uint32_t page_index = list.glyphs[first].page;
					size_t run_end = first + 1;
					while (run_end < end && list.glyphs[run_end].page == page_index) ++run_end;
					const Ref<Texture2D> page = page_index < pages->size()
							? (*pages)[page_index]
							: Ref<Texture2D>();
					if (page.is_valid()) {
						GlyphRunArrays arrays;
						append_glyph_quads(list.glyphs, first, run_end, uv_flag, arrays);
						rs->canvas_item_add_triangle_array(target, arrays.indices, arrays.points,
								arrays.colors, arrays.uvs, PackedInt32Array(), PackedFloat32Array(),
								page->get_rid());
						if (glyph_record_ != nullptr) {
							Dictionary row;
							row["page"] = static_cast<int64_t>(page_index);
							row["uvs"] = arrays.uvs;
							row["colors"] = arrays.colors;
							glyph_record_->push_back(row);
						}
					}
					first = run_end;
				}
				const int32_t underline_end = run.underline_first + run.underline_count;
				for (int32_t i = run.underline_first;
						i < underline_end &&
						i < static_cast<int32_t>(list.underlines.size());
						++i) {
					const opennova::hud::GameFontUnderline &underline =
							list.underlines[static_cast<size_t>(i)];
					rs->canvas_item_add_line(target, Vector2(underline.x0, underline.y),
							Vector2(underline.x1, underline.y),
							opennova::color_from_argb(underline.color), 1.0f);
				}
			};
	// The custom-draw slot: the companion's item shows only while the pass
	// ran, and the ops from it on move to the item above it.
	custom_slot_drawn_ = list.custom_slot_op >= 0;
	rs->canvas_item_set_visible(slot_canvas_item_, custom_slot_drawn_);
	for (size_t op_index = 0; op_index < list.draw_ops.size(); ++op_index) {
		if (static_cast<int32_t>(op_index) == list.overlay_op_start) {
			target = overlay_canvas_item_;
		}
		if (static_cast<int32_t>(op_index) == list.custom_slot_op) {
			target = overlay_upper_canvas_item_;
		}
		const opennova::menu::MenuDrawList::DrawOp &op = list.draw_ops[op_index];
		switch (op.kind) {
			case opennova::menu::MenuDrawList::DrawOp::Kind::Quad:
				if (op.index >= 0 && op.index < static_cast<int32_t>(list.quads.size())) {
					apply_quad(list.quads[static_cast<size_t>(op.index)]);
				}
				break;
			case opennova::menu::MenuDrawList::DrawOp::Kind::Line:
				if (op.index >= 0 && op.index < static_cast<int32_t>(list.lines.size())) {
					apply_line(list.lines[static_cast<size_t>(op.index)]);
				}
				break;
			case opennova::menu::MenuDrawList::DrawOp::Kind::FontRun:
				if (op.index >= 0 &&
						op.index < static_cast<int32_t>(list.font_runs.size())) {
					apply_font_run(list.font_runs[static_cast<size_t>(op.index)]);
				}
				break;
		}
	}
}

Array MenuFrame::options_scroll_ranges() {
	Array out;
	for (const opennova::menu::OptionsScrollRange &r : opennova::menu::kOptionsScrollRanges) {
		Dictionary row;
		row["control"] = String(r.control);
		row["minimum"] = r.minimum;
		row["maximum"] = r.maximum;
		row["page"] = r.page;
		out.push_back(row);
	}
	return out;
}

Array MenuFrame::video_quality_controls() {
	Array out;
	for (const opennova::menu::VideoQualityControl &c : opennova::menu::kVideoQualityControls) {
		Dictionary row;
		row["control"] = String(c.control);
		row["value"] = String(c.semantic_value);
		out.push_back(row);
	}
	return out;
}

int MenuFrame::video_gamma_reference() {
	return opennova::menu::kVideoGammaReference;
}

PackedStringArray MenuFrame::object_detail_controls() {
	PackedStringArray out;
	for (const char *name : opennova::menu::kObjectDetailControls) out.push_back(String(name));
	return out;
}

PackedStringArray MenuFrame::texture_filter_controls() {
	PackedStringArray out;
	for (const char *name : opennova::menu::kTextureFilterControls) out.push_back(String(name));
	return out;
}

PackedStringArray MenuFrame::particle_density_controls() {
	PackedStringArray out;
	for (const char *name : opennova::menu::kParticleDensityControls) out.push_back(String(name));
	return out;
}

PackedStringArray MenuFrame::texture_compression_controls() {
	PackedStringArray out;
	for (const char *name : opennova::menu::kTexCompressionControls) out.push_back(String(name));
	return out;
}

PackedStringArray MenuFrame::video_preset_buttons() {
	PackedStringArray out;
	for (const char *name : opennova::menu::kVideoPresetButtons) out.push_back(String(name));
	return out;
}

PackedStringArray MenuFrame::options_unsupported_controls() {
	PackedStringArray out;
	for (const char *name : opennova::menu::kOptionsUnsupportedControls) out.push_back(String(name));
	return out;
}

Array MenuFrame::options_forced_checks() {
	Array out;
	for (const opennova::menu::OptionsForcedCheck &c : opennova::menu::kOptionsForcedChecks) {
		Dictionary row;
		row["control"] = String(c.control);
		row["checked"] = c.checked;
		out.push_back(row);
	}
	return out;
}

void MenuFrame::_bind_methods() {
	ClassDB::bind_static_method("MenuFrame", D_METHOD("options_scroll_ranges"),
			&MenuFrame::options_scroll_ranges);
	ClassDB::bind_static_method("MenuFrame", D_METHOD("video_quality_controls"),
			&MenuFrame::video_quality_controls);
	ClassDB::bind_static_method("MenuFrame", D_METHOD("object_detail_controls"),
			&MenuFrame::object_detail_controls);
	ClassDB::bind_static_method("MenuFrame", D_METHOD("texture_filter_controls"),
			&MenuFrame::texture_filter_controls);
	ClassDB::bind_static_method("MenuFrame", D_METHOD("particle_density_controls"),
			&MenuFrame::particle_density_controls);
	ClassDB::bind_static_method("MenuFrame", D_METHOD("texture_compression_controls"),
			&MenuFrame::texture_compression_controls);
	ClassDB::bind_static_method("MenuFrame", D_METHOD("video_gamma_reference"),
			&MenuFrame::video_gamma_reference);
	ClassDB::bind_static_method("MenuFrame", D_METHOD("options_unsupported_controls"),
			&MenuFrame::options_unsupported_controls);
	ClassDB::bind_static_method("MenuFrame", D_METHOD("options_forced_checks"),
			&MenuFrame::options_forced_checks);
	ClassDB::bind_static_method("MenuFrame", D_METHOD("video_preset_buttons"),
			&MenuFrame::video_preset_buttons);
	ClassDB::bind_static_method("MenuFrame", D_METHOD("glyph_shader_code"),
			&MenuFrame::glyph_shader_code);
	ClassDB::bind_method(D_METHOD("get_glyph_submissions"), &MenuFrame::get_glyph_submissions);
	ClassDB::bind_method(
			D_METHOD("configure", "document", "screen_name", "root", "style",
					"override_text"),
			&MenuFrame::configure);
	ClassDB::bind_method(D_METHOD("is_configured"), &MenuFrame::is_configured);
	ClassDB::bind_method(D_METHOD("set_widget_disabled", "index", "disabled"),
			&MenuFrame::set_widget_disabled);
	ClassDB::bind_method(D_METHOD("set_widget_checked", "index", "checked"),
			&MenuFrame::set_widget_checked);
	ClassDB::bind_method(D_METHOD("set_widget_text", "index", "text"),
			&MenuFrame::set_widget_text);
	ClassDB::bind_method(D_METHOD("get_widget_hover_item", "index"),
			&MenuFrame::get_widget_hover_item);
	ClassDB::bind_method(D_METHOD("scroll_hit_at", "index", "position"),
			&MenuFrame::scroll_hit_at);
	ClassDB::bind_method(D_METHOD("set_widget_scroll_range", "index", "minimum",
								 "maximum", "page", "value"),
			&MenuFrame::set_widget_scroll_range);
	ClassDB::bind_method(D_METHOD("process_mouse", "position", "button_down"),
			&MenuFrame::process_mouse);
	ClassDB::bind_method(D_METHOD("widget_index", "name"),
			&MenuFrame::widget_index);
	ADD_SIGNAL(MethodInfo("widget_clicked",
			PropertyInfo(Variant::INT, "index"),
			PropertyInfo(Variant::INT, "part")));
	// The engine pump's CScrollWnd interaction result: a standalone Scroll's
	// authored-range value, or an embedded row owner's new first-visible row.
	ADD_SIGNAL(MethodInfo("scroll_value_changed",
			PropertyInfo(Variant::INT, "index"),
			PropertyInfo(Variant::INT, "value")));
	ClassDB::bind_method(D_METHOD("get_draw_list_stats"),
			&MenuFrame::get_draw_list_stats);
	ClassDB::bind_method(D_METHOD("set_widget_items", "index", "items"),
			&MenuFrame::set_widget_items);
	ClassDB::bind_method(D_METHOD("set_widget_table_cells", "index", "rows"),
			&MenuFrame::set_widget_table_cells);
	ClassDB::bind_method(D_METHOD("set_widget_clip_rect", "index", "enabled", "rect"),
			&MenuFrame::set_widget_clip_rect);
	ClassDB::bind_method(D_METHOD("widget_count"), &MenuFrame::widget_count);
	ClassDB::bind_method(D_METHOD("widget_name", "index"),
			&MenuFrame::widget_name);
	ClassDB::bind_method(D_METHOD("widget_kind", "index"),
			&MenuFrame::widget_kind);
	ClassDB::bind_method(D_METHOD("is_widget_disabled", "index"),
			&MenuFrame::is_widget_disabled);
	ClassDB::bind_method(D_METHOD("is_widget_shown", "index"),
			&MenuFrame::is_widget_shown);
	ClassDB::bind_method(D_METHOD("widget_rect", "index"),
			&MenuFrame::widget_rect);
	ClassDB::bind_method(D_METHOD("widget_local_rect", "index"),
			&MenuFrame::widget_local_rect);
	ClassDB::bind_method(D_METHOD("set_mount_widget", "index"), &MenuFrame::set_mount_widget);
	ClassDB::bind_method(D_METHOD("get_mount_widget"), &MenuFrame::get_mount_widget);
	ClassDB::bind_method(D_METHOD("set_custom_slot_widget", "index"),
			&MenuFrame::set_custom_slot_widget);
	ClassDB::bind_method(D_METHOD("get_custom_slot_widget"), &MenuFrame::get_custom_slot_widget);
	ClassDB::bind_method(D_METHOD("get_custom_slot_canvas_item"),
			&MenuFrame::get_custom_slot_canvas_item);
	ClassDB::bind_method(D_METHOD("is_custom_slot_drawn"), &MenuFrame::is_custom_slot_drawn);
	ClassDB::bind_method(D_METHOD("item_count", "index"),
			&MenuFrame::item_count);
	ClassDB::bind_method(D_METHOD("get_widget_text", "index"),
			&MenuFrame::get_widget_text);
	ClassDB::bind_method(D_METHOD("hit_test", "position"),
			&MenuFrame::hit_test);
	ClassDB::bind_method(D_METHOD("get_cursor_texture"),
			&MenuFrame::get_cursor_texture);
	ClassDB::bind_method(D_METHOD("get_unresolved_asset_count"),
			&MenuFrame::get_unresolved_asset_count);
	BIND_CONSTANT(DESIGN_WIDTH);
	BIND_CONSTANT(DESIGN_HEIGHT);
	BIND_CONSTANT(EDIT_KEY_BACKSPACE);
	BIND_CONSTANT(EDIT_KEY_ENTER);
	BIND_CONSTANT(EDIT_KEY_END);
	BIND_CONSTANT(EDIT_KEY_HOME);
	BIND_CONSTANT(EDIT_KEY_LEFT);
	BIND_CONSTANT(EDIT_KEY_RIGHT);
	BIND_CONSTANT(EDIT_KEY_DELETE);
	BIND_CONSTANT(EDIT_RESULT_CHANGED);
	BIND_CONSTANT(EDIT_RESULT_COMMIT);
	BIND_CONSTANT(SCROLL_HIT_SHUTTLE);
}
