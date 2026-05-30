#pragma once

#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/variant/string.hpp>

namespace opennova {

// Case-insensitively resolve a texture filename within `dir` to an existing path.
// For res:// dirs the result is a Godot resource path (imported .tga -> .ctex remap,
// or .pcx/.mdt via ResourceFormatLoaderNovaTexture); for absolute/external dirs it
// is the on-disk path with its real casing.
godot::String resolve_texture_path(const godot::String &dir, const godot::String &filename);

// Resolve + load a texture. res:// paths go through ResourceLoader (works in editor
// and in exported PCKs); absolute/external paths are decoded from raw bytes.
godot::Ref<godot::Texture2D> load_texture_from_dir(const godot::String &dir, const godot::String &filename);

// Case-insensitive lookup of a sidecar file (e.g. a .til) next to `dir`.
godot::String resolve_sidecar_path(const godot::String &dir, const godot::String &filename, const char *ext);

} // namespace opennova
