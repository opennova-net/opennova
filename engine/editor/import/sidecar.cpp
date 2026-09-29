#include <editor/import/sidecar.h>

#include <filesystem>
#include <system_error>

#include <base/io/hash.h>
#include <base/io/json.h>
#include <editor/project/project_files.h>

namespace opennova::editor {

namespace {

std::string sidecar_text(const ImportSidecar &sidecar) {
	io::JsonValue json = io::JsonValue::make_object();
	json.set("schema_version", io::JsonValue::make_number(kImportSidecarSchemaVersion));
	json.set("importer", io::JsonValue::make_string(sidecar.importer));
	json.set("version", io::JsonValue::make_number(sidecar.version));
	io::JsonValue options = io::JsonValue::make_object();
	for (const auto &option : sidecar.options) options.set(option.first, io::JsonValue::make_string(option.second));
	json.set("options", std::move(options));
	json.set("source_hash", io::JsonValue::make_string(io::hex64(sidecar.source_hash)));
	io::JsonValue outputs = io::JsonValue::make_array();
	for (const std::string &output : sidecar.outputs) outputs.push(io::JsonValue::make_string(output));
	json.set("outputs", std::move(outputs));
	return io::json_write(json);
}

} // namespace

bool load_import_sidecar(const std::string &path, ImportSidecar &out, Diagnostic &error) {
	error = Diagnostic();
	std::error_code ec;
	if (!std::filesystem::exists(path, ec)) return false;
	std::string text, message;
	if (!read_file_text(path, text, message)) {
		error = make_diagnostic(DiagnosticSeverity::Error, "import.sidecar", message, path);
		return false;
	}
	io::JsonValue json;
	if (!io::json_parse(text, json, message) || !json.is_object()) {
		error = make_diagnostic(DiagnosticSeverity::Error, "import.sidecar", "The import record is not valid JSON: " + message, path);
		return false;
	}
	if (json.get_int("schema_version", -1) != kImportSidecarSchemaVersion) {
		error = make_diagnostic(DiagnosticSeverity::Error, "import.sidecar",
		                        "The import record has an unknown schema version.", path);
		return false;
	}
	ImportSidecar sidecar;
	sidecar.importer = json.get_string("importer", "");
	sidecar.version = json.get_int("version", 0);
	if (const io::JsonValue *options = json.get("options"); options && options->is_object())
		for (const io::JsonMember &member : options->object)
			if (member.value.is_string()) sidecar.options[member.key] = member.value.string;
	if (!io::parse_hex64(json.get_string("source_hash", ""), sidecar.source_hash)) sidecar.source_hash = 0;
	if (const io::JsonValue *outputs = json.get("outputs"); outputs && outputs->is_array())
		for (const io::JsonValue &output : outputs->array)
			if (output.is_string()) sidecar.outputs.push_back(output.string);
	if (sidecar.importer.empty()) {
		error = make_diagnostic(DiagnosticSeverity::Error, "import.sidecar", "The import record names no importer.", path);
		return false;
	}
	out = std::move(sidecar);
	return true;
}

bool save_import_sidecar(const std::string &path, const ImportSidecar &sidecar, Diagnostic &error) {
	std::string message;
	if (!write_file_atomic(path, sidecar_text(sidecar), message)) {
		error = make_diagnostic(DiagnosticSeverity::Error, "import.sidecar", message, path);
		return false;
	}
	return true;
}

uint64_t import_sidecar_fingerprint(const ImportSidecar &sidecar) {
	const std::string text = sidecar_text(sidecar);
	return io::fnv1a64_bytes(io::kFnv1a64Offset, text.data(), text.size());
}

} // namespace opennova::editor
