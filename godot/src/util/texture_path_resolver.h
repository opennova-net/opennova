#pragma once

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <runtime/renderer/material_texture.h>
#include <runtime/renderer/texture_load_rules.h>

#include <functional>
#include <string>

namespace opennova {

// The image a loader makes of one attempt's bytes: the retail reader the attempt
// names (renderer::TextureReader: the TGA reader formats/tga, the PCX reader
// formats/pcx decode_pcx_menu_rgba, Godot's DDS and PNG decoders for the DXT decode
// and the menus' PNG), then the loader's transform. RGBA8, no mips; null when the
// bytes do not decode.
godot::Ref<godot::Image> decode_texture_load(const renderer::TextureLoad &load,
		const godot::PackedByteArray &bytes);

// A loader's image for `name` (renderer::texture_load_attempts over `files`), each
// attempt's bytes from `read`; the first attempt that decodes, else null.
godot::Ref<godot::Image> load_texture_image(renderer::TextureLoader loader, const godot::String &name,
		const renderer::TextureFileQuery &files,
		const std::function<godot::PackedByteArray(const std::string &)> &read);

// The texture the game draws an image with: its mip chain generated.
godot::Ref<godot::Texture2D> texture_with_mipmaps(const godot::Ref<godot::Image> &image);

// A loader's texture for `filename` within `dir`, the one source (no loose-first
// hit): what retail's loader opens, case-insensitively, and nothing else. A res://
// dir loads the exact name through ResourceLoader (the imported texture survives
// export); an absolute dir decodes the raw bytes the retail way.
godot::Ref<godot::Texture2D> load_texture_from_dir(const godot::String &dir, const godot::String &filename,
		renderer::TextureLoader loader);

// Decode one material row's selected file with the loader retail picked for it
// (renderer::material_image_source / plain_texture_load): a DDS keeps its authored
// mip chain, the TGA/MDT and PCX readers build theirs, the loader's transform
// applies. None or undecodable bytes give null.
godot::Ref<godot::Texture2D> load_material_image_from_bytes(
		const renderer::TextureLoad &load, const godot::PackedByteArray &bytes);

// The file a normal-map row (runtime type 4 or 5) opens and its reader
// (renderer::normal_material_filename picked `selected` for `name`): the .dds
// sibling through the DXT decode, a .tga or .mdt through the TGA reader.
renderer::TextureLoad normal_material_load(const godot::String &name, const std::string &selected);

// The highest mip level retail's device samples for a texture bound to an
// object stage: a DDS row keeps its file's chain (no ceiling), every texture
// built from decoded pixels ends at renderer::pixel_texture_mip_levels.
float material_texture_max_lod(const godot::Ref<godot::Texture> &texture);

// Upload the engine's material-specific pixel transform; generated textures
// share the resolver's epoch and shutdown lifetime. A normal map (type 4 or 5)
// is halved to renderer::kNormalMapSideCap.
godot::Ref<godot::Texture> prepare_material_texture(
		const godot::Ref<godot::Texture2D> &source,
		const godot::String &name, uint8_t type);

godot::Ref<godot::Texture> prepare_material_chunk(
        const godot::PackedByteArray &bytes, uint8_t type);

// Case-insensitive lookup of a sidecar file (e.g. a .til) next to `dir`.
godot::String resolve_sidecar_path(const godot::String &dir, const godot::String &filename, const char *ext);

// Case-insensitively resolve a single file `name` (with extension) within `dir`,
// returning the real on-disk path or "" when absent. The one primitive every
// caller (textures, models, scene files, sidecars) shares — no parallel scans.
godot::String resolve_file_in_dir(const godot::String &dir, const godot::String &name);

godot::Ref<godot::Texture> load_material_texture_from_dir(
        const godot::String &dir, const godot::String &name, uint8_t type);

// Drop the per-session directory-index and decoded-texture caches. Call when the
// resource directory changes or its on-disk contents may have changed. Main-thread
// only (the resolver is never called off-thread).
void clear_texture_resolver_caches();

} // namespace opennova
