#ifndef OPENNOVA_KDA_RESOURCE_FORMAT_H
#define OPENNOVA_KDA_RESOURCE_FORMAT_H

#include <godot_cpp/classes/resource_format_loader.hpp>
#include <godot_cpp/classes/resource_format_saver.hpp>

namespace godot {

class KdaResourceFormatLoader : public ResourceFormatLoader {
	GDCLASS(KdaResourceFormatLoader, ResourceFormatLoader);

protected:
	static void _bind_methods() {}

public:
	PackedStringArray _get_recognized_extensions() const override;
	bool _handles_type(const StringName &p_type) const override;
	String _get_resource_type(const String &p_path) const override;
	Variant _load(const String &p_path, const String &p_original_path, bool p_use_sub_threads,
	              int32_t p_cache_mode) const override;
};

class KdaResourceFormatSaver : public ResourceFormatSaver {
	GDCLASS(KdaResourceFormatSaver, ResourceFormatSaver);

protected:
	static void _bind_methods() {}

public:
	Error _save(const Ref<Resource> &p_resource, const String &p_path, uint32_t p_flags) override;
	bool _recognize(const Ref<Resource> &p_resource) const override;
	PackedStringArray _get_recognized_extensions(const Ref<Resource> &p_resource) const override;
};

}  // namespace godot

#endif  // OPENNOVA_KDA_RESOURCE_FORMAT_H
