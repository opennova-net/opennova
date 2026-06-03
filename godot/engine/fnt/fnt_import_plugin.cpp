#include "fnt/fnt_import_plugin.h"

#include "fnt/nova_fnt_resource.h"
#include "util/nova_data_format.h"

#include <godot_cpp/classes/resource_saver.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

void NovaFntImportPlugin::_bind_methods() {}

String NovaFntImportPlugin::_get_importer_name() const {
	return "opennova.fnt";
}

String NovaFntImportPlugin::_get_visible_name() const {
	return "Nova FNT";
}

int32_t NovaFntImportPlugin::_get_preset_count() const {
	return 1;
}

String NovaFntImportPlugin::_get_preset_name(int32_t p_preset_index) const {
	return "Default";
}

PackedStringArray NovaFntImportPlugin::_get_recognized_extensions() const {
	PackedStringArray extensions;
	extensions.push_back("fnt");
	return extensions;
}

TypedArray<Dictionary> NovaFntImportPlugin::_get_import_options(const String &p_path, int32_t p_preset_index) const {
	return TypedArray<Dictionary>();
}

String NovaFntImportPlugin::_get_save_extension() const {
	return "fnt";
}

String NovaFntImportPlugin::_get_resource_type() const {
	return "NovaFntResource";
}

float NovaFntImportPlugin::_get_priority() const {
	return 10.0f;
}

int32_t NovaFntImportPlugin::_get_import_order() const {
	return 0;
}

bool NovaFntImportPlugin::_get_option_visibility(const String &p_path, const StringName &p_option_name, const Dictionary &p_options) const {
	return true;
}

Error NovaFntImportPlugin::_import(const String &p_source_file, const String &p_save_path, const Dictionary &p_options, const TypedArray<String> &p_platform_variants, const TypedArray<String> &p_gen_files) const {
	PackedByteArray bytes;
	if (!read_nova_payload_file(p_source_file, bytes)) {
		return ERR_FILE_CANT_OPEN;
	}

	if (bytes.size() < 4 || bytes[0] != 'F' || bytes[1] != 'N' || bytes[2] != 'T' || bytes[3] != '0') {
		return ERR_FILE_UNRECOGNIZED;
	}

	Ref<NovaFntResource> font;
	font.instantiate();
	const Error load_err = font->load_from_bytes(bytes);
	if (load_err != OK) {
		UtilityFunctions::push_error("NovaFntImportPlugin: failed to parse Nova .fnt: ", p_source_file);
		return load_err;
	}

	const String target_path = p_save_path + String(".") + _get_save_extension();
	return ResourceSaver::get_singleton()->save(font, target_path);
}

bool NovaFntImportPlugin::_can_import_threaded() const {
	return true;
}
