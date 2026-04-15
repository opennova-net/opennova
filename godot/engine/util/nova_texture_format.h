#pragma once

#include <godot_cpp/classes/resource_format_loader.hpp>
#include <godot_cpp/core/class_db.hpp>

namespace godot {

// ResourceFormatLoader for texture formats without built-in Godot importers.
// .pcx — 8-bit paletted images (decoded to RGB8)
// .mdt — NovaLogic's renamed TGA format
class ResourceFormatLoaderNovaTexture : public ResourceFormatLoader {
	GDCLASS(ResourceFormatLoaderNovaTexture, ResourceFormatLoader)

protected:
	static void _bind_methods() {}

public:
	PackedStringArray _get_recognized_extensions() const override;
	bool _handles_type(const StringName &p_type) const override;
	String _get_resource_type(const String &p_path) const override;
	Variant _load(const String &p_path, const String &p_original_path, bool p_use_sub_threads, int32_t p_cache_mode) const override;
};

} // namespace godot
