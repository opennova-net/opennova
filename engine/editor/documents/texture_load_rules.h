#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include <editor/documents/texture_image.h>
#include <editor/documents/texture_roles.h>

namespace opennova::editor {

// The file a texture loader opens for a name, the reader that decodes it, and what the loader makes of
// the texels after (ADR 0046 S18), as the editor's checks, uses and previews read them: the game's own
// rules (runtime/renderer/texture_load_rules.h, its witnesses: renderer::texture_load_attempts by the
// loader a role goes through, texture_roles.h texture_role_renderer_loader; a model row's
// renderer::material_texture_source and material_texture_load) in the editor's words, and the texels a
// use makes for its picture. The project's files are packed as the game mounts them, so no loose-first
// search applies, and the particle manager's loose tga\ folder is no project file.

enum class TextureFileReader : uint8_t { None, Tga, Pcx, Dds, Png, Pcx8, Chunk };
const char *texture_file_reader_token(TextureFileReader reader);

// What a loader does to the texels its reader decoded.
enum class TextureLoadTransform : uint8_t {
	None,
	LuminanceAlpha, // ARCHIVE over a PCX: alpha = (85 x (r + g + b)) >> 8 of each texel's palette entry
	WhiteAlphaFromBlue, // HUD, FILE (flag 0x200000), PLAIN's upper-case .PCX: white, alpha = the blue byte
	AlphaOnly, // HUD alpha mode: an A8 texture of the alpha alone, drawn white by the HUD's alpha material
	NormalFromHeight, // NORMAL over a .tga: the height in alpha converted to a normal map
	BlueAlphaOnly, // HUD alpha mode over a PCX: white with the blue as alpha, then that alpha alone (an A8)
	kCount,
};
const char *texture_load_transform_token(TextureLoadTransform transform);
// What a transform makes of a texture, in a modder's words ("" for none).
const char *texture_load_transform_words(TextureLoadTransform transform);

struct TextureLoad {
	std::string file; // the name the loader opens ("" none)
	TextureFileReader reader = TextureFileReader::None;
	TextureLoadTransform transform = TextureLoadTransform::None;
	// ARCHIVE's second name: a PCX whose palette's luminance is the alpha (the name itself, the sky
	// maps' and the tracer smoke's caller passes it twice).
	std::string alpha_source;
};

using TextureNameTest = std::function<bool(const std::string &name)>;

// The load of `name` by `loader` for a use of `role` (kCount: none known): `exists` answers whether the
// project's files hold a name (the loaders probe a .dds sibling and test the file); `row_type` a model
// texture row's authored type (the STAGE, PLAIN, NORMAL, producer and chunk loaders pick by its runtime
// type, renderer::material_texture_source); `alpha_mode` the HUD caller's mode (1 alpha only, 0 colour,
// -1 the role's), which a .FULL or .ALPHA suffix overrides. Of a loader's tries the first the files hold,
// else its first: the file it would open.
TextureLoad texture_load(TextureLoader loader, std::string_view name, const TextureNameTest &exists,
                         uint8_t row_type = 0, int alpha_mode = -1, TextureRoleId role = TextureRoleId::kCount);

// The load a texture reference makes by its loader argument (texture_roles.h): a model row's by the row's
// type; a role's by its loader (a mission's tile set named with TGA for its extension first, the HUD's
// alpha-only art in its alpha mode); a use whose loader is not witnessed yet (-1), the name as written
// with no reader known.
TextureLoad texture_reference_load(std::string_view name, int32_t loader_arg, const TextureNameTest &exists);

// The texels the game makes of `image` for a load (a copy with the transform applied to every level):
// what a role's picture is "as the game draws it". `alpha_source` the palette ARCHIVE reads its alpha
// from (none: the image's own palette).
std::shared_ptr<const TextureImage> apply_load_transform(const TextureImage &image, TextureLoadTransform transform,
                                                         const TextureImage *alpha_source = nullptr);

} // namespace opennova::editor
