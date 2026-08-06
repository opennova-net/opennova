#include "til_resource_format.h"

#include "nova_terrain_tile_info.h"
#include "util/nova_data_format.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <til/til_io.h>

#include <cstring>
#include <string>
#include <vector>

using namespace godot;

PackedStringArray ResourceFormatLoaderTIL::_get_recognized_extensions() const {
	PackedStringArray exts;
	exts.push_back("til");
	return exts;
}

bool ResourceFormatLoaderTIL::_handles_type(const StringName &p_type) const {
	return p_type == StringName("NovaTerrainTileInfo") || p_type == StringName("Resource");
}

String ResourceFormatLoaderTIL::_get_resource_type(const String &p_path) const {
	if (p_path.get_extension().to_lower() == "til") {
		return "NovaTerrainTileInfo";
	}
	return "";
}

Variant ResourceFormatLoaderTIL::_load(const String &p_path, const String &p_original_path,
                                       bool p_use_sub_threads, int32_t p_cache_mode) const {
	PackedByteArray packed;
	if (!read_nova_payload_file(p_path, packed)) {
		return Variant();
	}

	std::vector<uint8_t> bytes(static_cast<size_t>(packed.size()));
	if (!bytes.empty()) {
		std::memcpy(bytes.data(), packed.ptr(), bytes.size());
	}

	opennova::TilFile til;
	std::string error;
	if (!opennova::load_til(bytes.data(), bytes.size(), til, error)) {
		UtilityFunctions::push_warning("ResourceFormatLoaderTIL: ", error.c_str());
		return Variant();
	}

	Ref<NovaTerrainTileInfo> resource;
	resource.instantiate();
	resource->copy_from_native(til);
	return resource;
}

Error ResourceFormatSaverTIL::_save(const Ref<Resource> &p_resource, const String &p_path, uint32_t p_flags) {
	Ref<NovaTerrainTileInfo> tile_info = p_resource;
	if (tile_info.is_null()) {
		return ERR_INVALID_PARAMETER;
	}

	std::vector<uint8_t> bytes;
	std::string error;
	if (!opennova::save_til(tile_info->to_native(), bytes, error)) {
		UtilityFunctions::push_warning("ResourceFormatSaverTIL: ", error.c_str());
		return ERR_FILE_CANT_WRITE;
	}

	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE);
	if (file.is_null()) {
		return ERR_FILE_CANT_WRITE;
	}

	PackedByteArray packed;
	packed.resize(static_cast<int64_t>(bytes.size()));
	if (!bytes.empty()) {
		std::memcpy(packed.ptrw(), bytes.data(), bytes.size());
	}
	file->store_buffer(packed);
	file->close();
	return OK;
}

bool ResourceFormatSaverTIL::_recognize(const Ref<Resource> &p_resource) const {
	return Object::cast_to<NovaTerrainTileInfo>(p_resource.ptr()) != nullptr;
}

PackedStringArray ResourceFormatSaverTIL::_get_recognized_extensions(const Ref<Resource> &p_resource) const {
	PackedStringArray exts;
	if (_recognize(p_resource)) {
		exts.push_back("til");
	}
	return exts;
}
