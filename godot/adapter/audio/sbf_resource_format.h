#ifndef SBF_RESOURCE_FORMAT_H
#define SBF_RESOURCE_FORMAT_H

#include <godot_cpp/classes/resource_format_loader.hpp>
#include <godot_cpp/classes/resource_format_saver.hpp>
#include <godot_cpp/core/class_db.hpp>

namespace godot {

class SbfResourceFormatLoader : public ResourceFormatLoader {
	GDCLASS(SbfResourceFormatLoader, ResourceFormatLoader)

protected:
	static void _bind_methods() {}

public:
	PackedStringArray _get_recognized_extensions() const override;
	bool _handles_type(const StringName &p_type) const override;
	String _get_resource_type(const String &p_path) const override;
	Variant _load(const String &p_path, const String &p_original_path,
			bool p_use_sub_threads, int32_t p_cache_mode) const override;
};

// Stub saver. _save returns ERR_UNAVAILABLE; Phase F implements the raw /
// re-encode passthrough. _recognize + extensions are wired now so editor
// inspectors that ask "can I save this resource as .sbf?" answer truthfully
// and our class registers without surprising the resource cache.
class SbfResourceFormatSaver : public ResourceFormatSaver {
	GDCLASS(SbfResourceFormatSaver, ResourceFormatSaver)

protected:
	static void _bind_methods() {}

public:
	Error _save(const Ref<Resource> &p_resource, const String &p_path, uint32_t p_flags) override;
	bool _recognize(const Ref<Resource> &p_resource) const override;
	PackedStringArray _get_recognized_extensions(const Ref<Resource> &p_resource) const override;
};

} // namespace godot

#endif // SBF_RESOURCE_FORMAT_H
