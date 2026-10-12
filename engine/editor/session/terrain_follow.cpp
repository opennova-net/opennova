#include <editor/session/terrain_follow.h>

#include <algorithm>

#include <base/io/strutil.h>
#include <editor/assets/asset_registry.h>
#include <editor/assets/project_asset_source.h>
#include <editor/documents/mission_document.h>
#include <editor/documents/mission_reads.h>
#include <editor/graph/asset_graph.h>
#include <editor/project/project_files.h>
#include <editor/session/document_set.h>
#include <editor/session/mission_base_terrain.h>
#include <editor/session/session_core.h>
#include <editor/session/view/session_view.h>
#include <formats/mission/bms.h>

namespace opennova::editor {

namespace {

constexpr size_t kAtlasWords = size_t(1024) * 1024;

// The ground's change under a point (mission 16.16), new less old, as the original editor samples each.
int32_t ground_change(const mission::MisTerrain &before, const mission::MisTerrain &after, int32_t x, int32_t y) {
	return static_cast<int32_t>(static_cast<uint32_t>(mission::mis_terrain_height(after, x, y)) -
	                            static_cast<uint32_t>(mission::mis_terrain_height(before, x, y)));
}

// The z edits that move a mission's unlocked entities with the ground: each z plus the change under it, in the
// 16.16 word the file holds (the original editor's arithmetic: its relative z over the new ground).
std::vector<Edit> follow_edits(const MissionDocument &document, const mission::MisTerrain &before,
		const mission::MisTerrain &after, uint64_t link) {
	std::vector<Edit> out;
	for (const MissionEntityRead &read : mission_entities(document)) {
		if (height_locked(document, read.address)) continue;
		const int32_t change = ground_change(before, after, bms::to_fixed_16_16(read.x), bms::to_fixed_16_16(read.y));
		if (change == 0) continue;
		const int32_t z = static_cast<int32_t>(static_cast<uint32_t>(bms::to_fixed_16_16(read.z)) +
		                                       static_cast<uint32_t>(change));
		Edit edit;
		edit.address = read.address;
		edit.field = "z";
		edit.value = static_cast<double>(z) / 65536.0;
		edit.gesture = link;
		out.push_back(std::move(edit));
	}
	return out;
}

// Whether a closed mission's file has an entity the change moves (its .bms read alone: its .mis's locks are read
// when it opens).
bool file_moves(const FileSource &files, const std::string &name, const mission::MisTerrain &before,
		const mission::MisTerrain &after) {
	std::vector<uint8_t> bytes;
	bms::File file;
	std::string error;
	if (!files.read(name, bytes) || !bms::parse(bytes.data(), bytes.size(), file, error)) return false;
	for (const std::vector<bms::Entity> *pool : {&file.items, &file.buildings, &file.markers, &file.organics})
		for (const bms::Entity &entity : *pool)
			if (ground_change(before, after, entity.x, entity.y) != 0) return true;
	return false;
}

} // namespace

bool height_locked(const MissionDocument &mission, const NodeAddress &address) {
	Value lock;
	if (!mission.get(address, "height_lock", lock)) return false;
	if (const auto *number = std::get_if<int64_t>(&lock)) return *number != 0;
	if (const auto *real = std::get_if<double>(&lock)) return *real != 0.0;
	return false;
}

GroundFollowers ground_followers(const MissionDocument &mission, double x, double y, double radius) {
	GroundFollowers out;
	for (const MissionEntityRead &read : mission_entities(mission)) {
		const double dx = read.x - x, dy = read.y - y;
		if (dx * dx + dy * dy > radius * radius) continue;
		++(height_locked(mission, read.address) ? out.locked : out.follow);
	}
	return out;
}

size_t follow_the_ground(SessionCore &core, const std::string &cpt, const std::vector<uint16_t> &before,
		const std::vector<uint16_t> &after, uint64_t link) {
	const SessionView &view = core.view();
	if (!view.project.open || !view.project.scan || !view.findings.assets || !view.findings.graph ||
	    before.size() != kAtlasWords || after.size() != kAtlasWords || before == after)
		return 0;
	const std::shared_ptr<const ProjectAssetSource> files = view.findings.assets;
	const std::shared_ptr<const AssetGraph> graph = view.findings.graph;
	const std::string cpt_name = basename_of(cpt);
	const std::string wanted = strutil::to_lower(cpt_name);
	DocumentSet &documents = core.documents();
	// Every .trn of the project naming the .cpt, in path order, and the missions on each.
	std::vector<std::string> terrains;
	for (const AssetEntry &entry : view.project.scan->entries)
		if (entry.kind == AssetKind::Terrain) terrains.push_back(entry.relative_path);
	std::sort(terrains.begin(), terrains.end());
	size_t moved = 0;
	std::vector<std::string> done;
	for (const std::string &terrain : terrains) {
		const AssetEntry *entry = view.project.scan->at_path(terrain);
		TrnConfig trn;
		int32_t scale = 128;
		if (!entry || !read_base_trn(*files, entry->logical_name, trn, scale) ||
		    strutil::to_lower(basename_of(trn.polydata)) != wanted)
			continue;
		const mission::MisTerrain ground_before = base_terrain_sampler(trn, scale, before.data());
		const mission::MisTerrain ground_after = base_terrain_sampler(trn, scale, after.data());
		std::vector<std::string> missions;
		for (const GraphEdge *edge : graph->referrers_of_file(terrain)) {
			if (edge->kind != ReferenceKind::Terrain) continue;
			const AssetEntry *source = view.project.scan->at_path(edge->source);
			if (source && source->kind == AssetKind::Mission &&
			    std::find(done.begin(), done.end(), edge->source) == done.end() &&
			    std::find(missions.begin(), missions.end(), edge->source) == missions.end())
				missions.push_back(edge->source);
		}
		std::sort(missions.begin(), missions.end());
		for (const std::string &path : missions) {
			done.push_back(path);
			DocumentBase *open = documents.document_for(path);
			if (!open) {
				const AssetEntry *source = view.project.scan->at_path(path);
				if (!source || !file_moves(*files, source->logical_name, ground_before, ground_after)) continue;
				open = documents.open_quietly(path, "its entities follow the ground of " + cpt_name);
				if (!open) continue;
			}
			auto *mission = dynamic_cast<MissionDocument *>(open);
			if (!mission) continue;
			if (mission->blocked() || mission->is_snapshot()) {
				core.note("The entities of " + path + " did not follow the ground of " + cpt_name +
				          ": it cannot be edited.");
				continue;
			}
			if (mission->unsaved() && !mission->matches_file()) {
				core.note("The entities of " + path + " did not follow the ground of " + cpt_name +
				          ": its file changed outside the editor under unsaved edits.");
				continue;
			}
			const std::vector<Edit> edits = follow_edits(*mission, ground_before, ground_after, link);
			if (edits.empty()) continue;
			const std::string words = "Followed the ground of " + cpt_name + " (" + std::to_string(edits.size()) +
			                          (edits.size() == 1 ? " entity)" : " entities)");
			if (!documents.apply_terrain_edits(*mission, edits, cpt, words)) continue;
			// The batch's gesture ends at once: the stroke it follows ended before its bake landed.
			if (link) documents.end_edit(path, link);
			core.note(words + " in " + path);
			moved += edits.size();
		}
	}
	return moved;
}

} // namespace opennova::editor
