#include "environment_inspector.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include <imgui.h>

#include <editor/assets/asset_registry.h>
#include <editor/documents/environment_document.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/reference_queries.h>
#include <editor/session/environment_uses.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/ui_kit.h>

namespace opennova::editor {

namespace {

// A sentence of the game's words, muted and wrapped.
void note(const std::string &text) {
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
	ImGui::TextWrapped("%s", text.c_str());
	ImGui::PopStyleColor();
}

// A line that goes to `target` (a Selectable cut to the room, whole in its tooltip).
void jump_line(Workspace &workspace, const ReferenceTarget &target, const std::string &line, const std::string &id) {
	ImGui::PushID(id.c_str());
	const std::string shown = ui_kit::fit(line, ImGui::GetContentRegionAvail().x);
	if (ImGui::Selectable((shown + "###jump").c_str()) && !target.file.empty()) window_requests::go_to(workspace, target);
	ui_kit::tooltip(line + "\nClick to go there.");
	ImGui::PopID();
}

std::string base_name(const std::string &path) { return path.substr(path.find_last_of('/') + 1); }

std::string clock(int hhmm) {
	char text[8];
	std::snprintf(text, sizeof(text), "%02d:%02d", hhmm / 100, hhmm % 100);
	return text;
}

// The uses, made again only when what they read moves (the graph, the files, the open documents).
const EnvironmentUses &uses_of(const SessionView &view, const std::string &path) {
	static std::string kept_path;
	static RevisionKey kept_key;
	static EnvironmentUses kept;
	const RevisionKey key =
			revision_key(view.revisions, {ViewConcern::Project, ViewConcern::Files, ViewConcern::Graph, ViewConcern::Documents});
	if (path != kept_path || key != kept_key || kept.path.empty()) {
		kept = environment_uses(view, path);
		kept_path = path;
		kept_key = key;
	}
	return kept;
}

// A keyframe's place in the day: the time the clock reaches it and the keyframe it blends toward
// after it (the next in time, past the last the first) [orig: Environment_FindKeyframeSegment
// @ 0x57dd80, its 24 h wrap].
void keyframe_words(const EnvironmentDocument &document, const NodeAddress &record) {
	const env::Config *config = document.config();
	Value time;
	if (!config || !document.get(record, "time", time) || !std::holds_alternative<int64_t>(time)) return;
	const int at = int(std::get<int64_t>(time));
	std::vector<int> times;
	for (const env::Keyframe &keyframe : config->keyframes) times.push_back(keyframe.time);
	std::stable_sort(times.begin(), times.end());
	// The game's search (formats/env find_keyframe_segment) in 16.16 hours.
	std::vector<int> hours;
	for (const int time : times) hours.push_back(env::hhmm_to_hours_fp(float(time)));
	const env::KeyframeSegment segment = env::find_keyframe_segment(hours, env::hhmm_to_hours_fp(float(at)));
	if (segment.hi < 0) return;
	const int next = times[size_t(segment.hi)];
	if (times.size() < 2)
		note("The only keyframe: its colours hold all day.");
	else
		note("The clock reaches these colours at " + clock(at) + "; after it the game blends toward the keyframe at " +
		     clock(next) + ".");
}

// A terrain key's line of a mission: its terrain takes it after its .trn and overcast.def, over what the keyword held
// before it (the file and value that set it, else the load's default).
std::string terrain_key_words(const EnvironmentMissionUse &use, const EnvironmentTerrainKey &key, const std::string &self) {
	const std::string terrain = use.terrain_file.empty() ? use.terrain : base_name(use.terrain_file);
	std::string words = "Its terrain's " + key.key + (key.value.empty() ? "" : " " + key.value) + " from this file";
	if (key.over.empty()) return words + ", read after " + terrain + "'s lines";
	if (key.over_file.empty()) return words + ", over the default " + key.over;
	if (key.over_file == self) return words + ", over its own " + key.over + " above";
	return words + ", over " + base_name(key.over_file) + "'s " + key.over;
}

} // namespace

bool draw_environment_inspector(Workspace &workspace, const Document &document, const NodeAddress &record,
                                InspectorTaken &) {
	const auto *environment = dynamic_cast<const EnvironmentDocument *>(&document);
	if (!environment) return false;
	const SessionView &view = workspace.view();
	if (record.child && record.kind == node_kind(EnvironmentKind::Keyframe)) keyframe_words(*environment, record);
	// A terrain key: what the terrain's reader does with this file's lines [orig: Terrain_LoadEnvironmentConfig
	// @ 0x6109AD; Environment_LoadTimeOfDayConfig @ 0x57DCBF] (D-TERRAIN-18).
	const bool on_key = record.child && record.kind == node_kind(EnvironmentKind::TerrainKey);
	if (on_key)
		note("A terrain key: the terrain's reader reads this file's lines after each mission's .trn and overcast.def, "
		     "so the terrain of a mission that runs on this environment takes it over theirs.");
	if (!view.project.scan) return true;
	const AssetScan &scan = *view.project.scan;
	const EnvironmentUses &uses = uses_of(view, environment->path());
	if (uses.missions.empty()) {
		note(uses.reading ? "Reading the project's references..."
		                  : "No mission runs on it: a mission's header names its environment (Environment).");
		ImGui::Separator();
		return true;
	}
	ImGui::Text("Missions that run on it (%zu):", uses.missions.size());
	int id = 0;
	for (const EnvironmentMissionUse &use : uses.missions) {
		const std::string tag = std::to_string(id++);
		const std::string title = use.title.empty() ? base_name(use.mission) : base_name(use.mission) + " (" + use.title + ")";
		jump_line(workspace, use.edge ? usage_target(scan, *use.edge) : file_target(scan, use.mission), title, "m" + tag);
		ImGui::Indent();
		// The terrain it pairs with.
		if (!use.terrain_file.empty())
			jump_line(workspace, file_target(scan, use.terrain_file), "On the terrain " + base_name(use.terrain_file), "t" + tag);
		else if (!use.terrain.empty())
			note("On the terrain " + use.terrain + ", which the project does not have.");
		// What its header sets over this environment, each on the header's field.
		const std::string locator = use.edge ? use.edge->locator : std::string();
		const auto header = [&](const char *field) {
			ReferenceTarget target;
			target.file = use.mission;
			target.locator = locator;
			target.field = field;
			target.editable = true;
			return target;
		};
		for (const EnvironmentOverride &each : environment_overrides(use))
			jump_line(workspace, header(each.field), "Its header sets " + each.words + " over this", "o" + tag + each.field);
		jump_line(workspace, header("start_time"), "It " + mission_clock_words(use) + " (curtime and tod_rate here are not read)",
		          "c" + tag);
		// Where its water plane comes from: the header's, the terrain's or this environment's.
		ReferenceTarget water;
		if (use.water_from == env::WaterRung::Mission) water = header("water_override");
		else if (use.water_from == env::WaterRung::Terrain) water = file_target(scan, use.terrain_file);
		else if (const EnvironmentRow *row = environment->environment_row()) {
			water.file = environment->path();
			water.locator = environment->locator({row->id, row->kind, 0});
			water.field = "water_height";
			water.editable = true;
		}
		jump_line(workspace, water, "Its water plane: " + water_words(use), "w" + tag);
		// The terrain keys of this file its terrain takes (the game's terrain reader reads its lines too), each a Go to
		// on its record; on a terrain key, its own line alone, or that its terrain does not take it.
		bool taken = false;
		for (const EnvironmentTerrainKey &key : use.terrain_keys) {
			const NodeAddress at = environment->terrain_key_address(key.index);
			if (on_key && at != record) continue;
			taken = true;
			ReferenceTarget target;
			target.file = environment->path();
			target.locator = at.child ? environment->locator(at) : std::string();
			target.field = "value";
			target.editable = true;
			jump_line(workspace, target, terrain_key_words(use, key, environment->path()),
			          "k" + tag + "." + std::to_string(key.index));
		}
		if (on_key && !taken)
			note(use.terrain_file.empty() ? "Its terrain is not in the project, so the line is read over nothing here."
			                              : "Its terrain does not take this line (Problems says why where the file alone "
			                                "shows it).");
		ImGui::Unindent();
	}
	ImGui::Separator();
	return true;
}

} // namespace opennova::editor
