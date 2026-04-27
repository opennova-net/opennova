#pragma once

#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/variant/string.hpp>

namespace opennova {

godot::String resolve_texture_path(const godot::String &dir, const godot::String &filename);
godot::Ref<godot::Texture2D> load_texture_from_dir(const godot::String &dir, const godot::String &filename);
godot::String resolve_asset_path(const godot::String &dir, const godot::String &name, const char *ext);

} // namespace opennova
