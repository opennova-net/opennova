#ifndef MUS_RESOURCE_FORMAT_H
#define MUS_RESOURCE_FORMAT_H

// MUS .bin loader. Recognises files whose generic decoded body is a valid
// SCR0 music script (MusFileHeader.magic).

#include <godot_cpp/classes/resource_format_loader.hpp>
#include <godot_cpp/classes/resource_format_saver.hpp>
#include <godot_cpp/core/class_db.hpp>

namespace godot {

class MusResourceFormatLoader : public ResourceFormatLoader {
	GDCLASS(MusResourceFormatLoader, ResourceFormatLoader)

protected:
	static void _bind_methods() {}

public:
	PackedStringArray _get_recognized_extensions() const override;
	bool _handles_type(const StringName &p_type) const override;
	String _get_resource_type(const String &p_path) const override;
	Variant _load(const String &p_path, const String &p_original_path,
			bool p_use_sub_threads, int32_t p_cache_mode) const override;
};

// Saver: passthrough only for v1. Writes the decoded SCR0 form back to disk
// via NovaMusicScript::get_raw_file_bytes().
class MusResourceFormatSaver : public ResourceFormatSaver {
	GDCLASS(MusResourceFormatSaver, ResourceFormatSaver)

protected:
	static void _bind_methods() {}

public:
	Error _save(const Ref<Resource> &p_resource, const String &p_path, uint32_t p_flags) override;
	bool _recognize(const Ref<Resource> &p_resource) const override;
	PackedStringArray _get_recognized_extensions(const Ref<Resource> &p_resource) const override;
};

} // namespace godot

#endif // MUS_RESOURCE_FORMAT_H
