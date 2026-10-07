#include <editor/session/environment_uses.h>

#include <cstdio>
#include <memory>
#include <sstream>

#include <editor/assets/asset_registry.h>
#include <editor/assets/project_asset_source.h>
#include <editor/documents/environment_document.h>
#include <editor/documents/mission_document.h>
#include <editor/graph/asset_graph.h>
#include <editor/session/view/session_view.h>
#include <formats/env/tod_clock.h>
#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>
#include <formats/trn/trn_io.h>
#include <runtime/environment/environment_state.h>
#include <runtime/environment/water_frame.h>

namespace opennova::editor {

namespace {

using io::JsonValue;
using io::json_number;
using io::json_string;

// The open document at `path`, as the base (null when it is not open).
const DocumentBase *open_document(const SessionView &view, const std::string &path) {
	for (const auto &document : view.documents.open)
		if (document && document->path() == path) return document.get();
	return nullptr;
}

// A project file's bytes as the game would read them now: the open document standing in for its file.
bool read_project_file(const SessionView &view, const std::string &path, std::vector<uint8_t> &out) {
	const AssetEntry *entry = view.project.scan ? view.project.scan->at_path(path) : nullptr;
	return entry && view.findings.assets && view.findings.assets->read(entry->logical_name, out);
}

// The mission's header facts: from its open document's record, else from the 616 bytes its file
// starts with (the header the game reads first [orig: Mission_LoadBMSFile, bms.h Header]).
bool mission_header(const SessionView &view, const std::string &path, mission::MissionInfo &out) {
	if (const auto *mission = dynamic_cast<const MissionDocument *>(open_document(view, path))) {
		const MissionRow *row = mission->mission_row();
		if (!row) return false;
		out = mission::mission_info(row->native);
		return true;
	}
	std::vector<uint8_t> bytes;
	if (!read_project_file(view, path, bytes) || bytes.size() < bms::kHeaderSize) return false;
	bms::File file;
	std::string error;
	if (!bms::parse_header_blob(bytes.data(), bms::kHeaderSize, file.header, error)) return false;
	out = mission::mission_info(file);
	return true;
}

// The environment as the game reads it: its open document's record, else its file through the
// engine's reader.
bool environment_config(const SessionView &view, const std::string &path, env::Config &out) {
	if (const auto *environment = dynamic_cast<const EnvironmentDocument *>(open_document(view, path))) {
		if (!environment->config()) return false;
		out = *environment->config();
		return true;
	}
	std::vector<uint8_t> bytes;
	if (!read_project_file(view, path, bytes)) return false;
	const std::string text(bytes.begin(), bytes.end());
	return env::load_mission_env(&text, out);
}

std::string byte_triple(const env::Rgb &rgb) {
	const auto byte = [](float c) { return std::to_string(int(c * 255.0f + 0.5f)); };
	return byte(rgb.r) + "," + byte(rgb.g) + "," + byte(rgb.b);
}

std::string number_words(float value) {
	char text[32];
	std::snprintf(text, sizeof(text), "%g", double(value));
	return text;
}

} // namespace

const char *water_from_token(WaterFrom from) {
	switch (from) {
	case WaterFrom::Mission: return "mission";
	case WaterFrom::Terrain: return "terrain";
	case WaterFrom::Environment: return "environment";
	case WaterFrom::None: break;
	}
	return "none";
}

EnvironmentUses environment_uses(const SessionView &view, const std::string &path) {
	EnvironmentUses uses;
	if (!view.project.open || !view.project.scan) return uses;
	const AssetEntry *entry = view.project.scan->named(path);
	if (!entry) return uses;
	uses.found = true;
	uses.path = entry->relative_path;
	uses.reading = !view.activity.validation.read || view.activity.validation.files_unread;
	const AssetGraph *graph = view.findings.graph.get();
	if (!graph) return uses;
	env::Config environment;
	const bool environment_read = environment_config(view, uses.path, environment);
	for (const GraphEdge *edge : graph->referrers_of_file(uses.path)) {
		if (edge->kind != ReferenceKind::Environment) continue;
		const AssetEntry *source = view.project.scan->at_path(edge->source);
		if (!source || source->kind != AssetKind::Mission) continue;
		EnvironmentMissionUse use;
		use.edge = edge;
		use.mission = edge->source;
		// The terrain the same header names, and the file it finds.
		for (const GraphEdge *reference : graph->references_of(edge->source)) {
			if (reference->kind != ReferenceKind::Terrain) continue;
			use.terrain_edge = reference;
			use.terrain = reference->value;
			std::string file;
			if (graph->resolve(*reference, &file) == ReferenceStatus::Present) use.terrain_file = file;
			break;
		}
		mission::MissionInfo info;
		use.read = mission_header(view, use.mission, info);
		if (use.read) {
			use.title = info.mission_name;
			use.overrides = env::bms_env_overrides_from_header(static_cast<uint32_t>(info.attrib_flags), info.water_override,
			                                                   info.fog_override, info.fog_color, info.water_color,
			                                                   info.water_murk);
			use.start_time = info.start_time;
			use.minutes_per_day = info.minutes_per_day;
			use.tile_set = info.tile_set;
			use.attrib_flags = static_cast<uint32_t>(info.attrib_flags);
			use.water_override = info.water_override;
			use.fog_override = info.fog_override;
			use.water_murk = info.water_murk;
			for (int i = 0; i < 3; ++i) {
				use.fog_color[i] = info.fog_color[i];
				use.water_color[i] = info.water_color[i];
			}
		}
		if (!use.terrain_file.empty()) {
			std::vector<uint8_t> bytes;
			if (read_project_file(view, use.terrain_file, bytes)) {
				std::istringstream input(std::string(bytes.begin(), bytes.end()));
				TrnConfig trn;
				std::string error;
				use.terrain_read = load_trn(input, trn, error);
				if (use.terrain_read) use.terrain_water = trn.water_height;
			}
		}
		// The water plane by the game's ladder: the header's override, then the terrain's water height
		// where it is set, then the environment's (runtime/environment/water_frame.h, env #28) [orig:
		// TimeOfDay_ParseProperty @ 0x57cb4e; Terrain_Init @ 0x60fcb1..0x60fcba; Game_LoadTerrainDuringConnect
		// @ 0x520710].
		env::WaterHeightRungs rungs;
		rungs.has_mission_override = use.overrides.has_water_height;
		rungs.mission_override = use.overrides.has_water_height ? use.overrides.water_height * 0.5f : 0.0f;
		rungs.terrain_height = use.terrain_read ? float(use.terrain_water) * 0.5f : 0.0f;
		rungs.has_loaded_terrain = use.terrain_read;
		env::EnvironmentState state;
		if (environment_read) state.set_config(&environment, true);
		use.water_height = env::resolve_water_height(rungs, environment_read ? &state : nullptr, 0.0f);
		use.water_from = rungs.has_mission_override       ? WaterFrom::Mission
		                 : rungs.terrain_height != 0.0f   ? WaterFrom::Terrain
		                 : environment_read && environment.water_height_set ? WaterFrom::Environment
		                                                                     : WaterFrom::None;
		uses.missions.push_back(std::move(use));
	}
	return uses;
}

std::vector<EnvironmentOverride> environment_overrides(const EnvironmentMissionUse &use) {
	std::vector<EnvironmentOverride> out;
	const env::BmsEnvOverrides &o = use.overrides;
	// mission_table's header keys; the gates are the header's (env.h bms_env_overrides_from_header).
	if (o.has_fog_level) out.push_back({"fog_override", "fog distance " + number_words(o.fog_level) + " m"});
	if (o.has_fog_color) out.push_back({"fog_color", "fog colour " + byte_triple(o.fog_color)});
	if (o.has_water_height)
		out.push_back({"water_override", "water height " + number_words(o.water_height * 0.5f) + " m"});
	if (o.has_water_color) out.push_back({"water_color", "water colour " + byte_triple(o.water_color)});
	if (o.has_water_murk) out.push_back({"murk", "water murk " + number_words(o.water_murk)});
	return out;
}

std::string mission_clock_words(const EnvironmentMissionUse &use) {
	// The header's Q8.8 start hour into the clock [orig: Game_StartMission @ 0x5253ca..0x5253d5], its day
	// length into the advance, none for 0 [orig: Environment_SetTodAdvanceRate @ 0x57d170].
	const int32_t fixed = env::tod_start_fixed24(use.start_time);
	const int hours = fixed >> 24;
	const int minutes = int((int64_t(fixed & 0xFFFFFF) * 60) >> 24);
	char text[96];
	if (use.minutes_per_day == 0)
		std::snprintf(text, sizeof(text), "starts at %02d:%02d, the clock standing", hours, minutes);
	else
		std::snprintf(text, sizeof(text), "starts at %02d:%02d, a day of %d min", hours, minutes,
		              use.minutes_per_day < env::kTodMinMinutesPerDay ? env::kTodMinMinutesPerDay : use.minutes_per_day);
	return text;
}

std::string water_words(const EnvironmentMissionUse &use) {
	switch (use.water_from) {
	case WaterFrom::Mission: return "at " + number_words(use.water_height) + " m, from the mission's header";
	case WaterFrom::Terrain: return "at " + number_words(use.water_height) + " m, from the terrain";
	case WaterFrom::Environment: return "at " + number_words(use.water_height) + " m, from this environment";
	case WaterFrom::None: break;
	}
	return "set by none of the mission's header, its terrain and this environment";
}

io::JsonValue environment_uses_json(const EnvironmentUses &uses) {
	JsonValue out = JsonValue::make_object();
	out.set("path", json_string(uses.path));
	out.set("found", JsonValue::make_bool(uses.found));
	if (uses.reading) out.set("reading", JsonValue::make_bool(true));
	JsonValue missions = JsonValue::make_array();
	for (const EnvironmentMissionUse &use : uses.missions) {
		JsonValue mission = JsonValue::make_object();
		mission.set("mission", json_string(use.mission));
		mission.set("title", json_string(use.title));
		mission.set("read", JsonValue::make_bool(use.read));
		if (use.edge) {
			mission.set("locator", json_string(use.edge->locator));
			mission.set("field", json_string(use.edge->field));
		}
		JsonValue terrain = JsonValue::make_object();
		terrain.set("name", json_string(use.terrain));
		if (!use.terrain_file.empty()) terrain.set("file", json_string(use.terrain_file));
		if (use.terrain_edge) terrain.set("field", json_string(use.terrain_edge->field));
		if (use.terrain_read) terrain.set("water_height", json_number(double(use.terrain_water) * 0.5));
		mission.set("terrain", std::move(terrain));
		JsonValue overrides = JsonValue::make_array();
		for (const EnvironmentOverride &each : environment_overrides(use)) {
			JsonValue row = JsonValue::make_object();
			row.set("field", json_string(each.field));
			row.set("words", json_string(each.words));
			overrides.push(std::move(row));
		}
		mission.set("overrides", std::move(overrides));
		mission.set("start_time", json_number(double(use.start_time)));
		mission.set("minutes_per_day", json_number(double(use.minutes_per_day)));
		mission.set("clock", json_string(mission_clock_words(use)));
		JsonValue water = JsonValue::make_object();
		water.set("from", json_string(water_from_token(use.water_from)));
		water.set("height", json_number(double(use.water_height)));
		water.set("words", json_string(water_words(use)));
		mission.set("water", std::move(water));
		missions.push(std::move(mission));
	}
	out.set("missions", std::move(missions));
	return out;
}

} // namespace opennova::editor
