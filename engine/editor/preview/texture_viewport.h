#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include <editor/documents/texture_compare.h>
#include <editor/documents/texture_image.h>
#include <editor/documents/texture_load_rules.h>
#include <editor/documents/texture_roles.h>
#include <editor/graph/texture_uses.h>
#include <editor/preview/texture_role_view.h>
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
// its alpha as grey, its colour over a checkerboard by its alpha (what shows through where it is
// transparent), or its colour read as a normal map and lit from the options' light (the relief a normal
// map gives a surface).
enum class TextureChannels : uint8_t { Rgb, Red, Green, Blue, Alpha, Rgba, Normals };
// "rgb", "red", "green", "blue", "alpha", "rgba", "normals", and the token's channels (false for none).
const char *texture_channels_token(TextureChannels channels);
bool texture_channels_from_token(const std::string &token, TextureChannels &out);

// How a texture viewport draws its texture: the channels, the mip level (0 the texture itself; a texture
// built from pixels shows the chain the game builds of it, a DDS its own; a level past the last shows the
// last), the use it shows the texture as, by its index among the texture's uses (texture_uses; -1 the file as
// its reader decodes it), the object texture detail it shows the device texture at (-1 as stored; 0 to 3, by
// the shown use's slot and loader, else the first model row's: renderer/device_texture), the light a
// normal map is lit from (degrees round the picture, 0 from its right, 90 from its top), and the compare
// (documents/texture_compare: the texture beside the DXT texture made of it, split at `split`, a fraction of
// its width, the compressed alone, or their difference; a compare shows the file's texels, whatever use is
// picked).
struct TextureViewportOptions {
	TextureChannels channels = TextureChannels::Rgba;
	int level = 0;
	int as_used = -1;
	int detail = -1;
	float light = 135.0f;
	TextureCompareView compare = TextureCompareView::Off;
	float split = 0.5f;
	bool operator==(const TextureViewportOptions &other) const {
		return channels == other.channels && level == other.level && as_used == other.as_used && detail == other.detail &&
		       light == other.light && compare == other.compare && split == other.split;
	}
	bool operator!=(const TextureViewportOptions &other) const { return !(*this == other); }
};

// A use as a texture viewport shows it (ADR 0046 S18): its index among the texture's uses (-1 none: the
// file itself) and words, what its loader makes of the texels, a cut-out material's test (its reference,
// -1 none; `inverted` keeps a texel at or below it), and the cells the game cuts it in (a tile atlas's
// 64 texels; 0 none), which the picture draws over it.
struct TextureShownUse {
	int index = -1;
	std::string words;
	renderer::TextureRoleId role = renderer::TextureRoleId::kCount;
	TextureLoadTransform transform = TextureLoadTransform::None;
	int cutout = -1;
	bool inverted = false;
	uint32_t cells = 0;
	// A model row's: what its alpha is to the game (texture_row_alpha_meaning) and that in words; a use of
	// another role says its role's (TextureRoleRow::alpha), `model_row` false.
	bool model_row = false;
	renderer::TextureAlphaMeaning alpha = renderer::TextureAlphaMeaning::Unused;
	std::string alpha_words;
	// A particle graphic's blend mode (formats/particle BlendMode), whose atlas page the picture shows it as; -1 none.
	int blend_mode = -1;
	bool operator==(const TextureShownUse &other) const {
		return index == other.index && words == other.words && role == other.role && transform == other.transform &&
		       cutout == other.cutout && inverted == other.inverted && cells == other.cells && model_row == other.model_row &&
		       alpha == other.alpha && alpha_words == other.alpha_words && blend_mode == other.blend_mode;
	}
};
// What a viewport shows of the use `use`, the texture's use at `index`.
TextureShownUse texture_shown_use(const TextureUse &use, int index);
// The texels a use shows of `image`: its loader's transform, then what its role's consumer makes of them
// (texture_role_texels: a blend map's weights, a particle graphic as its atlas page holds it), then its cut-out (alpha 255 where the
// material's test keeps a texel, 0 where it discards it), or for a model row whose alpha the game draws as no
// transparency (a specular brightness, unused) every texel opaque; `image` itself where the use changes nothing.
std::shared_ptr<const TextureImage> texture_as_used(const std::shared_ptr<const TextureImage> &image,
                                                    const TextureShownUse &use);

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

// The chain the game builds of a texture made from pixels: `first` and each level after it the D3DX box filter of
// the bytes the one before was stored as, `levels` in all (0: to 1 x 1) [orig: GTexture_CreateFromPixelData_0
// @ 0x6878BE, D3DXFilterTexture BOX; D3DXFilterTexture @ 0x6910C9, each level from the previous one
// @ 0x6912AD..0x6912F9]: renderer::extend_box_chain, the game's own chain.
std::vector<TextureLevel> texture_game_chain(const TextureLevel &first, uint32_t levels);
// `level` halved `halvings` times as the game halves a texture before it makes its device texture: each texel
// the truncated mean of a 2 x 2 block (renderer::halve_rgba_times, GTexture_Downsample2x2_RGBA8).
TextureLevel texture_halved(const TextureLevel &level, uint32_t halvings);
// A normal map's level lit from `light` degrees round the picture (0 from its right, 90 from its top) and
// above it: each texel's colour read as a normal ((c / 255) * 2 - 1, red across, green down the texture as the
// game's tangent frame runs, blue out of it) and drawn as the grey of its light, opaque.
TextureLevel texture_lit_normals(const TextureLevel &level, float light);

