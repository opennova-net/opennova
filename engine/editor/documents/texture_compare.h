#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <editor/documents/texture_image.h>
#include <formats/pcx/pcx.h>

namespace opennova::editor {

// A texture beside the DXT texture made of it (ADR 0046 S18, the compare): what the compression the game's
// model textures are stored in does to the texels it draws. The reference is the texture's own texels (a TGA,
// an MDT, a PCX or a PNG: what its `.dds` would be made from) or, for a `.dds` an import makes, its source as
// that import prepares it (image_import_texels); the compressed texture is the reference written as the
// `.dds` the image importer writes (encode_image: DXT5 where it holds an alpha, else DXT1, with its full
// chain) and read back as the game reads a DDS (decode_texture: dds_read, its blocks through the D3DX codec's
// port, runtime/renderer/texture_dxt). Each level is weighed against the reference's level of the same sides,
// the reference's chain each level the D3DX box filter of the one before (renderer::box_filter_half, the filter
// the game's texture creator halves with). Tooling, not a port: the error figures are the editor's.

// A peak signal-to-noise ratio where the two are equal.
inline constexpr double kTexturePsnrExact = 99.0;

// One level's error against its reference: the peak signal-to-noise ratio of the colour (its three channels
// together) and of the alpha, in dB; the root mean square error of each; the largest error of a colour
// channel and of the alpha; and the 4 x 4 block (its top-left texel) whose colour error is the largest, a
// DXT block's place, with that block's root mean square error.
struct TextureLevelError {
	uint32_t width = 0, height = 0;
	double psnr_rgb = kTexturePsnrExact, psnr_alpha = kTexturePsnrExact;
	double rms_rgb = 0.0, rms_alpha = 0.0;
	uint8_t max_rgb = 0, max_alpha = 0;
	uint32_t worst_x = 0, worst_y = 0;
	double worst_rms = 0.0;
};
// `test` weighed against `reference`; both of the same sides (an empty error for any other).
TextureLevelError texture_level_error(const TextureLevel &reference, const TextureLevel &test);

struct TextureCompression {
	bool made = false;
	// Why there is none: a texture of no texels, a `.dds` no import makes (it holds its compressed texels alone).
	std::string why;
	// What the reference is, in words: "its own texels", "art/crate.png, its import's source".
	std::string against;
	// The compressed texture's format ("DXT5", "DXT1", a `.dds`'s own) and its file's bytes.
	std::string format;
	uint64_t file_bytes = 0;
	// The reference's levels and the compressed texture's, decoded; the error of each level.
	std::shared_ptr<const TextureImage> reference;
	std::shared_ptr<const TextureImage> compressed;
	std::vector<TextureLevelError> errors;
};

// The texture `image` (decoded) written as the `.dds` its model row's loader reads first: DXT5 where it holds
// an alpha, else DXT1 (`dds` "dxt5" or "dxt1" choosing), with its full chain, against its own texels.
TextureCompression compress_texture(const TextureImage &image, const std::string &dds = std::string());
// A `.dds` (`image`, decoded) against the texels it was made from (`source`: an import's source as its import
// prepares it), `against` in words; refused where the sides differ.
TextureCompression compare_dds(const std::shared_ptr<const TextureImage> &image, const RgbaImage &source,
                               const std::string &against);

// How a compare is drawn: off; the reference left of `split` (a fraction of the width) and the compressed
// right of it; the compressed texture alone; or their difference, each colour channel's |a - b| times the
// gain and the alpha 255 less the alpha's times the gain (an alpha error shows through as the checkerboard).
enum class TextureCompareView : uint8_t { Off, Split, Compressed, Difference };
inline constexpr int kTextureDifferenceGain = 8;
// "off", "split", "dds", "difference": its token on the wire.
const char *texture_compare_view_token(TextureCompareView view);
bool texture_compare_view_from_token(const std::string &token, TextureCompareView &out);
// The picture a compare view draws, every level of it; null for Off or no compression.
std::shared_ptr<const TextureImage> texture_compare_picture(const TextureCompression &compression, TextureCompareView view,
                                                            float split);

// On the wire (the texture viewport's `compare`): `made`, `why`, `against`, `format`, `file_bytes`, `levels` (each
// `level`, `width`, `height`, `psnr_rgb`, `psnr_alpha`, `rms_rgb`, `rms_alpha`, `max_rgb`, `max_alpha`, `worst_block`
// {`x`, `y`, `rms`}).
io::JsonValue texture_compression_json(const TextureCompression &compression);
// A level's error in words: "PSNR 41.2 dB colour, 48.9 dB alpha; largest error 23; worst block at 512, 96".
std::string texture_level_error_words(const TextureLevelError &error, bool alpha);

} // namespace opennova::editor
