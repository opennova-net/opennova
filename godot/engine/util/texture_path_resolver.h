#pragma once

#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <vector>

namespace opennova {

// Candidate texture filenames to probe for a NovaLogic texture reference, in
// priority order.
std::vector<godot::String> texture_candidate_filenames(const godot::String &filename);

// Case-insensitively resolve a texture filename within `dir` to an existing path.
// For res:// dirs the result is a Godot resource path (imported .tga -> .ctex remap,
// or .pcx/.mdt via ResourceFormatLoaderNovaTexture); for absolute/external dirs it
// is the on-disk path with its real casing.
godot::String resolve_texture_path(const godot::String &dir, const godot::String &filename);

// Resolve + load a texture. res:// paths go through ResourceLoader (works in editor
// and in exported PCKs); absolute/external paths are decoded from raw bytes.
godot::Ref<godot::Texture2D> load_texture_from_dir(const godot::String &dir, const godot::String &filename);

// Decode a texture from mounted-resource bytes. Supports .pcx, .tga, .mdt, PNG,
// JPEG, BMP, and DDS (DXT/BC, detected by magic) so archive and loose VFS winners
// follow one decode path without losing the editor-friendly image formats.
godot::Ref<godot::Texture2D> load_texture_from_bytes(const godot::String &filename, const godot::PackedByteArray &bytes);

// Case-insensitive lookup of a sidecar file (e.g. a .til) next to `dir`.
godot::String resolve_sidecar_path(const godot::String &dir, const godot::String &filename, const char *ext);

// Case-insensitively resolve a single file `name` (with extension) within `dir`,
// returning the real on-disk path or "" when absent. The one primitive every
// caller (textures, models, scene files, sidecars) shares — no parallel scans.
godot::String resolve_file_in_dir(const godot::String &dir, const godot::String &name);

// Drop the per-session directory-index and decoded-texture caches. Call when the
// resource directory changes or its on-disk contents may have changed. Main-thread
// only (the resolver is never called off-thread).
void clear_texture_resolver_caches();

} // namespace opennova
