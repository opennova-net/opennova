#include <editor/preview/animation_uses.h>

#include <algorithm>
#include <set>

#include <base/io/strutil.h>
#include <editor/documents/animation_map_document.h>
#include <editor/documents/animation_slots.h>
#include <editor/graph/asset_graph.h>

namespace opennova::editor {

namespace {

// A file of a kind as the scan lists it, from a graph target (its extension optional).
std::string scan_file(const AssetScan &scan, const std::string &target, AssetKind kind, const char *extension) {
	const AssetEntry *entry = scan.find(target);
	if (!entry && !strutil::ends_with_icase(target, extension)) entry = scan.find(target + extension);
	return entry && entry->kind == kind ? entry->relative_path : std::string();
}

std::string file_name(const std::string &path) { return path.substr(path.find_last_of("/\\") + 1); }

// The row's key of a map edge's record ("anim_walk_forward/I_WalkF.bad": anim_walk_forward).
std::string record_key(const std::string &record) { return record.substr(0, record.find('/')); }

} // namespace

std::vector<ClipUse> clip_uses(const AssetGraph &graph, const AssetScan &, const std::string &clip_path) {
	std::vector<ClipUse> out;
	for (const GraphEdge *edge : graph.referrers_of_file(clip_path)) {
		if (edge->kind != ReferenceKind::Animation) continue;
		ClipUse use;
		use.map = edge->source;
		use.key = record_key(edge->record);
		const int slot = animation_key_slot(use.key);
		use.words = slot >= 0 ? animation_slot_words(slot) : use.key;
		use.edge = edge;
		out.push_back(std::move(use));
	}
	return out;
}

std::vector<MapPlayer> map_players(const AssetGraph &graph, const AssetScan &scan, const std::string &map_path) {
	std::vector<MapPlayer> out;
	for (const GraphEdge *edge : graph.referrers_of_file(map_path)) {
		if (edge->kind != ReferenceKind::AnimationMap) continue;
		MapPlayer player;
		player.record = edge->record;
		player.file = edge->source;
		player.edge = edge;
		// A weapon's animadm is its view model's map (the first person); an item's anim_def plays on the
		// model its graphic names (the pairing the preview's rig takes, model_preview_rig).
		player.first_person = edge->field == "animadm";
		if (edge->field == "anim_def")
			for (const GraphEdge *graphic : graph.references_of(edge->source)) {
				if (graphic->record != edge->record || graphic->kind != ReferenceKind::Model || graphic->field != "graphic")
					continue;
				player.model = file_name(scan_file(scan, graphic->target, AssetKind::Model, ".3di"));
				break;
			}
		out.push_back(std::move(player));
	}
	return out;
}

std::vector<ModelAnimation> model_animations(const AssetGraph &graph, const AssetScan &scan,
                                             const std::string &model_path) {
	std::vector<ModelAnimation> out;
	std::set<std::pair<std::string, std::string>> seen;
	for (const GraphEdge *edge : graph.referrers_of_file(model_path)) {
		if (edge->kind != ReferenceKind::Model || edge->field != "graphic") continue;
		for (const GraphEdge *map : graph.references_of(edge->source)) {
			if (map->record != edge->record || map->kind != ReferenceKind::AnimationMap || map->field != "anim_def") continue;
			const std::string file = scan_file(scan, map->target, AssetKind::AnimationMap, ".adm");
			if (file.empty() || !seen.insert({file, edge->record}).second) continue;
			out.push_back({file, edge->record, edge->source});
		}
	}
	return out;
}

std::vector<std::string> animated_models(const AssetGraph &graph, const AssetScan &scan) {
	std::set<std::string> sources;
	for (const GraphSymbol *symbol : graph.symbols_of_kind(ReferenceKind::Item)) sources.insert(symbol->file);
	std::vector<std::string> out;
	std::set<std::string> seen;
	for (const std::string &source : sources) {
		const std::vector<const GraphEdge *> edges = graph.references_of(source);
		std::set<std::string> animated; // the records naming a map
		for (const GraphEdge *edge : edges)
			if (edge->kind == ReferenceKind::AnimationMap && edge->field == "anim_def") animated.insert(edge->record);
		for (const GraphEdge *edge : edges) {
			if (edge->kind != ReferenceKind::Model || edge->field != "graphic" || !animated.count(edge->record)) continue;
			const std::string file = file_name(scan_file(scan, edge->target, AssetKind::Model, ".3di"));
			if (!file.empty() && seen.insert(strutil::to_lower(file)).second) out.push_back(file);
		}
	}
	std::sort(out.begin(), out.end(),
	          [](const std::string &a, const std::string &b) { return strutil::to_lower(a) < strutil::to_lower(b); });
	return out;
}

std::vector<int> map_unauthored_slots(const AnimationMapDocument &document) {
	std::set<int> named;
	bool first_person = false;
	for (const auto &node : document.rows()) {
		if (!node) continue;
		const int slot = animation_key_slot(static_cast<const AnimationMapRow &>(*node).key);
		if (slot < 0) continue;
		named.insert(slot);
		if (slot >= kAnimFirstPersonSlot) first_person = true;
	}
	// A weapon's map (its slots the first person's) leaves out first-person slots; a body's, body slots.
	std::vector<int> out;
	const int first = first_person ? kAnimFirstPersonSlot + 1 : 1;
	const int last = first_person ? kAnimFirstPersonSlot + 11 : kAnimBodySlotCount - 1;
	for (int slot = first; slot <= last; ++slot)
		if (!named.count(slot)) out.push_back(slot);
	return out;
}

std::vector<std::string> map_row_notes(const AnimationMapDocument &document, const NodeAddress &address) {
	std::vector<std::string> out;
	const Node *node = document.row(address.row);
	if (!node || node->kind != node_kind(AnimationMapKind::Row)) return out;
	const auto &row = static_cast<const AnimationMapRow &>(*node);
	const int slot = animation_key_slot(row.key);
	// A key naming no slot: the row is skipped before any clip loads [orig: AnimMap_ParseConfigLine @
	// 0x40CB60, the test @ 0x40CBA4].
	if (slot < 0) {
		out.push_back("'" + row.key + "' names none of the game's 252 slots (a key names its slot past its first five "
		              "characters): the game skips this row, and none of its clips loads.");
		return out;
	}
	out.push_back(animation_slot_words(slot) + " (slot " + std::to_string(slot) + "): " + animation_slot_meaning(slot));
	// The turn: a row serves its clips from the last back, one step at every play and every loop
	// [orig: AnimMap_RegisterBoneNode @ 0x40C2D0, the ring insert @ 0x40C37F..0x40C385;
	// AnimMap_PlayAnimBySlot @ 0x40BDA0; AnimMap_AdvanceToNextAnim @ 0x40BDF0].
	if (slot != 0 && row.clips.size() > 1)
		out.push_back("The game plays these " + std::to_string(row.clips.size()) +
		              " clips in turn, from the last back to the first: one step each time the slot plays or loops.");
	// The reset row: each reset clip replaces slot 0's head, the bind pinned is the last, and the
	// registration backfills every unauthored slot with the first [orig: AnimMap_RegisterBoneNode @
	// 0x40C2D0, @ 0x40C38B..0x40C38F, the backfill @ 0x40C39A..0x40C3E2; AnimMap_LoadAdmFile @ 0x40CC40,
	// the pin @ 0x40CE19, no reset @ 0x40CE03..0x40CE16]; a map name the mount lacks loads default.adm
	// [orig: AnimMap_LoadAdmFile @ 0x40CC40, @ 0x40CD00..0x40CD25] (runtime/anim/adm_fallback.h).
	if (slot == 0) {
		if (row.clips.size() > 1)
			out.push_back("Each reset clip replaces the one before: the skeleton is the last that loads, and the slots "
			              "the map leaves out play the first.");
		const std::vector<int> left_out = map_unauthored_slots(document);
		if (!left_out.empty())
			out.push_back("This map leaves out " + std::to_string(left_out.size()) +
			              " slots; each plays this row's first clip.");
		out.push_back("A map with no reset row does not load, and an item whose map the game lacks plays default.adm.");
	}
	// A slot an earlier row names: its clips join that slot's turn (a reset clip replaces the head)
	// [orig: AnimMap_RegisterBoneNode @ 0x40C2D0, @ 0x40C37F..0x40C38F].
	for (const auto &other : document.rows()) {
		if (!other || other->id == node->id) break;
		if (animation_key_slot(static_cast<const AnimationMapRow &>(*other).key) != slot) continue;
		out.push_back(slot == 0 ? std::string("An earlier row names the reset slot too: the game keeps only the last reset "
		                                      "clip it loads.")
		                        : "An earlier row names this slot too: the game joins their clips into one turn.");
		break;
	}
	return out;
}

} // namespace opennova::editor
