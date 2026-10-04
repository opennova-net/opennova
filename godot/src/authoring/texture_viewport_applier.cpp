#include "authoring/texture_viewport_applier.h"

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/vector2.hpp>

#include <algorithm>
#include <cstring>

#include <editor/preview/texture_viewport.h>
#include <editor/preview/viewport_device.h>

namespace godot {

namespace {

// The picture: each pixel's first-level texel by the camera; the background off the texture; the
// level's texel (nearest, or filtered where a texel covers less than a pixel) through the channels
// shown; the checkerboard 8 pixels a square, behind the colour by its alpha.
constexpr const char *kShader = R"(shader_type canvas_item;
uniform sampler2D level_nearest : filter_nearest, repeat_disable;
uniform sampler2D level_linear : filter_linear, repeat_disable;
uniform vec2 canvas_size = vec2(1.0);
uniform vec2 base_size = vec2(1.0);
uniform vec2 centre = vec2(0.0);
uniform float scale = 1.0;
uniform int channels = 5;
uniform bool nearest = true;
uniform bool has_texture = false;
void fragment() {
	vec2 pixel = UV * canvas_size;
	vec2 texel = centre + (pixel - canvas_size * 0.5) / scale;
	if (!has_texture || texel.x < 0.0 || texel.y < 0.0 || texel.x >= base_size.x || texel.y >= base_size.y) {
		COLOR = vec4(0.16, 0.16, 0.16, 1.0);
	} else {
		vec2 uv = texel / base_size;
		vec4 c = nearest ? texture(level_nearest, uv) : texture(level_linear, uv);
		float check = mod(floor(pixel.x / 8.0) + floor(pixel.y / 8.0), 2.0);
		vec3 board = mix(vec3(0.40), vec3(0.62), check);
		if (channels == 0) COLOR = vec4(c.rgb, 1.0);
		else if (channels == 1) COLOR = vec4(vec3(c.r), 1.0);
		else if (channels == 2) COLOR = vec4(vec3(c.g), 1.0);
		else if (channels == 3) COLOR = vec4(vec3(c.b), 1.0);
		else if (channels == 4) COLOR = vec4(vec3(c.a), 1.0);
		else COLOR = vec4(mix(board, c.rgb, c.a), 1.0);
	}
}
)";

} // namespace

TextureViewportApplier::TextureViewportApplier(SubViewport &viewport) {
	Ref<Shader> shader;
	shader.instantiate();
	shader->set_code(kShader);
	material_.instantiate();
	material_->set_shader(shader);
	rect_ = memnew(ColorRect);
	rect_->set_name("Texture");
	rect_->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	rect_->set_material(material_);
	rect_->set_position(Vector2(0.0f, 0.0f));
	rect_->set_size(Vector2(1.0f, 1.0f));
	viewport.add_child(rect_);
	rect_id_ = rect_->get_instance_id();
}

void TextureViewportApplier::rebuild(const opennova::editor::ViewportModel &model, const opennova::editor::SessionView &,
		const opennova::editor::PreviewClock &) {
	const auto &texture = static_cast<const opennova::editor::TextureViewport &>(model);
	clear();
	image_ = texture.image();
	if (!image_) return;
	for (const opennova::editor::TextureLevel &level : image_->levels) {
		Ref<ImageTexture> made;
		if (level.width && level.height && level.rgba.size() == size_t(level.width) * level.height * 4) {
			PackedByteArray bytes;
			bytes.resize(int64_t(level.rgba.size()));
			std::memcpy(bytes.ptrw(), level.rgba.data(), level.rgba.size());
			const Ref<Image> picture = Image::create_from_data(int32_t(level.width), int32_t(level.height), false, Image::FORMAT_RGBA8, bytes);
			made = ImageTexture::create_from_image(picture);
		}
		levels_.push_back(made);
	}
}

void TextureViewportApplier::update(const opennova::editor::ViewportModel &, const opennova::editor::PreviewClock &) {}

void TextureViewportApplier::clear() {
	levels_.clear();
	image_.reset();
	bound_ = -1;
	material_->set_shader_parameter("has_texture", false);
	material_->set_shader_parameter("level_nearest", Variant());
	material_->set_shader_parameter("level_linear", Variant());
}

void TextureViewportApplier::apply(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &,
		opennova::editor::ViewportDeviceReport &) {
	const auto &texture = static_cast<const opennova::editor::TextureViewport &>(model);
	if (!image_ || levels_.empty() || texture.image() != image_) {
		material_->set_shader_parameter("has_texture", false);
		return;
	}
	const int level = int(std::min(texture.shown_level(), levels_.size() - 1));
	if (level != bound_) {
		bound_ = level;
		material_->set_shader_parameter("level_nearest", levels_[size_t(level)]);
		material_->set_shader_parameter("level_linear", levels_[size_t(level)]);
	}
	const opennova::editor::TexturePlacement placed = texture.placement(width_, height_);
	const opennova::editor::TextureLevel &shown = image_->levels[size_t(level)];
	// A texel of the level shown covers this many picture pixels.
	const float covers = placed.scale * float(image_->width()) / float(std::max(1u, shown.width));
	material_->set_shader_parameter("has_texture", levels_[size_t(level)].is_valid());
	material_->set_shader_parameter("canvas_size", Vector2(float(width_), float(height_)));
	material_->set_shader_parameter("base_size", Vector2(float(image_->width()), float(image_->height())));
	material_->set_shader_parameter("centre", Vector2(placed.x, placed.y));
	material_->set_shader_parameter("scale", placed.scale);
	material_->set_shader_parameter("channels", int(texture.options().channels));
	material_->set_shader_parameter("nearest", covers >= 1.0f);
}

void TextureViewportApplier::tick(const opennova::editor::ViewportModel &, const opennova::editor::PreviewClock &) {}

void TextureViewportApplier::resize(int width, int height) {
	width_ = std::max(width, 1);
	height_ = std::max(height, 1);
	if (ColorRect *rect = Object::cast_to<ColorRect>(ObjectDB::get_instance(rect_id_)))
		rect->set_size(Vector2(float(width_), float(height_)));
}

} // namespace godot
