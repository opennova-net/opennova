#include "tpj_resource_format.h"

#include "nova_terrain_project.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <tpj/tpj_io.h>

#include <sstream>
#include <string>

using namespace godot;

PackedStringArray ResourceFormatLoaderTPJ::_get_recognized_extensions() const {
	PackedStringArray exts;
	exts.push_back("tpj");
	return exts;
}

bool ResourceFormatLoaderTPJ::_handles_type(const StringName &p_type) const {
	return p_type == StringName("NovaTerrainProject") || p_type == StringName("Resource");
}

String ResourceFormatLoaderTPJ::_get_resource_type(const String &p_path) const {
	if (p_path.get_extension().to_lower() == "tpj") {
		return "NovaTerrainProject";
	}
	return "";
}

Variant ResourceFormatLoaderTPJ::_load(const String &p_path, const String &p_original_path,
                                       bool p_use_sub_threads, int32_t p_cache_mode) const {
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
	if (file.is_null()) {
		return Variant();
	}

	std::istringstream input(file->get_as_text().utf8().get_data());
	file.unref();

	opennova::TpjProject project;
	std::string error;
	if (!opennova::load_tpj(input, project, error)) {
		return Variant();
	}

	Ref<NovaTerrainProject> resource;
	resource.instantiate();
	resource->copy_from_native(project);
	return resource;
}

Error ResourceFormatSaverTPJ::_save(const Ref<Resource> &p_resource, const String &p_path, uint32_t p_flags) {
	Ref<NovaTerrainProject> project = p_resource;
	if (project.is_null()) {
		return ERR_INVALID_PARAMETER;
	}

	std::ostringstream output;
	std::string error;
	if (!opennova::save_tpj(output, project->to_native(), error)) {
		UtilityFunctions::printerr("ResourceFormatSaverTPJ: ", error.c_str());
		return ERR_FILE_CANT_WRITE;
	}

	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE);
	if (file.is_null()) {
		return ERR_FILE_CANT_WRITE;
	}

	const std::string contents = output.str();
	file->store_string(String::utf8(contents.c_str(), static_cast<int64_t>(contents.size())));
	file->close();
	return OK;
}

bool ResourceFormatSaverTPJ::_recognize(const Ref<Resource> &p_resource) const {
	return Object::cast_to<NovaTerrainProject>(p_resource.ptr()) != nullptr;
}

PackedStringArray ResourceFormatSaverTPJ::_get_recognized_extensions(const Ref<Resource> &p_resource) const {
	PackedStringArray exts;
	if (_recognize(p_resource)) {
		exts.push_back("tpj");
	}
	return exts;
}
