#pragma once

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <runtime/renderer/material_texture.h>
#include <runtime/renderer/texture_load_rules.h>

#include <functional>
#include <string>
#include <vector>

namespace opennova {

// The image a loader makes of one attempt's bytes: the retail reader the attempt
// names (renderer::TextureReader: the TGA reader formats/tga in its archive or
// particle-loose form, the PCX reader formats/pcx decode_pcx_menu_rgba, the DDS
// reader's content sniff over Godot's BMP/DDS/JPEG/PNG/TGA decoders, the menus' PNG),
// then the loader's transform. RGBA8; only a DDS keeps a mip chain, its file's; null
// when the bytes do not decode.
godot::Ref<godot::Image> decode_texture_load(const renderer::TextureLoad &load,
		const godot::PackedByteArray &bytes);

// Reads one attempt's bytes from its source (a mounted file, or a loose file in the
// particle folder); empty when there is none.
using TextureReadFn = std::function<godot::PackedByteArray(const renderer::TextureLoad &)>;

// The image of the first of a loader's attempts (renderer::texture_load_attempts)
// that reads and decodes, else null; `r_alpha_only` reports whether the winning
// attempt is alpha-only (the HUD's alpha material), `r_reader` its reader.
godot::Ref<godot::Image> load_texture_image(const std::vector<renderer::TextureLoad> &attempts,
		const TextureReadFn &read, bool *r_alpha_only = nullptr,
		renderer::TextureReader *r_reader = nullptr);

// A loader's resolved attempts as a cache identity: each attempt's source, reader,
// transform, mode and file (lower-cased: the file sets are case-insensitive, and the
// rules that read the name's case have already decided).
std::string texture_load_key(const std::vector<renderer::TextureLoad> &attempts);

// The pixels the game's texture of an image holds, on the CPU: decompressed, the mip
// chain a DDS carries, else generated; null for no image. `image` is changed in place.
godot::Ref<godot::Image> image_with_mipmaps(const godot::Ref<godot::Image> &image);

// The texture the game draws an image with: image_with_mipmaps's, uploaded.
godot::Ref<godot::Texture2D> texture_with_mipmaps(const godot::Ref<godot::Image> &image);

// load_texture_with_mipmaps's pixels made on the CPU alone (image_with_mipmaps over the
// first attempt that decodes), no device texture made and none read back; uncached.
// `r_reader` takes the reader that decoded it.
godot::Ref<godot::Image> load_image_with_mipmaps(const std::vector<renderer::TextureLoad> &attempts,
		const TextureReadFn &read, renderer::TextureReader *r_reader = nullptr);

// load_texture_with_mipmaps's texture made of those pixels (`image` its levels, `reader` the
// one that decoded it): the image uploaded; a DDS reader's remembered as D3DX's chain for
// texture_max_lod. Null for no image.
godot::Ref<godot::Texture2D> upload_texture_with_mipmaps(const godot::Ref<godot::Image> &image,
		renderer::TextureReader reader);

// A loader's texture: the first of its attempts that reads and decodes
// (load_texture_image), with its mip chain (texture_with_mipmaps). A texture the
// DDS reader decoded is remembered as D3DX's chain for texture_max_lod.
godot::Ref<godot::Texture2D> load_texture_with_mipmaps(const std::vector<renderer::TextureLoad> &attempts,
		const TextureReadFn &read);

// A loader's texture for `filename` within `dir`, the one source (no loose-first
// hit): what retail's loader opens, case-insensitively, and nothing else. A res://
// dir loads the exact name through ResourceLoader (the imported texture survives
// export); an absolute dir decodes the raw bytes the retail way.
godot::Ref<godot::Texture2D> load_texture_from_dir(const godot::String &dir, const godot::String &filename,
		renderer::TextureLoader loader);

// load_texture_from_dir's pixels with their mip chain: an absolute dir's decoded on the
// CPU (load_image_with_mipmaps, uncached); a res:// dir's imported texture's image.
godot::Ref<godot::Image> load_image_from_dir(const godot::String &dir, const godot::String &filename,
		renderer::TextureLoader loader);

// The highest mip level retail's device samples for a texture: one D3DX built
// (a DDS row's or sibling's) keeps its chain (no ceiling, kUnboundedMaxLod);
// every texture built from decoded pixels ends at its creation chain's last
// level (renderer::pixel_texture_last_level under `creation_flags`; a model
// stage's word carries no chain cap).
inline constexpr float kUnboundedMaxLod = 1000.0f;
float texture_max_lod(const godot::Ref<godot::Texture> &texture, uint32_t creation_flags = 0);

// The texture a material row's loader makes of its file: `load` the file and reader it picked
// (renderer::material_texture_source, as a load: renderer::material_texture_load), `read` that
// file's bytes. The file is decoded with that reader, never by its name (the DDS reader takes
// what D3DX takes by content, renderer::dds_reader_codec_order, and keeps its authored mip
// chain; the TGA reader, .tga and .mdt files, a TGA alone; the PCX reader a PCX), then made
// into one device texture: the decoded file with its chain, or the engine's material-specific
// pixel transform of the decoded pixels (a normal map, type 4 or 5, or the occlusion producer
// halved to renderer::kNormalMapSideCap), never a device texture read back (engine
// renderer/material_texture.h carries the witness). Null when the loader makes nothing (no
// file decoded, a type or name no producer takes: nothing is read for those), where the
// dispatcher binds missing_material_texture instead: what a row's loader makes is what the
// texture registry keeps (renderer/texture_registry.h), the checkerboard never.
godot::Ref<godot::Texture> make_material_texture(const renderer::TextureLoad &load,
		const std::function<godot::PackedByteArray()> &read, const godot::String &name, uint8_t type);

// A chunk producer's texture (types 16 to 18); null when the chunk does not load.
godot::Ref<godot::Texture> prepare_material_chunk(
        const godot::PackedByteArray &bytes, uint8_t type);

// The dispatcher's missing-texture checkerboard, bound where a row's loader made
// nothing (renderer::missing_material_texture_rgba).
godot::Ref<godot::Texture> missing_material_texture();

// The flat normal (one 1x1 texel of (0.5, 0.5, 1, 1)) an object material binds when its row has no
// normal-map stage: one texture for every such material, made once, never one per material.
godot::Ref<godot::Texture> flat_normal_material_texture();

// Case-insensitive lookup of a sidecar file (e.g. a .til) next to `dir`.
godot::String resolve_sidecar_path(const godot::String &dir, const godot::String &filename, const char *ext);

// Case-insensitively resolve a single file `name` (with extension) within `dir`,
// returning the real on-disk path or "" when absent. The one primitive every
// caller (textures, models, scene files, sidecars) shares — no parallel scans.
godot::String resolve_file_in_dir(const godot::String &dir, const godot::String &name);

// One material row's texture of runtime `type` from `dir`: the texture registered under
// the row's key in that directory's registry (renderer::texture_registry_key: the first
// row of a key to load decides it), else what the row's own loader makes, registered;
// the checkerboard when that loads nothing.
godot::Ref<godot::Texture> load_material_texture_from_dir(
        const godot::String &dir, const godot::String &name, uint8_t type);

// Drop the per-session directory-index and decoded-texture caches. Call when the
// resource directory changes or its on-disk contents may have changed. Main-thread
// only (the resolver is never called off-thread).
void clear_texture_resolver_caches();

} // namespace opennova