// The change a SetViewport makes to set a texture viewport's camera to `camera`.
std::string texture_camera_change(const TextureCamera &camera);
// And its options to `options`.
std::string texture_options_change(const TextureViewportOptions &options);

// A texture's viewport (ADR 0046 S18; ViewportKind::Texture, the Main role of the texture type, which
// the Preview window shows too for a texture Files selects, open or not): the texture as the game
// reads it (texture_image.h), at a zoom (fitted, or a scale about a middle texel, which the wheel steps
// about the pointer and a drag pans: its camera), through one or all of its channels, at a mip level, as
// the file or as one of its uses draws it (its options); each a SetViewport. It reads the texture document open at its path, else the project's
// file there (read again when the scan says its size or last write moved), so a file Files selects shows
// before it is opened. A Rebuild hands the device another texture's texels; the camera and the options
// it applies every pump. A point of the picture names the texel under it (hit: its column and row in the
// level shown, its value), never a record: a texture holds none.
class TextureViewport final : public ViewportModel {
public:
	explicit TextureViewport(std::string path);
	static std::unique_ptr<ViewportModel> make(const std::string &path);

	TextureViewStatus view_status() const { return reason_; }
	// The texture shown (null unless one is read: the file as its reader decodes it, or as the use the
	// options name shows it), the file as its reader decodes it, the use shown, how many times a texture
	// was read for it (an open document's image taken, or a closed file read and decoded), and whether it
	// came from a file rather than an open document.
	const std::shared_ptr<const TextureImage> &image() const { return image_; }
	const std::shared_ptr<const TextureImage> &source() const { return source_; }
	const TextureShownUse &shown_use() const { return use_; }
	// The object texture detail shown (the options' where a model row's budget holds it): its level, -1 none, and
	// the device texture that is.
	int shown_detail() const { return detail_level_; }
	// What the use shown's role reads of the texture (texture_role_view: a blend map's weights and the splat details
	// they weigh, a foliage map's codes and what grows on them, a particle graphic's atlas page); empty for none.
	const TextureRoleView &role_view() const { return role_view_; }
	const renderer::DeviceTexture &shown_device() const { return detail_device_; }
	uint64_t reads() const { return reads_; }
	bool from_file() const { return from_file_; }
	// The compare, made while the options ask for one (null otherwise): the texture beside the DXT texture made
	// of it, or the `.dds` beside its import's source, with each level's error.
	const std::shared_ptr<const TextureCompression> &compression() const { return compression_; }
	// The error of the level shown, where a compare is made (null otherwise).
	const TextureLevelError *shown_error() const;
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
	std::shared_ptr<const TextureImage> source_;
	TextureShownUse use_;
	uint64_t reads_ = 0;
	bool from_file_ = false;
	// The closed file last read: its size and last write as the scan listed them, and what it held.
	bool file_read_ = false;
	uint64_t file_size_ = 0;
	int64_t file_modified_ = 0;
	std::shared_ptr<const TextureImage> file_image_;
	// The object texture detail shown and its device texture; the picture's options last made (level, detail,
	// channels, light), which move the picture when they change.
	int detail_level_ = -1;
	renderer::DeviceTexture detail_device_;
	TextureRoleView role_view_;
	// The terrain a terrain map's use names it from (its .trn, read again when the scan says its size or last write
	// moved): what its legend reads; null where it did not read.
	std::string terrain_path_;
	uint64_t terrain_size_ = 0;
	int64_t terrain_modified_ = 0;
	std::shared_ptr<const TrnConfig> terrain_;
	TextureViewportOptions made_options_;
	// The picture of the source and the use: the transforms the options ask for applied.
	std::shared_ptr<const TextureImage> picture(const std::shared_ptr<const TextureImage> &used, const TextureBudget *budget);
	// The compare shown (null while the options ask for none) and the one kept, made of `compared_` (and its
	// import source's stamp, a .dds's) whether shown or not; the picture's view and split last made.
	std::shared_ptr<const TextureCompression> compression_;
	std::shared_ptr<const TextureCompression> kept_;
	std::shared_ptr<const TextureImage> compared_;
	uint64_t compared_source_size_ = 0;
	int64_t compared_source_modified_ = 0;
	TextureCompareView shown_compare_ = TextureCompareView::Off;
	float shown_split_ = 0.5f;
	// The compare of `image` as the view stands: its own texels made a .dds, or a .dds against its import's source.
	std::shared_ptr<const TextureCompression> compare(const ViewportInput &input, const std::shared_ptr<const TextureImage> &image);
};

} // namespace opennova::editor
