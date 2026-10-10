#pragma once

#include <godot_cpp/classes/color_rect.hpp>
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/classes/texture2d.hpp>

#include <memory>
#include <vector>

#include "authoring/viewport_applier.h"

namespace opennova::editor {
struct FontPicture;
}

namespace godot {

// A font viewport's device work (round S23 lane A): the font's pages as textures (the texels the document holds,
// one RGBA8 texture a page, nothing decoded here), and the picture the portable viewport lays out
// (editor/preview/font_viewport: the text's glyph quads from the game's text engine, or a page with its glyphs'
// rects) drawn on one canvas item, each glyph run through the font page's material, MODULATE2X(TEXTURE, DIFFUSE)
// (hud/font_page_glyphs: the game's page stage), each texel `zoom` pixels across, nearest. The editor's preview
// background behind it. A Rebuild takes the pages anew; every pump draws the layout again where it moved. It reads
// none of the process-wide render state a mission publishes.
class FontViewportApplier final : public ViewportApplier {
public:
	explicit FontViewportApplier(SubViewport &viewport);

	void rebuild(const opennova::editor::ViewportModel &model, const opennova::editor::SessionView &view,
			const opennova::editor::PreviewClock &clock) override;
	void update(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock) override;
	void clear() override;
	void apply(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock,
			opennova::editor::ViewportDeviceReport &report) override;
	void tick(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock) override;
	void resize(int width, int height) override;
	bool reads_scene_state() const override { return false; }
	void background(opennova::editor::PreviewBackground background) override;

	// What it holds (a GUT device test reads it): the pages uploaded, the glyph quads drawn last.
	int pages() const { return int(pages_.size()); }
	int quads_drawn() const { return quads_drawn_; }

private:
	void draw(const opennova::editor::ViewportModel &model);

	uint64_t backdrop_id_ = 0;
	uint64_t glyphs_id_ = 0;
	Ref<ShaderMaterial> backdrop_material_;
	Ref<Shader> glyph_shader_;
	Ref<ShaderMaterial> glyph_material_;
	std::shared_ptr<const opennova::editor::FontPicture> picture_; // the font its pages are of
	std::vector<Ref<Texture2D>> pages_;
	uint64_t drawn_serial_ = 0;
	bool drawn_ = false;
	int quads_drawn_ = 0;
	int width_ = 1, height_ = 1;
};

} // namespace godot
