#include <editor/project/project_document.h>

#include <cstdio>
#include <filesystem>
#include <system_error>

#include <base/gameprofile/gameprofile.h>
#include <base/io/os_path.h>
#include <base/io/strutil.h>
#include <base/os_random/os_random.h>
#include <editor/model/diagnostic.h>
#include <editor/project/expansion_name.h>
#include <editor/project/project_files.h>

namespace fs = std::filesystem;

namespace opennova::editor {

namespace {

std::string join(const std::string &root, const char *leaf) {
	return join_path(root, leaf);
}

bool fail(Diagnostic &error, CoreFinding code, std::string message) {
	error = make_finding(code, DiagnosticSeverity::Error, std::move(message));
	return false;
}

} // namespace

ProjectPaths ProjectPaths::for_root(const std::string &root) {
	ProjectPaths p;
	p.root = utf8_of(path_of(root));
	p.project_file = join(p.root, kProjectFileName);
	p.cache_dir = join(p.root, kProjectCacheDirName);
	p.local_settings_file = join(p.cache_dir, kLocalSettingsFileName);
	p.imported_dir = join(p.cache_dir, "imported");
	p.import_cache_file = join(p.cache_dir, kImportCacheFileName);
	p.index_dir = join(p.cache_dir, "index");
	p.build_dir = join(p.cache_dir, "build");
	p.build_cache_file = join(p.cache_dir, kBuildCacheFileName);
	p.run_dir = join(p.cache_dir, "run");
	p.staging_dir = join(p.cache_dir, "staging");
	p.install_copy_dir = join(p.cache_dir, "install_copy");
	p.trash_dir = join(p.cache_dir, "trash");
	return p;
}

std::string ProjectPaths::export_dir(const ProjectDocument &doc) const {
	const fs::path output = path_of(doc.export_settings.output);
	if (output.is_absolute()) return io::without_trailing_separator(utf8_of(output.lexically_normal()));
	return io::without_trailing_separator(utf8_of((path_of(root) / output).lexically_normal()));
}

bool ensure_project_cache_dir(const ProjectPaths &paths, std::string &error) {
	if (!io::ensure_directory(paths.cache_dir, error)) return false;
	const std::string gitignore = join(paths.cache_dir, ".gitignore");
	std::error_code ec;
	if (fs::is_regular_file(system_path(gitignore), ec)) return true;
	return io::write_file_atomic(gitignore, std::string("*\n"), error);
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
	// A standalone project has none: the object's absence is what says so.
	if (!doc.expansion.standalone()) {
		io::JsonValue expansion = io::JsonValue::make_object();
		expansion.set("name", io::JsonValue::make_string(doc.expansion.name));
		expansion.set("builds_on", io::JsonValue::make_string(doc.expansion.builds_on));
		// Written only when it names one (T5): a project on the install's base game keeps S16's form.
		if (doc.expansion.on_base_project())
			expansion.set("base_project", io::JsonValue::make_string(doc.expansion.base_project));
		json.set("expansion", std::move(expansion));
	}
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
		std::string message = buf;
		// No reader for an older file (pre-1.0): what changed, so its author can bring it over.
		if (version == 1)
			message += " Schema 2 added the optional \"expansion\" object and changed nothing else: set "
			           "\"schema_version\" to 2 to open it.";
		return fail(error, CoreFinding::ProjectSchemaVersionUnsupported, message);
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
	if (const io::JsonValue *expansion = json.get("expansion")) {
		if (!expansion->is_object())
			return fail(error, CoreFinding::ProjectFieldInvalid, "\"expansion\" must be an object.");
		for (const char *key : { "name", "builds_on", "base_project" }) {
			const io::JsonValue *value = expansion->get(key);
			if (value && !value->is_string())
				return fail(error, CoreFinding::ProjectFieldInvalid,
				            std::string("\"expansion\".\"") + key + "\" must be a string.");
		}
		doc.expansion.name = expansion->get_string("name", "");
		doc.expansion.builds_on = expansion->get_string("builds_on", "");
		doc.expansion.base_project = expansion->get_string("base_project", "");
		// An object naming nothing is no expansion's: the object's absence says standalone.
		if (doc.expansion.standalone() && doc.expansion.builds_on.empty() && !doc.expansion.on_base_project())
			return fail(error, CoreFinding::ProjectFieldInvalid,
			            "\"expansion\" names no expansion: give it a \"name\", or leave the object out for a "
			            "standalone project.");
		if (!check_project_expansion(doc.target_game, doc.expansion, error)) return false;
	}
	out = std::move(doc);
	return true;
}

bool load_project_document(const std::string &project_file, ProjectDocument &out, Diagnostic &error) {
	std::string text;
	std::string io_error;
	if (!io::read_file_text(project_file, text, io_error)) {
		std::error_code ec;
		if (!fs::exists(system_path(project_file), ec))
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
	if (!io::write_file_atomic(project_file, io::json_write(project_document_to_json(doc)), io_error))
		return fail(error, CoreFinding::ProjectWrite, io_error);
	return true;
}

bool can_create_project(const std::string &root, const std::string &target_game, Diagnostic &error,
                        const ProjectExpansion &expansion) {
	const std::string code = strutil::to_lower(target_game);
	if (gameprofile::gameprofile_by_code(code.c_str()) == nullptr)
		return fail(error, CoreFinding::ProjectTargetGameUnknown, "Unknown target game \"" + target_game + "\".");
	if (!check_project_expansion(code, expansion, error)) return false;
	const ProjectPaths paths = ProjectPaths::for_root(root);
	std::error_code ec;
	if (fs::exists(system_path(paths.project_file), ec)) // a project past MAX_PATH too
		return fail(error, CoreFinding::ProjectExists, "There is already a project at " + paths.root + ".");
	return true;
}

bool create_project(const std::string &root, const std::string &title, const std::string &target_game,
                    ProjectDocument &out, Diagnostic &error, const ProjectExpansion &expansion) {
	if (!can_create_project(root, target_game, error, expansion)) return false;
	const std::string code = strutil::to_lower(target_game);
	const ProjectPaths paths = ProjectPaths::for_root(root);
	std::string io_error;
	if (!io::ensure_directory(paths.root, io_error) || !ensure_project_cache_dir(paths, io_error))
		return fail(error, CoreFinding::ProjectWrite, io_error);
	ProjectDocument doc;
	doc.project_id = opennova::make_uuid_v4(); // a fresh random UUID from the OS CSPRNG, its version 4 text form
	doc.title = title.empty() ? utf8_of(path_of(paths.root).filename()) : title;
	doc.target_game = code;
	doc.expansion = expansion;
	if (!save_project_document(paths.project_file, doc, error)) return false;
	out = std::move(doc);
	return true;
}

bool open_project(const std::string &root, ProjectDocument &out, Diagnostic &error) {
	return load_project_document(ProjectPaths::for_root(root).project_file, out, error);
}

} // namespace opennova::editor
