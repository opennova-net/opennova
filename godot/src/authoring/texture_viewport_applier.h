#pragma once

#include <godot_cpp/classes/color_rect.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>

#include <memory>
#include <vector>

#include <editor/documents/texture_image.h>

#include "authoring/viewport_applier.h"

namespace godot {

// A texture viewport's device work (ADR 0046 S18): the texture's levels as GPU textures (one a level,
// the texels the portable decode made: editor/documents/texture_image, nothing decoded here) drawn
// across the device's SubViewport by one canvas shader, which places the texture by the viewport's
// camera on the picture's size (TextureViewport::placement), shows the channels its options name (the
// colour, one channel or the alpha as grey, the colour over a checkerboard by its alpha) and samples the
// level they name, texel for texel where a texel covers a pixel or more (nearest), filtered where it
// covers less. A Rebuild takes another texture; the camera and the options it applies every pump. It
// reads none of the process-wide render state a mission publishes (reads_scene_state false).
class TextureViewportApplier final : public ViewportApplier {
public:
	explicit TextureViewportApplier(SubViewport &viewport);

	void rebuild(const opennova::editor::ViewportModel &model, const opennova::editor::SessionView &view,
			const opennova::editor::PreviewClock &clock) override;
	void update(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock) override;
	void clear() override;
	void apply(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock,
			opennova::editor::ViewportDeviceReport &report) override;
	void tick(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock) override;
	void resize(int width, int height) override;
	bool reads_scene_state() const override { return false; }
	// The editor's preview background around the texture (its own 0.16 grey on Dark).
	void background(opennova::editor::PreviewBackground background) override;

	// What it holds (a GUT device test reads it): the levels uploaded, the level bound, the shader's
	// placement.
	int levels() const { return int(levels_.size()); }
	int bound_level() const { return bound_; }
	ShaderMaterial *material() const { return material_.ptr(); }

private:
	ColorRect *rect_ = nullptr;
	uint64_t rect_id_ = 0;
	Ref<ShaderMaterial> material_;
	std::shared_ptr<const opennova::editor::TextureImage> image_; // the texture its levels are of
	std::vector<Ref<ImageTexture>> levels_;
	int bound_ = -1;
	int width_ = 1, height_ = 1;
};

} // namespace godot
