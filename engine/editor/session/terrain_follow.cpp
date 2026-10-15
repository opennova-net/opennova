#include <editor/session/terrain_follow.h>

#include <algorithm>
#include <iterator>
#include <variant>

#include <base/io/strutil.h>
#include <base/io/file_io.h>
#include <editor/assets/asset_registry.h>
#include <editor/assets/project_asset_source.h>
#include <editor/documents/mission_document.h>
#include <editor/documents/mission_file_set.h>
#include <editor/documents/mission_names_file.h>
#include <editor/documents/mission_reads.h>
#include <editor/graph/rename_transaction.h>
#include <editor/model/change_set.h>
#include <editor/project/project_files.h>
#include <editor/session/document_set.h>
#include <editor/session/mission_base_terrain.h>
#include <editor/session/session_core.h>
#include <editor/session/terrain_family.h>
#include <editor/session/view/session_view.h>
#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>
#include <formats/mission/mission_chains.h>
#include <formats/trn/trn_io.h>
#include <runtime/mission/mission_sidecars.h>

namespace opennova::editor {

static bool height_locked(const MissionDocument &mission, const NodeAddress &address);

namespace {

constexpr size_t kAtlasWords = size_t(1024) * 1024;

// The ground's change under a point (mission 16.16), new less old, as the original editor samples each.
int32_t ground_change(const mission::MisTerrain &before, const mission::MisTerrain &after, int32_t x, int32_t y) {
	return static_cast<int32_t>(static_cast<uint32_t>(mission::mis_terrain_height(after, x, y)) -
	                            static_cast<uint32_t>(mission::mis_terrain_height(before, x, y)));
}

// The rows edited since `mark` (or since its load, where the mission was opened or read again since): their entities
// were put against the ground the views showed, so they stay. Null where the history cannot say (every entity
// follows).
bool edited_since(const MissionDocument &document, const FollowMark *mark, std::vector<NodeId> &rows) {
	rows.clear();
	const bool same = mark && mark->identity == document.identity() && mark->load == document.load_generation();
	ChangeSet changes;
	if (!document.changes_since(document.load_generation(), same ? mark->revision : 0, changes)) return false;
	const RowChanges *changed = std::get_if<RowChanges>(&changes);
	if (!changed) return false;
	rows = changed->changed;
	rows.insert(rows.end(), changed->added.begin(), changed->added.end());
	std::sort(rows.begin(), rows.end());
	// The window's own earlier follows are no edit of the user's.
	if (same && !mark->followed.empty()) {
		std::vector<NodeId> theirs;
		std::set_difference(rows.begin(), rows.end(), mark->followed.begin(), mark->followed.end(), std::back_inserter(theirs));
		rows = std::move(theirs);
	}
	return true;
}

// Each unlocked entity's change of z with the ground under it (16.16), but (`marked`) those edited since the window
// opened (`mark`: the mission's state then; null, opened since).
std::vector<std::pair<NodeAddress, int32_t>> follow_changes(const MissionDocument &document,
		const mission::MisTerrain &before, const mission::MisTerrain &after, bool marked, const FollowMark *mark) {
	std::vector<NodeId> edited;
	const bool known = marked && edited_since(document, mark, edited);
	std::vector<std::pair<NodeAddress, int32_t>> out;
	for (const MissionEntityRead &read : mission_entities(document)) {
		if (height_locked(document, read.address)) continue;
		if (known && std::binary_search(edited.begin(), edited.end(), read.address.row)) continue;
		const int32_t change = ground_change(before, after, bms::to_fixed_16_16(read.x), bms::to_fixed_16_16(read.y));
		if (change != 0) out.emplace_back(read.address, change);
	}
	return out;
}

// The z edits a mission's changes make, each z as it stands plus its change, in the 16.16 word the file holds (the
// original editor's arithmetic: its relative z over the new ground).
std::vector<Edit> follow_edits(const MissionDocument &document,
		const std::vector<std::pair<NodeAddress, int32_t>> &changes, uint64_t link) {
	std::vector<Edit> out;
	for (const auto &[address, change] : changes) {
		MissionEntityRead read;
		if (!mission_entity(document, address, read)) continue;
		const int32_t z = static_cast<int32_t>(static_cast<uint32_t>(bms::to_fixed_16_16(read.z)) +
		                                       static_cast<uint32_t>(change));
		Edit edit;
		edit.address = address;
		edit.field = "z";
		edit.value = static_cast<double>(z) / 65536.0;
		edit.gesture = link;
		out.push_back(std::move(edit));
	}
	return out;
}

std::string followed_words(const std::string &cpt_name, size_t count) {
	return "Followed the ground of " + cpt_name + " (" + std::to_string(count) + (count == 1 ? " entity)" : " entities)");
}

// The edits applied to the mission as one step carrying the link, its gesture ended at once (the stroke it follows
// ended before its bake landed).
bool apply_follow(SessionCore &core, MissionDocument &mission, const std::vector<Edit> &edits, const std::string &view,
		const std::string &cpt_name, uint64_t link) {
	DocumentSet &documents = core.documents();
	const std::string words = followed_words(cpt_name, edits.size());
	const std::string path = mission.path();
	if (!documents.apply_terrain_edits(mission, edits, view, words)) return false;
	if (link) documents.end_edit(path, link);
	core.note(words + " in " + path);
	return true;
}

// The ground a mission reads, by its header's terrain and environment: the terrain's configuration as the game
// resolves it for the mission (the .trn, overcast.def and the mission's .env through one parser, D-TERRAIN-18: the
// routing a stroke in its view takes, session/terrain_family) and the original editor's scale of its .trn (its atol
// read, mission_base_terrain.h); `follows` where its polytrn_polydata is the .cpt landed on.
struct MissionGround {
	bool follows = false;
	TrnConfig trn;
	int32_t scale = 128;
};

const MissionGround &mission_ground(std::map<std::string, MissionGround> &grounds, const ProjectAssetSource &files,
		const std::string &terrain, const std::string &environment, const std::string &wanted) {
	const std::string key = strutil::to_lower(terrain) + "|" + strutil::to_lower(environment);
	if (const auto found = grounds.find(key); found != grounds.end()) return found->second;
	MissionGround &ground = grounds[key];
	if (terrain.empty()) return ground;
	const TrnTextReader read_text = [&files](const std::string &name, std::string &text) {
		std::vector<uint8_t> bytes;
		if (!files.read(name, bytes)) return false;
		text.assign(bytes.begin(), bytes.end());
		return true;
	};
	std::string error;
	TrnConfig base;
	// A terrain the game refuses samples no height (read_base_trn's rule).
	ground.follows = read_mission_trn(read_text, terrain + ".trn", environment.empty() ? std::string() : environment + ".env",
	                                  ground.trn, error) &&
	                 read_base_trn(files, terrain + ".trn", base, ground.scale) &&
	                 strutil::to_lower(basename_of(ground.trn.polydata)) == wanted;
	return ground;
}

// A closed mission's .bms and the height locks its .mis beside it gives its entities (DocumentSet's companion: the
// file of the mission's base name in its folder, read and matched as the mission's load reads it,
// mission_names_file.h), flat over its pools; none where it has no .mis that matches.
bool read_closed_mission(const SessionView &view, const ProjectAssetSource &files, const AssetEntry &entry,
		bms::File &file, std::vector<int32_t> &locks) {
	std::vector<uint8_t> bytes;
	std::string error;
	if (!files.read(entry.logical_name, bytes) || !bms::parse(bytes.data(), bytes.size(), file, error)) return false;
	locks.clear();
	const std::string name = mission::mission_base_name(basename_of(entry.relative_path)) + ".mis";
	const AssetEntry *mis = file_in_folder(*view.project.scan, folder_of_path(entry.relative_path), name);
	std::vector<uint8_t> text;
	if (!mis || !io::read_file_bytes(join_path(view.project.root, mis->relative_path), text, error)) return true;
	std::vector<mission::EventChain> events;
	mission::RunReport report;
	if (!mission::split_event_chains(file, events, report)) return true;
	MissionNamesFile names = read_names_file(file, events, text);
	if (names.found == MissionNamesFile::Found::Matched) locks = std::move(names.kept_locks);
	return true;
}

// Whether a closed mission has an entity the change moves, one its .mis does not height lock.
bool file_moves(const bms::File &file, const std::vector<int32_t> &locks, const mission::MisTerrain &before,
		const mission::MisTerrain &after) {
	size_t index = 0;
	for (const std::vector<bms::Entity> *pool : {&file.items, &file.buildings, &file.markers, &file.organics})
		for (const bms::Entity &entity : *pool) {
			const bool locked = index < locks.size() && locks[index] != 0;
			++index;
			if (!locked && ground_change(before, after, entity.x, entity.y) != 0) return true;
		}
	return false;
}

} // namespace

// Whether the entity at `address` of `mission` is height-locked (its height_lock field set; a mission whose entities
// carry none: each follows the ground).
static bool height_locked(const MissionDocument &mission, const NodeAddress &address) {
	Value lock;
	if (!mission.get(address, "height_lock", lock)) return false;
	if (const auto *number = std::get_if<int64_t>(&lock)) return *number != 0;
	if (const auto *real = std::get_if<double>(&lock)) return *real != 0.0;
	return false;
}

std::vector<const MissionDocument *> missions_under_view(const SessionView &view, const DocumentBase &document,
		const TerrainFamily &family) {
	std::vector<const MissionDocument *> out;
	if (const auto *mission = dynamic_cast<const MissionDocument *>(&document)) {
		out.push_back(mission);
		return out;
	}
	// The open missions whose heights a stroke in their own view would edit (the same routing: their .env's).
	const std::string &heights = family.plane(TerrainPlane::Heights).path;
	if (heights.empty()) return out;
	for (const auto &open : view.documents.open)
		if (const auto *mission = dynamic_cast<const MissionDocument *>(open.get())) {
			TerrainFamily theirs;
			if (resolve_terrain_family(view, *mission, theirs) && theirs.plane(TerrainPlane::Heights).path == heights)
				out.push_back(mission);
		}
	return out;
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

FollowResult follow_the_ground(SessionCore &core, const FollowGround &ground) {
	FollowResult result;
	const SessionView &view = core.view();
	if (!view.project.open || !view.project.scan || !view.findings.assets || !ground.before ||
	    !ground.after || ground.before->size() != kAtlasWords || ground.after->size() != kAtlasWords ||
	    *ground.before == *ground.after)
		return result;
	const std::shared_ptr<const ProjectAssetSource> files = view.findings.assets;
	const std::string cpt_name = basename_of(ground.cpt);
	const std::string wanted = strutil::to_lower(cpt_name);
	DocumentSet &documents = core.documents();
	// Every mission of the project, in path order, on the ground its header and .env resolve: those reading the .cpt.
	std::vector<std::string> paths;
	for (const AssetEntry &entry : view.project.scan->entries)
		if (entry.kind == AssetKind::Mission) paths.push_back(entry.relative_path);
	std::sort(paths.begin(), paths.end());
	std::map<std::string, MissionGround> grounds;
	for (const std::string &path : paths) {
		DocumentBase *open = documents.document_for(path);
		mission::MissionInfo info;
		bms::File file;
		std::vector<int32_t> locks;
		if (const auto *document = dynamic_cast<const MissionDocument *>(open)) {
			if (const MissionRow *row = document->mission_row()) info = mission::mission_info(row->native);
		} else if (!open) {
			const AssetEntry *entry = view.project.scan->at_path(path);
			if (!entry || !read_closed_mission(view, *files, *entry, file, locks)) continue;
			info = mission::mission_info(file);
		}
		const MissionGround &on = mission_ground(grounds, *files, info.terrain, info.environment, wanted);
		if (!on.follows) continue;
		const mission::MisTerrain ground_before = base_terrain_sampler(on.trn, on.scale, ground.before->data());
		const mission::MisTerrain ground_after = base_terrain_sampler(on.trn, on.scale, ground.after->data());
		bool opened = false;
		if (!open) {
			// Opened only where an entity its .mis does not lock moves.
			if (!file_moves(file, locks, ground_before, ground_after)) continue;
			open = documents.open_quietly(path, "its entities follow the ground of " + cpt_name);
			if (!open) continue;
			opened = true;
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
		const FollowMark *mark = nullptr;
		if (ground.marks)
			if (const auto found = ground.marks->find(path); found != ground.marks->end()) mark = &found->second;
		std::vector<std::pair<NodeAddress, int32_t>> changes =
		        follow_changes(*mission, ground_before, ground_after, ground.marks != nullptr, mark);
		if (changes.empty()) {
			// Opened for nothing (its entities' locks as its load read them lock every one that moves): closed again.
			if (opened && !mission->unsaved()) documents.discard(path);
			continue;
		}
		// A drag open in it: its follow waits for the drag's end (its step would split the drag).
		bool dragging = false;
		for (const OpenGesture &gesture : view.documents.gestures) dragging = dragging || gesture.in(path);
		if (dragging) {
			FollowDeferred deferred;
			deferred.mission = path;
			deferred.cpt_name = cpt_name;
			deferred.view = ground.view;
			deferred.identity = mission->identity();
			deferred.link = ground.link;
			deferred.changes = std::move(changes);
			result.deferred.push_back(std::move(deferred));
			continue;
		}
		const std::vector<Edit> edits = follow_edits(*mission, changes, ground.link);
		if (edits.empty() || !apply_follow(core, *mission, edits, ground.view, cpt_name, ground.link)) continue;
		result.moved += edits.size();
		result.missions.push_back(path);
		std::vector<NodeId> &rows = result.rows[path];
		for (const auto &change : changes) rows.push_back(change.first.row);
		std::sort(rows.begin(), rows.end());
	}
	return result;
}

bool apply_deferred_follow(SessionCore &core, const FollowDeferred &deferred) {
	const SessionView &view = core.view();
	for (const OpenGesture &gesture : view.documents.gestures)
		if (gesture.in(deferred.mission)) return false;
	auto *mission = dynamic_cast<MissionDocument *>(core.documents().document_for(deferred.mission));
	if (!mission || mission->identity() != deferred.identity || mission->blocked()) return true;
	const std::vector<Edit> edits = follow_edits(*mission, deferred.changes, deferred.link);
	if (!edits.empty()) apply_follow(core, *mission, edits, deferred.view, deferred.cpt_name, deferred.link);
	return true;
}

} // namespace opennova::editor
