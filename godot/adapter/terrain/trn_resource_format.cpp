#include "trn_resource_format.h"
#include "nova_terrain_data.h"
#include "util/nova_data_format.h"
#include "util/texture_path_resolver.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <sstream>

using namespace godot;

// ---------------------------------------------------------------------------
// Loader
// ---------------------------------------------------------------------------

PackedStringArray ResourceFormatLoaderTRN::_get_recognized_extensions() const {
	PackedStringArray exts;
	exts.push_back("trn");
	return exts;
}

bool ResourceFormatLoaderTRN::_handles_type(const StringName &p_type) const {
	return p_type == StringName("NovaTerrainData") || p_type == StringName("Resource");
}

String ResourceFormatLoaderTRN::_get_resource_type(const String &p_path) const {
	if (p_path.get_extension().to_lower() == "trn") {
		return "NovaTerrainData";
	}
	return "";
}

PackedStringArray ResourceFormatLoaderTRN::_get_dependencies(const String &p_path, bool p_add_types) const {
	PackedStringArray deps;

	PackedByteArray bytes;
	if (!read_nova_payload_file(p_path, bytes)) return deps;
	std::string content(reinterpret_cast<const char *>(bytes.ptr()), static_cast<size_t>(bytes.size()));

	opennova::TrnConfig trn;
	std::string error;
	std::istringstream stream(content);
	if (!opennova::load_trn(stream, trn, error)) return deps;

	String dir = p_path.get_base_dir();
	auto add = [&](const std::string& name) {
		if (!name.empty()) deps.push_back(dir.path_join(String(name.c_str())));
	};

	add(trn.polydata);
	add(trn.colormap);
	add(trn.detailmap);
	add(trn.detailmap_c1);
	add(trn.detailmap_c2);
	add(trn.detailmap_c3);
	add(trn.detailmap2);
	add(trn.detailmapdist);
	add(trn.detailmapdist2);
	add(trn.detailblendmap);
	add(trn.charmap);
	add(trn.foliagemap);
	add(trn.tilestrip);
	if (!trn.tileinfo.empty()) {
		const String tileinfo = String(trn.tileinfo.c_str());
		const String resolved = opennova::resolve_sidecar_path(dir, tileinfo, "til");
		if (!resolved.is_empty()) {
			deps.push_back(resolved);
		} else if (tileinfo.get_extension().to_lower() == "til") {
			deps.push_back(dir.path_join(tileinfo));
		} else {
			deps.push_back(dir.path_join(tileinfo + String(".til")));
		}
	}

	// Foliage .3di models (transitively pulls in their textures)
	for (const auto& def : trn.foliage_defs) {
		add(def.graphic);
	}

	return deps;
}

Variant ResourceFormatLoaderTRN::_load(const String &p_path, const String &p_original_path,
                                        bool p_use_sub_threads, int32_t p_cache_mode) const {
	Ref<NovaTerrainData> data;
	data.instantiate();
	data->set_trn_path(p_path);
	Error err = data->load();
	if (err != OK) {
		return Variant();
	}
	return data;
}

// ---------------------------------------------------------------------------
// Saver
// ---------------------------------------------------------------------------

Error ResourceFormatSaverTRN::_save(const Ref<Resource> &p_resource, const String &p_path, uint32_t p_flags) {
	Ref<NovaTerrainData> data = p_resource;
	if (data.is_null()) return ERR_INVALID_PARAMETER;

	opennova::TrnConfig trn_copy = data->get_trn();
	std::ostringstream oss;
	std::string error;
	if (!opennova::save_trn(oss, trn_copy, error)) {
		UtilityFunctions::push_warning("ResourceFormatSaverTRN: ", error.c_str());
		return ERR_FILE_CANT_WRITE;
	}

	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::WRITE);
	if (f.is_null()) return ERR_FILE_CANT_WRITE;
	std::string content = oss.str();
	f->store_string(String::utf8(content.c_str(), content.size()));
	f->close();
	return OK;
}

bool ResourceFormatSaverTRN::_recognize(const Ref<Resource> &p_resource) const {
	return Object::cast_to<NovaTerrainData>(p_resource.ptr()) != nullptr;
}

PackedStringArray ResourceFormatSaverTRN::_get_recognized_extensions(const Ref<Resource> &p_resource) const {
	PackedStringArray exts;
	if (_recognize(p_resource)) {
		exts.push_back("trn");
	}
	return exts;
}
