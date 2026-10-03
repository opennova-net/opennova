#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include <editor/documents/texture_image.h>
#include <editor/preview/viewport_model.h>

namespace opennova::editor {

// What a texture viewport shows, and why not (ADR 0046 S18): the kind's reason.
enum class TextureViewStatus : uint8_t {
	NoTexture, // no texture is open or selected at its path
	Unloadable, // the game cannot load the file (the image's refusal)
	Undecoded, // the game loads it, but the editor does not decode its texels yet
	Ready,
};
// "no_texture", "unloadable", "undecoded", "ready": its token on the wire.
const char *texture_view_status_token(TextureViewStatus status);

// Which of a texture's channels the picture shows: its colour (alpha left out), one channel as grey,
// its alpha as grey, or its colour over a checkerboard by its alpha (what shows through where it is
// transparent).
enum class TextureChannels : uint8_t { Rgb, Red, Green, Blue, Alpha, Rgba };
// "rgb", "red", "green", "blue", "alpha", "rgba", and the token's channels (false for none).
const char *texture_channels_token(TextureChannels channels);
bool texture_channels_from_token(const std::string &token, TextureChannels &out);

// How a texture viewport draws its texture: the channels, and the mip level (0 the texture itself; a
// level past the file's last shows the last).
struct TextureViewportOptions {
	TextureChannels channels = TextureChannels::Rgba;
	int level = 0;
	bool operator==(const TextureViewportOptions &other) const {
		return channels == other.channels && level == other.level;
	}
	bool operator!=(const TextureViewportOptions &other) const { return !(*this == other); }
};

// Its camera: fitted to the picture (the whole texture, as large as the picture holds it), or a scale
// (picture pixels a texel of the first level) with the texel at the picture's middle (x, y, in the
// first level's texels, from its top left corner).
struct TextureCamera {
	bool fit = true;
	float scale = 1.0f;
	float x = 0.0f;
	float y = 0.0f;
	bool operator==(const TextureCamera &other) const {
		return fit == other.fit && scale == other.scale && x == other.x && y == other.y;
	}
};
// The zoom steps the wheel and the toolbar take, smallest first.
inline constexpr float kTextureZoomSteps[] = {1.0f / 32, 1.0f / 16, 1.0f / 8, 1.0f / 4, 1.0f / 3, 1.0f / 2, 2.0f / 3,
                                              1.0f, 1.5f, 2.0f, 3.0f, 4.0f, 6.0f, 8.0f, 12.0f, 16.0f, 24.0f, 32.0f,
                                              48.0f, 64.0f};
// The step after `scale` toward larger (`in`) or smaller.
float texture_zoom_step(float scale, bool in);

// A camera as a picture of `width` x `height` pixels shows it (a fit made a scale and a middle).
struct TexturePlacement {
	float scale = 1.0f;
	float x = 0.0f;
	float y = 0.0f;
	// The first-level texel under picture pixel (px, py), as a real (its whole part the texel).
	void texel_of(float px, float py, int width, int height, float &tx, float &ty) const {
		tx = x + (px - float(width) * 0.5f) / scale;
		ty = y + (py - float(height) * 0.5f) / scale;
	}
	// The picture pixel of first-level texel position (tx, ty).
	void pixel_of(float tx, float ty, int width, int height, float &px, float &py) const {
		px = (tx - x) * scale + float(width) * 0.5f;
		py = (ty - y) * scale + float(height) * 0.5f;
	}
};

// The change a SetViewport makes to set a texture viewport's camera to `camera`.
std::string texture_camera_change(const TextureCamera &camera);
// And its options to `options`.
std::string texture_options_change(const TextureViewportOptions &options);

// A texture's viewport (ADR 0046 S18; ViewportKind::Texture, the Main role of the texture type, which
// the Preview window shows too for a texture Files selects, open or not): the texture as the game
// reads it (texture_image.h), at a zoom (fitted, or a scale about a middle texel, which the wheel steps
// about the pointer and a drag pans: its camera), through one or all of its channels, at a mip level
// (its options); each a SetViewport. It reads the texture document open at its path, else the project's
// file there (read again when the scan says its size or last write moved), so a file Files selects shows
// before it is opened. A Rebuild hands the device another texture's texels; the camera and the options
// it applies every pump. A point of the picture names the texel under it (hit: its column and row in the
// level shown, its value), never a record: a texture holds none.
class TextureViewport final : public ViewportModel {
public:
	explicit TextureViewport(std::string path);
	static std::unique_ptr<ViewportModel> make(const std::string &path);

