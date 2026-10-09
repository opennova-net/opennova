#include <editor/session/terrain_uses.h>

#include <algorithm>
#include <memory>

#include <base/io/strutil.h>
#include <editor/assets/asset_registry.h>
#include <editor/assets/project_asset_source.h>
#include <editor/graph/asset_graph.h>
#include <editor/import/sidecar.h>
#include <editor/import/terrain_import.h>
#include <editor/project/project_document.h>
#include <editor/project/project_files.h>
#include <editor/session/environment_uses.h>
#include <editor/session/request_factories.h>
#include <editor/session/session_json.h>
#include <editor/session/view/session_view.h>
#include <formats/env/env.h>
#include <formats/mission/mission.h>
#include <formats/trn/trn_io.h>

namespace opennova::editor {

namespace {

using io::JsonValue;
using io::json_number;
using io::json_string;

std::string folder_of(const std::string &path) {
	const size_t slash = path.find_last_of('/');
	return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

// A project file's bytes as the game would read them now: the open document standing in for its file.
bool read_text(const SessionView &view, const std::string &path, std::string &out) {
	const AssetEntry *entry = path.empty() || !view.project.scan ? nullptr : view.project.scan->at_path(path);
	std::vector<uint8_t> bytes;
	if (!entry || !view.findings.assets || !view.findings.assets->read(entry->logical_name, bytes)) return false;
	out.assign(bytes.begin(), bytes.end());
	return true;
}

std::string stem_of(const std::string &path) {
	std::string name = basename_of(path);
	const size_t dot = name.find_last_of('.');
	if (dot != std::string::npos) name.erase(dot);
	return name;
}

// The import an output comes from: its set read, the images it names, its record's options (S20).
void read_import(const SessionView &view, const AssetEntry &entry, TerrainImport &out) {
	if (entry.imported_from.empty()) return;
	out.imported = true;
	out.source = entry.imported_from;
	out.record = out.source + kImportSidecarSuffix;
	const AssetScan &scan = *view.project.scan;
	for (const AssetEntry &each : scan.entries)
		if (each.imported_from == out.source) out.outputs.push_back(each.relative_path);
	ImportSidecar sidecar;
	Diagnostic read_error;
	const bool record_read = load_import_sidecar(join_path(view.project.root, out.record), sidecar, read_error);
	if (!record_read) out.error = out.record + " does not read" + (read_error.message.empty() ? "." : ": " + read_error.message);
	for (const ImportOptionRow &row : terrain_import_option_rows()) {
		TerrainImportOption option;
		option.key = row.key;
		option.label = row.label;
		option.words = row.words;
		const auto held = sidecar.options.find(row.key);
		option.set = held != sidecar.options.end() && !held->second.empty();
		option.value = option.set ? held->second : row.fallback;
		out.options.push_back(std::move(option));
	}
	// The set's images, each from its folder (the import reads them through its context, terrain_import.h).
	std::vector<uint8_t> bytes;
	std::string why;
	TerrainSet set;
	if (!io::read_file_bytes(join_path(view.project.root, out.source), bytes, why) || !parse_terrain_set(bytes, set, why)) {
		if (out.error.empty()) out.error = out.source + " does not read: " + why;
		return;
	}
	const std::string folder = folder_of(out.source);
	for (const auto &[key, name] : {std::pair<const char *, const std::string *>{"heightmap", &set.heightmap},
	                                {"colormap", &set.colormap},
	                                {"detail", &set.detail},
	                                {"tiles", &set.tiles},
	                                {"surface", &set.surface},
	                                {"foliagemap", &set.foliagemap}}) {
		if (name->empty()) continue;
		TerrainSetImage image;
		image.key = key;
		image.name = *name;
		const std::string relative = folder.empty() ? *name : folder + "/" + *name;
		if (const AssetEntry *file = scan.at_path(relative)) image.file = file->relative_path;
		out.images.push_back(std::move(image));
	}
	out.foliage = set.foliage.size();
}

} // namespace

TerrainUses terrain_uses(const SessionView &view, const std::string &path) {
	TerrainUses uses;
	if (!view.project.open || !view.project.scan) return uses;
	const AssetEntry *entry = view.project.scan->named(path);
	if (!entry) return uses;
	uses.found = true;
	uses.path = entry->relative_path;
	uses.reading = !view.activity.validation.read || view.activity.validation.files_unread;
	read_import(view, *entry, uses.import);
	const AssetGraph *graph = view.findings.graph.get();
	if (!graph) return uses;
	// The texts a mission's terrain load reads after this one, the .env its own (D-TERRAIN-18).
	std::string terrain, overcast;
	const bool terrain_read = read_text(view, uses.path, terrain);
	if (const AssetEntry *file = view.project.scan->find(env::kOvercastFile)) uses.overcast_file = file->relative_path;
	const bool overcast_read = read_text(view, uses.overcast_file, overcast);
	for (const GraphEdge *edge : graph->referrers_of_file(uses.path)) {
		if (edge->kind != ReferenceKind::Terrain) continue;
		const AssetEntry *source = view.project.scan->at_path(edge->source);
		if (!source || source->kind != AssetKind::Mission) continue;
		TerrainMissionUse use;
		use.edge = edge;
		use.mission = edge->source;
		use.name = stem_of(source->logical_name);
		// The environment the same header names, and the file it finds.
		for (const GraphEdge *reference : graph->references_of(edge->source)) {
			if (reference->kind != ReferenceKind::Environment) continue;
			use.environment = reference->value;
			std::string file;
			if (graph->resolve(*reference, &file) == ReferenceStatus::Present) use.environment_file = file;
			break;
		}
		mission::MissionInfo info;
		use.read = read_mission_header(view, use.mission, info);
		if (use.read) {
			use.title = info.mission_name;
			if (use.environment.empty()) use.environment = info.environment;
			use.tile_set = info.tile_set;
			use.attrib_flags = static_cast<uint32_t>(info.attrib_flags);
			use.water_override = info.water_override;
			use.fog_override = info.fog_override;
			use.water_murk = info.water_murk;
			for (int i = 0; i < 3; ++i) {
				use.fog_color[i] = info.fog_color[i];
				use.water_color[i] = info.water_color[i];
			}
			use.start_time = info.start_time;
			use.minutes_per_day = info.minutes_per_day;
		}
		use.tiles = view.project.scan->find(use.name + ".til") != nullptr;
		if (terrain_read) {
			std::string environment;
			TrnLaterTexts later;
			if (overcast_read) later.overcast = &overcast;
			if (read_text(view, use.environment_file, environment)) later.environment = &environment;
			TrnConfig config;
			std::string error;
			std::vector<TrnLaterLine> taken;
			load_mission_trn(terrain, later, config, error, &taken);
			// Each line's place among its file's terrain lines.
			std::vector<int> overcast_lines, environment_lines;
			if (later.overcast) read_trn_key_lines(*later.overcast, &overcast_lines);
			if (later.environment) read_trn_key_lines(*later.environment, &environment_lines);
			for (TrnLaterLine &line : taken) {
				const std::vector<int> &lines =
						line.file == TrnLaterLine::File::Environment ? environment_lines : overcast_lines;
				TerrainMissionUse::Later kept;
				static_cast<TrnLaterLine &>(kept) = std::move(line);
				kept.index = size_t(std::find(lines.begin(), lines.end(), kept.line) - lines.begin());
				use.later.push_back(std::move(kept));
			}
		}
		uses.missions.push_back(std::move(use));
	}
	return uses;
}

io::JsonValue terrain_uses_json(const TerrainUses &uses) {
	JsonValue out = JsonValue::make_object();
	out.set("path", json_string(uses.path));
	out.set("found", JsonValue::make_bool(uses.found));
	if (uses.reading) out.set("reading", JsonValue::make_bool(true));
	if (!uses.overcast_file.empty()) out.set("overcast", json_string(uses.overcast_file));
	JsonValue missions = JsonValue::make_array();
	for (const TerrainMissionUse &use : uses.missions) {
		JsonValue mission = JsonValue::make_object();
		mission.set("mission", json_string(use.mission));
		mission.set("name", json_string(use.name));
		mission.set("title", json_string(use.title));
		mission.set("read", JsonValue::make_bool(use.read));
		if (use.edge) {
			mission.set("locator", json_string(use.edge->locator));
			mission.set("field", json_string(use.edge->field));
		}
		JsonValue environment = JsonValue::make_object();
		environment.set("name", json_string(use.environment));
		if (!use.environment_file.empty()) environment.set("file", json_string(use.environment_file));
		mission.set("environment", std::move(environment));
		mission.set("tile_set", json_string(use.tile_set));
		mission.set("tiles", JsonValue::make_bool(use.tiles));
		mission.set("start_time", json_number(double(use.start_time)));
		mission.set("minutes_per_day", json_number(double(use.minutes_per_day)));
		JsonValue later = JsonValue::make_array();
		for (const TerrainMissionUse::Later &line : use.later) {
			JsonValue row = JsonValue::make_object();
			const bool environment = line.file == TrnLaterLine::File::Environment;
			row.set("file", json_string(environment ? use.environment_file : uses.overcast_file));
			row.set("line", json_number(double(line.line)));
			row.set("index", json_number(double(line.index)));
			row.set("key", json_string(line.key));
			row.set("value", json_string(line.value));
			later.push(std::move(row));
		}
		mission.set("later", std::move(later));
		missions.push(std::move(mission));
	}
	out.set("missions", std::move(missions));
	if (!uses.import.imported) {
		out.set("import", JsonValue::make_null());
		return out;
	}
	const TerrainImport &made = uses.import;
	JsonValue import = JsonValue::make_object();
	import.set("source", json_string(made.source));
	import.set("record", json_string(made.record));
	if (!made.error.empty()) import.set("error", json_string(made.error));
	JsonValue images = JsonValue::make_array();
	for (const TerrainSetImage &image : made.images) {
		JsonValue row = JsonValue::make_object();
		row.set("key", json_string(image.key));
		row.set("name", json_string(image.name));
		if (!image.file.empty()) row.set("file", json_string(image.file));
		images.push(std::move(row));
	}
	import.set("images", std::move(images));
	import.set("foliage", json_number(double(made.foliage)));
	JsonValue options = JsonValue::make_array();
	for (const TerrainImportOption &option : made.options) {
		JsonValue row = JsonValue::make_object();
		row.set("key", json_string(option.key));
		row.set("label", json_string(option.label));
		row.set("value", json_string(option.value));
		row.set("set", JsonValue::make_bool(option.set));
		row.set("words", json_string(option.words));
		options.push(std::move(row));
	}
	import.set("options", std::move(options));
	JsonValue outputs = JsonValue::make_array();
	for (const std::string &output : made.outputs) outputs.push(json_string(output));
	import.set("outputs", std::move(outputs));
	import.set("reimport", editor_request_to_json(request::reimport(made.source, true)));
	out.set("import", std::move(import));
	return out;
}

} // namespace opennova::editor
