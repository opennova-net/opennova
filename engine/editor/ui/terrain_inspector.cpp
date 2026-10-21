#include "terrain_inspector.h"

#include <cstdio>
#include <string>

#include <imgui.h>

#include <base/io/os_path.h>
#include <editor/assets/asset_registry.h>
#include <editor/documents/environment_document.h>
#include <editor/documents/terrain_document.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/reference_queries.h>
#include <editor/model/text_document.h>
#include <editor/session/request_factories.h>
#include <editor/session/terrain_uses.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/ui_kit.h>

namespace opennova::editor {

namespace {

constexpr NodeKind kSectorRow = node_kind(TerrainKind::SectorRow);

// The uses, made again only when what they read moves (the graph, the files, the open documents).
const TerrainUses &uses_of(const SessionView &view, const std::string &path) {
	static std::string kept_path;
	static RevisionKey kept_key;
	static TerrainUses kept;
	const RevisionKey key =
			revision_key(view.revisions, {ViewConcern::Project, ViewConcern::Files, ViewConcern::Graph, ViewConcern::Documents});
	if (path != kept_path || key != kept_key || kept.path.empty()) {
		kept = terrain_uses(view, path);
		kept_path = path;
		kept_key = key;
	}
	return kept;
}

// A grid row's place in the world: the mission's y it spans (north up, the grid's rows running south), and
// where its first cell lies east; each cell 512 units [orig: Terrain_GetHeightAtPosition @ 0x606720, the cell
// a world position routes through, less the origin; runtime/terrain_query/coords.h].
void row_words(const TerrainDocument &document, const NodeAddress &record) {
	const TrnConfig *config = document.config();
	Document::Placement at;
	if (!config || !document.placement(record, at)) return;
	const int row = int(at.index);
	const int north = -(row + config->origin_y) * 512;
	const int west = config->origin_x * 512;
	char line[96];
	std::snprintf(line, sizeof(line), "y %d to %d, cell 1 at x %d, %d cells read", north, north - 512, west,
	              config->sector_count);
	char text[256];
	std::snprintf(text, sizeof(text),
	              "This row spans the mission's y from %d down to %d; its cell 1 spans x from %d to %d, each next cell the "
	              "512 east of it. The game reads its first %d cells (the grid's width).",
	              north, north - 512, west, west + 512, config->sector_count);
	ui_kit::quiet_text(line, text);
}

void draw_import(Workspace &workspace, const AssetScan &scan, const TerrainImport &made) {
	ui_kit::clipped_text("Made by the import of " + io::utf8_file_name(made.source) + ".",
	                     "The terrain set's images and the import's options make this terrain: the editor takes no edit of "
	                     "it here. Change an image or an option, then Reimport.");
	if (!made.error.empty()) ui_kit::refusal_text(made.error);
	inspector_jump_line(workspace, file_target(scan, made.source), "The terrain set: " + made.source, "set");
	ImGui::Indent();
	for (const TerrainSetImage &image : made.images) {
		const std::string line = image.key + ": " + image.name + (image.file.empty() ? " (not in the project)" : "");
		inspector_jump_line(workspace, image.file.empty() ? ReferenceTarget() : file_target(scan, image.file), line, "i" + image.key);
	}
	if (made.foliage)
		ui_kit::quiet_text(std::to_string(made.foliage) + (made.foliage == 1 ? " foliage definition" : " foliage definitions"),
		                   "Written into the terrain as its foliage blocks.");
	for (const TerrainImportOption &option : made.options)
		ui_kit::quiet_text(option.label + ": " + option.value, option.set ? std::string() : "The importer's own.");
	ImGui::Unindent();
	const bool can = workspace.view().allows(EditorRequestKind::Reimport);
	ImGui::BeginDisabled(!can);
	if (ImGui::Button("Reimport") && can) workspace.request(request::reimport(made.source, true));
	ImGui::EndDisabled();
	ui_kit::tooltip("Run the terrain importer again over the set's images and options: every file it makes, this one "
	                "among them, is made again.");
}

} // namespace

bool draw_terrain_inspector(Workspace &workspace, const Document &document, const NodeAddress &record, InspectorTaken &) {
	const auto *terrain = dynamic_cast<const TerrainDocument *>(&document);
	if (!terrain) return false;
	const SessionView &view = workspace.view();
	if (record.child && record.kind == kSectorRow) row_words(*terrain, record);
	if (!view.project.scan) return true;
	const AssetScan &scan = *view.project.scan;
	const TerrainUses &uses = uses_of(view, terrain->path());
	if (uses.import.imported) {
		draw_import(workspace, scan, uses.import);
		ImGui::Separator();
	}
	if (!ui_kit::uses_heading("Missions on it", uses.missions.size(), uses.reading,
	                          "The missions whose header names this terrain, with what each lays over it.",
	                          "No mission runs on it.", "A mission's header names its terrain (Terrain).")) {
		ImGui::Separator();
		return true;
	}
	int id = 0;
	for (const TerrainMissionUse &use : uses.missions) {
		const std::string tag = std::to_string(id++);
		const std::string title = use.title.empty() ? io::utf8_file_name(use.mission) : io::utf8_file_name(use.mission) + " (" + use.title + ")";
		inspector_jump_line(workspace, use.edge ? usage_target(scan, *use.edge) : file_target(scan, use.mission), title, "m" + tag);
		ImGui::Indent();
		if (!use.environment_file.empty())
			inspector_jump_line(workspace, file_target(scan, use.environment_file),
			                    "Under the environment " + io::utf8_file_name(use.environment_file), "e" + tag);
		else if (!use.environment.empty())
			ui_kit::quiet_text("Under the environment " + use.environment + " (missing)",
			                   "The project has no " + use.environment + ".");
		if (!use.tile_set.empty()) ui_kit::quiet_text("Its tiles from the tile set " + use.tile_set, "Not the terrain's own.");
		if (use.tiles) ui_kit::quiet_text("It places its own tiles (" + use.name + ".til)");
		// The terrain keys overcast.def and its environment set after this file's (the game's terrain reader reads
		// their lines too): the mission's terrain has each from there. A Go to on the line: the environment's terrain
		// key, overcast.def's line (a text).
		for (const TerrainMissionUse::Later &line : use.later) {
			const bool environment = line.file == TrnLaterLine::File::Environment;
			const std::string &file = environment ? use.environment_file : uses.overcast_file;
			ReferenceTarget target = file_target(scan, file);
			target.locator = environment ? terrain_key_locator(line.index) : TextDocument::locator(size_t(line.line), 1);
			if (environment) target.field = "value";
			inspector_jump_line(workspace, target, "Its " + line.key + " " + line.value + " from " + io::utf8_file_name(file),
			                    "k" + tag + "." + std::to_string(int(line.file)) + "." + std::to_string(line.line),
			                    "Line " + std::to_string(line.line) + ", read after this file: the mission's terrain has it from there.");
		}
		ImGui::Unindent();
	}
	ImGui::Separator();
	return true;
}

} // namespace opennova::editor