	TextureViewStatus view_status() const { return reason_; }
	// The texture shown (null unless one is read), how many times a texture was read for it (an open
	// document's image taken, or a closed file read and decoded), and whether it came from a file
	// rather than an open document.
	const std::shared_ptr<const TextureImage> &image() const { return image_; }
	uint64_t reads() const { return reads_; }
	bool from_file() const { return from_file_; }
	const TextureViewportOptions &options() const { return options_; }
	const TextureCamera &camera() const { return camera_; }
	// The level drawn (the options' held within the texture's levels).
	size_t shown_level() const;
	// The camera on a picture `width` x `height`.
	TexturePlacement placement(int width, int height) const;
	// The texel of the level shown under picture pixel (x, y) of a picture `width` x `height`, and its
	// value: false off the texture or with none shown.
	bool texel_at(float x, float y, int width, int height, uint32_t &tx, uint32_t &ty, uint8_t rgba[4]) const;
	// A texel's words: "12, 40: R 255 G 128 B 64 A 255", its palette index after where it has one.
	std::string texel_words(uint32_t tx, uint32_t ty, const uint8_t rgba[4]) const;

	ViewportStatus status() const override;
	const char *reason() const override { return texture_view_status_token(reason_); }
	std::string message() const override;
	const std::string &detail() const override { return detail_; }
	std::string caption() const override;
	const char *units() const override { return "pixels"; }
	ViewportLayout layout() const override { return ViewportLayout(); }
	std::unique_ptr<CanvasHalf> make_canvas() const override;
	// The texel under the point: index its place in the level shown (row by row), name its words,
	// kind "texel".
	ViewportHit hit(const ViewportContext &context, float x, float y) const override;
	bool handle_point(const ViewportContext &context, NodeId id, const std::string &handle, float &x, float &y,
	                  std::string &error) const override;
	bool drag(const ViewportContext &context, const ViewportDrag &drag, CanvasRequests &out,
	          std::string &error) const override;
	// "fit" (the camera fitted), "actual" (one texel a pixel about the middle the picture shows now).
	bool command(const ViewportContext &context, const std::string &name, const std::vector<NodeId> &ids,
	             CanvasRequests &out, std::string &error) const override;
	io::JsonValue options_json() const override;
	io::JsonValue camera_json() const override;
	// The texture's facts (texture_image_json), the level shown and the camera as the device's size
	// places it.
	io::JsonValue body_json(const ViewportInput &input) const override;
	io::JsonValue items_json(const ViewportInput &input) const override;

protected:
	ViewportAction follow_(const ViewportInput &input, PreviewClock &clock) override;
	bool takes_(const std::string &member) const override;
	bool check_(const io::JsonValue &json, std::string &error) const override;
	void apply_(const io::JsonValue &json, PreviewClock &clock) override;

private:
	TextureViewStatus reason_ = TextureViewStatus::NoTexture;
	std::string detail_;
	TextureViewportOptions options_;
	TextureCamera camera_;
	std::shared_ptr<const TextureImage> image_;
	uint64_t reads_ = 0;
	bool from_file_ = false;
	// The closed file last read: its size and last write as the scan listed them, and what it held.
	bool file_read_ = false;
	uint64_t file_size_ = 0;
	int64_t file_modified_ = 0;
	std::shared_ptr<const TextureImage> file_image_;
};

} // namespace opennova::editor
