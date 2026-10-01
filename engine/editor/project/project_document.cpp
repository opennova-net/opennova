#include <editor/project/project_document.h>

#include <cstdio>
#include <filesystem>
#include <random>
#include <system_error>

#include <base/gameprofile/gameprofile.h>
#include <base/io/strutil.h>
#include <editor/model/diagnostic.h>
#include <editor/project/project_files.h>

namespace fs = std::filesystem;

namespace opennova::editor {

namespace {

std::string join(const std::string &root, const char *leaf) {
	return (fs::path(root) / leaf).generic_string();
}

bool fail(Diagnostic &error, CoreFinding code, std::string message) {
	error = make_finding(code, DiagnosticSeverity::Error, std::move(message));
	return false;
}

} // namespace

ProjectPaths ProjectPaths::for_root(const std::string &root) {
	ProjectPaths p;
	p.root = fs::path(root).generic_string();
	p.project_file = join(p.root, kProjectFileName);
	p.cache_dir = join(p.root, kProjectCacheDirName);
	p.local_settings_file = (fs::path(p.cache_dir) / kLocalSettingsFileName).generic_string();
	p.imported_dir = (fs::path(p.cache_dir) / "imported").generic_string();
	p.import_cache_file = (fs::path(p.cache_dir) / kImportCacheFileName).generic_string();
	p.index_dir = (fs::path(p.cache_dir) / "index").generic_string();
	p.build_dir = (fs::path(p.cache_dir) / "build").generic_string();
	p.build_cache_file = (fs::path(p.cache_dir) / kBuildCacheFileName).generic_string();
	p.run_dir = (fs::path(p.cache_dir) / "run").generic_string();
	p.staging_dir = (fs::path(p.cache_dir) / "staging").generic_string();
	return p;
}

std::string ProjectPaths::export_dir(const ProjectDocument &doc) const {
	const fs::path output(doc.export_settings.output);
	if (output.is_absolute()) return output.generic_string();
	return (fs::path(root) / output).lexically_normal().generic_string();
}

bool ensure_project_cache_dir(const ProjectPaths &paths, std::string &error) {
	if (!ensure_directory(paths.cache_dir, error)) return false;
	const std::string gitignore = (fs::path(paths.cache_dir) / ".gitignore").generic_string();
	std::error_code ec;
	if (fs::is_regular_file(gitignore, ec)) return true;
	return write_file_atomic(gitignore, std::string("*\n"), error);
}

io::JsonValue project_document_to_json(const ProjectDocument &doc) {
	io::JsonValue json = io::JsonValue::make_object();
	json.set("schema_version", io::JsonValue::make_number(doc.schema_version));
	json.set("project_id", io::JsonValue::make_string(doc.project_id));
	json.set("title", io::JsonValue::make_string(doc.title));
	json.set("target_game", io::JsonValue::make_string(doc.target_game));
	io::JsonValue features = io::JsonValue::make_object();
	features.set("menu", io::JsonValue::make_bool(doc.features.menu));
	features.set("mission", io::JsonValue::make_bool(doc.features.mission));
	features.set("multiplayer", io::JsonValue::make_bool(doc.features.multiplayer));
	json.set("features", std::move(features));
	io::JsonValue export_settings = io::JsonValue::make_object();
	export_settings.set("output", io::JsonValue::make_string(doc.export_settings.output));
	export_settings.set("include_runtime",
	                    io::JsonValue::make_bool(doc.export_settings.include_runtime));
	json.set("export", std::move(export_settings));
	return json;
}

bool project_document_from_json(const io::JsonValue &json, ProjectDocument &out, Diagnostic &error) {
	if (!json.is_object()) return fail(error, CoreFinding::ProjectJson, "The project file is not a JSON object.");
	const int version = json.get_int("schema_version", -1);
	if (version != kProjectSchemaVersion) {
		char buf[160];
		std::snprintf(buf, sizeof(buf),
		              "This project file uses schema version %d; this editor reads version %d only.",
		              version, kProjectSchemaVersion);
		return fail(error, CoreFinding::ProjectSchemaVersionUnsupported, buf);
	}
	ProjectDocument doc;
	doc.schema_version = version;
	const io::JsonValue *id = json.get("project_id");
	if (!id || !id->is_string() || id->string.empty())
		return fail(error, CoreFinding::ProjectFieldInvalid, "The project file has no project_id.");
	doc.project_id = id->string;
	const io::JsonValue *title = json.get("title");
	if (!title || !title->is_string())
		return fail(error, CoreFinding::ProjectFieldInvalid, "The project file has no title.");
	doc.title = title->string;
	doc.target_game = strutil::to_lower(json.get_string("target_game", kDefaultTargetGame));
	if (gameprofile::gameprofile_by_code(doc.target_game.c_str()) == nullptr)
		return fail(error, CoreFinding::ProjectTargetGameUnknown,
		            "Unknown target game \"" + doc.target_game + "\".");
	if (const io::JsonValue *features = json.get("features")) {
		if (!features->is_object())
			return fail(error, CoreFinding::ProjectFieldInvalid, "\"features\" must be an object.");
		doc.features.menu = features->get_bool("menu", true);
		doc.features.mission = features->get_bool("mission", false);
		doc.features.multiplayer = features->get_bool("multiplayer", false);
	}
	if (const io::JsonValue *exp = json.get("export")) {
		if (!exp->is_object()) return fail(error, CoreFinding::ProjectFieldInvalid, "\"export\" must be an object.");
		doc.export_settings.output = exp->get_string("output", kDefaultExportOutput);
		doc.export_settings.include_runtime = exp->get_bool("include_runtime", false);
	}
	out = std::move(doc);
	return true;
}

bool load_project_document(const std::string &project_file, ProjectDocument &out, Diagnostic &error) {
	std::string text;
	std::string io_error;
	if (!read_file_text(project_file, text, io_error)) {
		std::error_code ec;
		if (!fs::exists(project_file, ec))
			return fail(error, CoreFinding::ProjectFileMissing, "No project file at " + project_file + ".");
		return fail(error, CoreFinding::ProjectFileUnreadable, io_error);
	}
	io::JsonValue json;
	std::string parse_error;
	if (!io::json_parse(text, json, parse_error))
		return fail(error, CoreFinding::ProjectJson, project_file + ": " + parse_error);
	return project_document_from_json(json, out, error);
}

bool save_project_document(const std::string &project_file, const ProjectDocument &doc,
                           Diagnostic &error) {
	std::string io_error;
	if (!write_file_atomic(project_file, io::json_write(project_document_to_json(doc)), io_error))
		return fail(error, CoreFinding::ProjectWrite, io_error);
	return true;
}

std::string make_project_id() {
	std::random_device device;
	std::mt19937_64 rng(static_cast<uint64_t>(device()) << 32 ^ device());
	uint64_t hi = rng();
	uint64_t lo = rng();
	hi = (hi & 0xFFFFFFFFFFFF0FFFull) | 0x0000000000004000ull; // version 4
	lo = (lo & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull; // variant 1
	char buf[40];
	std::snprintf(buf, sizeof(buf), "%08x-%04x-%04x-%04x-%012llx",
	              static_cast<unsigned>(hi >> 32), static_cast<unsigned>((hi >> 16) & 0xFFFF),
	              static_cast<unsigned>(hi & 0xFFFF), static_cast<unsigned>(lo >> 48),
	              static_cast<unsigned long long>(lo & 0xFFFFFFFFFFFFull));
	return buf;
}

bool can_create_project(const std::string &root, const std::string &target_game, Diagnostic &error) {
	const std::string code = strutil::to_lower(target_game);
	if (gameprofile::gameprofile_by_code(code.c_str()) == nullptr)
		return fail(error, CoreFinding::ProjectTargetGameUnknown, "Unknown target game \"" + target_game + "\".");
	const ProjectPaths paths = ProjectPaths::for_root(root);
	std::error_code ec;
	if (fs::exists(paths.project_file, ec))
		return fail(error, CoreFinding::ProjectExists, "There is already a project at " + paths.root + ".");
	return true;
}

bool create_project(const std::string &root, const std::string &title, const std::string &target_game,
                    ProjectDocument &out, Diagnostic &error) {
	if (!can_create_project(root, target_game, error)) return false;
	const std::string code = strutil::to_lower(target_game);
	const ProjectPaths paths = ProjectPaths::for_root(root);
	std::string io_error;
	if (!ensure_directory(paths.root, io_error) || !ensure_project_cache_dir(paths, io_error))
		return fail(error, CoreFinding::ProjectWrite, io_error);
	ProjectDocument doc;
	doc.project_id = make_project_id();
	doc.title = title.empty() ? fs::path(paths.root).filename().string() : title;
	doc.target_game = code;
	if (!save_project_document(paths.project_file, doc, error)) return false;
	out = std::move(doc);
	return true;
}

bool open_project(const std::string &root, ProjectDocument &out, Diagnostic &error) {
	return load_project_document(ProjectPaths::for_root(root).project_file, out, error);
}

} // namespace opennova::editor
