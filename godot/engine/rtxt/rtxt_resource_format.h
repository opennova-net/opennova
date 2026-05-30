#pragma once

#include <godot_cpp/classes/resource_format_loader.hpp>
#include <godot_cpp/classes/resource_format_saver.hpp>
#include <godot_cpp/core/class_db.hpp>

namespace godot {

// Loads NovaLogic strings/*.bin localized string tables into RtxtStringFile.
// .bin is an ambiguous extension, so recognition is gated on the 'RTXT' magic;
// non-RTXT .bin files fall through to other loaders.
class ResourceFormatLoaderRTXT : public ResourceFormatLoader {
	GDCLASS(ResourceFormatLoaderRTXT, ResourceFormatLoader)

protected:
	static void _bind_methods() {}

public:
	PackedStringArray _get_recognized_extensions() const override;
	bool _handles_type(const StringName &p_type) const override;
	String _get_resource_type(const String &p_path) const override;
	Variant _load(const String &p_path, const String &p_original_path, bool p_use_sub_threads, int32_t p_cache_mode) const override;
};

class ResourceFormatSaverRTXT : public ResourceFormatSaver {
	GDCLASS(ResourceFormatSaverRTXT, ResourceFormatSaver)

protected:
	static void _bind_methods() {}

public:
	Error _save(const Ref<Resource> &p_resource, const String &p_path, uint32_t p_flags) override;
	bool _recognize(const Ref<Resource> &p_resource) const override;
	PackedStringArray _get_recognized_extensions(const Ref<Resource> &p_resource) const override;
};

} // namespace godot
