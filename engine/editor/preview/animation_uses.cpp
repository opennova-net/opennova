#include <editor/preview/animation_uses.h>

#include <algorithm>
#include <set>

#include <base/io/strutil.h>
#include <editor/documents/animation_map_document.h>
#include <editor/documents/animation_slots.h>
#include <editor/graph/asset_graph.h>
#include <editor/preview/model_preview_rig.h>
#include <formats/avatars/preview_animation.h>
#include <runtime/anim/adm_fallback.h>
#include <runtime/anim/rig_files.h>

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
		// model its graphic names, and on its graphic_enemy (the pairing the preview's rig takes,
		// preview_model_fields).
		player.first_person = edge->field == "animadm";
		const std::vector<const char *> fields = preview_model_fields(edge->field);
		for (const GraphEdge *graphic : graph.references_of(edge->source)) {
			if (graphic->record != edge->record || graphic->kind != ReferenceKind::Model) continue;
			const std::string model = file_name(scan_file(scan, graphic->target, AssetKind::Model, ".3di"));
			if (model.empty()) continue;
			if (!fields.empty() && graphic->field == fields[0] && player.model.empty()) player.model = model;
			if (graphic->field == "graphic_enemy" && edge->field == "anim_def") player.enemy_model = model;
		}
		if (strutil::iequals(player.enemy_model, player.model)) player.enemy_model.clear();
		if (player.model.empty()) std::swap(player.model, player.enemy_model);
		out.push_back(std::move(player));
	}
	return out;
}

std::vector<ModelAnimation> model_animations(const AssetGraph &graph, const AssetScan &scan,
                                             const std::string &model_path) {
	std::vector<ModelAnimation> out;
	std::set<std::pair<std::string, std::string>> seen;
	const AssetEntry *fallback = scan.find(anim::kDefaultAdmName);
	for (const GraphEdge *edge : graph.referrers_of_file(model_path)) {
		if (edge->kind != ReferenceKind::Model) continue;
		for (const GraphEdge *map : graph.references_of(edge->source)) {
			if (map->record != edge->record || map->kind != ReferenceKind::AnimationMap) continue;
			const std::vector<const char *> fields = preview_model_fields(map->field);
			if (std::none_of(fields.begin(), fields.end(), [&](const char *field) { return edge->field == field; })) continue;
			std::string file = scan_file(scan, map->target, AssetKind::AnimationMap, ".adm");
			std::string via = map->field == "animadm" ? "first person" : edge->field == "graphic_enemy" ? "as an enemy" : "";
			if (file.empty()) {
				// A map the game lacks plays as default.adm.
				if (!fallback || fallback->kind != AssetKind::AnimationMap) continue;
				file = fallback->relative_path;
				const std::string named = map->target.find('.') == std::string::npos ? map->target + ".adm" : map->target;
				via = (via.empty() ? std::string() : via + ", ") + "in place of " + named;
			}
			if (!seen.insert({file, edge->record}).second) continue;
			out.push_back({file, edge->record, edge->source, via});
		}
	}
	return out;
}

std::vector<std::string> animated_models(const AssetGraph &graph, const AssetScan &scan) {
	std::set<std::string> sources;
	for (const ReferenceKind kind : {ReferenceKind::Item, ReferenceKind::Weapon})
		for (const GraphSymbol *symbol : graph.symbols_of_kind(kind)) sources.insert(symbol->file);
	std::vector<std::string> out;
	std::set<std::string> seen;
	for (const std::string &source : sources) {
		const std::vector<const GraphEdge *> edges = graph.references_of(source);
		std::set<std::pair<std::string, std::string>> pairs; // (record, model field) pairing a map
		for (const GraphEdge *edge : edges)
			if (edge->kind == ReferenceKind::AnimationMap)
				for (const char *field : preview_model_fields(edge->field)) pairs.insert({edge->record, field});
		for (const GraphEdge *edge : edges) {
			if (edge->kind != ReferenceKind::Model || !pairs.count({edge->record, edge->field})) continue;
			const std::string file = file_name(scan_file(scan, edge->target, AssetKind::Model, ".3di"));
			if (!file.empty() && seen.insert(strutil::to_lower(file)).second) out.push_back(file);
		}
	}
	std::sort(out.begin(), out.end(),
	          [](const std::string &a, const std::string &b) { return strutil::to_lower(a) < strutil::to_lower(b); });
	return out;
}

// [orig: AnimMap_FindOrLoadBoneFile @ 0x40C030, the failsafe @ 0x40C25B..0x40C2A1; PlayerInfo_InitPreviewModel
// @ 0x5600D0, its two .bad loads @ 0x56013D, @ 0x56014C; AnimMap_LoadAdmFile @ 0x40CD00..0x40CD25]
std::string clip_unused_words(const AssetGraph &graph, const std::string &clip_path) {
	const std::string name = file_name(clip_path);
	if (strutil::iequals(name, anim::kFailsafeClip))
		return "No map of the project names this clip: the game plays it in place of any clip a map names that does "
		       "not load.";
	if (strutil::iequals(name, avatars::kPreviewSkeletonBad))
		return "No map of the project names this clip: the menu's player preview binds its skeleton on it.";
	if (strutil::iequals(name, avatars::kPreviewIdleBad))
		return "No map of the project names this clip: the menu's player preview plays it.";
	if (graph.base())
		return "No map of this project names this clip; the base layer's own maps may (their names are not read here).";
	return "No map of the project names this clip: the game never plays it.";
}

std::string map_unused_words(const AssetGraph &graph, const std::string &map_path) {
	if (strutil::iequals(file_name(map_path), anim::kDefaultAdmName))
		return "No item or weapon of the project names this map: the game plays it for any whose map it lacks.";
	if (graph.base())
		return "No item or weapon of this project names this map; the base layer's own may (their names are not read "
		       "here).";
	return "No item or weapon of the project names this map: the game plays it for none.";
}

ClipLoads project_clip_loads(const AssetScan &scan) {
	if (scan.find(anim::kFailsafeClip)) return [](const std::string &clip) { return !clip.empty(); };
	return [&scan](const std::string &clip) {
		if (clip.empty()) return false;
		const AssetEntry *entry = scan.find(anim::bad_file_name(clip));
		return entry && entry->kind == AssetKind::Animation;
	};
}

namespace {

// Whether a row's clips register any clip (an empty predicate: any named one).
bool row_authors(const AnimationMapRow &row, const ClipLoads &loads) {
	for (const std::string &clip : row.clips)
		if (!clip.empty() && (!loads || loads(clip))) return true;
	return false;
}

size_t row_loading_clips(const AnimationMapRow &row, const ClipLoads &loads) {
	size_t count = 0;
	for (const std::string &clip : row.clips) count += !clip.empty() && (!loads || loads(clip)) ? 1 : 0;
	return count;
}

} // namespace

std::vector<int> map_unauthored_slots(const AnimationMapDocument &document, const ClipLoads &loads) {
	std::set<int> authored;
	bool body = false, first_person = false;
	for (const auto &node : document.rows()) {
		if (!node) continue;
		const auto &row = static_cast<const AnimationMapRow &>(*node);
		const int slot = animation_key_slot(row.key);
		if (slot < 0) continue;
		if (slot >= 1 && slot < kAnimFirstPersonSlot) body = true;
		if (slot >= kAnimFirstPersonSlot) first_person = true;
		if (row_authors(row, loads)) authored.insert(slot);
	}
	// A weapon's map (its slots past the reset all the first person's) leaves out first-person slots;
	// a body's, body slots.
	const bool weapon = first_person && !body;
	const int first = weapon ? kAnimFirstPersonSlot : 1;
	const int last = weapon ? kAnimFirstPersonSlot + 11 : kAnimBodySlotCount - 1;
	std::vector<int> out;
	for (int slot = first; slot <= last; ++slot)
		if (!authored.count(slot)) out.push_back(slot);
	return out;
}

std::vector<std::string> map_row_notes(const AnimationMapDocument &document, const NodeAddress &address,
                                       const ClipLoads &loads) {
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
	// None of its clips registers: the slot is as one the map leaves out (unless another row gives it).
	const size_t loading = row_loading_clips(row, loads);
	if (slot != 0 && loading == 0)
		out.push_back(std::string(row.clips.empty() ? "This row names no clip. " : "None of its clips loads. ") +
		              animation_slot_when_missing(slot));
	// The turn: a row serves its clips from the last back, one step at every play and every loop, and
	// the ring heads are the loaded map's, shared by both channels of every body on it [orig:
	// AnimMap_RegisterBoneNode @ 0x40C2D0, the ring insert @ 0x40C37F..0x40C385; AnimMap_LoadAdmFile @
	// 0x40CC40, the reuse by name @ 0x40CD2F; AnimMap_LinkEntity @ 0x40BA10, @ 0x40BA77].
	if (slot != 0 && loading > 1)
		out.push_back("The game plays the " + std::to_string(loading) +
		              " clips that load in turn, from the last back to the first: one step each time any body on this "
		              "map plays the slot or loops it (the turn is the map's, shared by every body on it and by both of "
		              "a body's channels).");
	// The reset row: each reset clip replaces slot 0's head, the bind pinned is the last, and the
	// registration backfills every unauthored slot with the first [orig: AnimMap_RegisterBoneNode @
	// 0x40C2D0, @ 0x40C38B..0x40C38F, the backfill @ 0x40C39A..0x40C3E2; AnimMap_LoadAdmFile @ 0x40CC40,
	// the pin @ 0x40CE19; with clips but no reset the null head is read @ 0x40CE11..0x40CE16, with none
	// the load fails @ 0x40CE03..0x40CE07]; a mission's forced animation keeps a slot left out [orig:
	// Script_ForceAnimation @ 0x4F2610; Entity_UpdateInfantryAI, the consumer @ 0x4BD256]; a map name
	// the mount lacks loads default.adm [orig: AnimMap_LoadAdmFile @ 0x40CD00..0x40CD25].
	if (slot == 0) {
		if (loading > 1)
			out.push_back("Each reset clip replaces the one before: the skeleton is the last that loads, and the slots "
			              "the map leaves out serve the first.");
		const std::vector<int> left_out = map_unauthored_slots(document, loads);
		if (!left_out.empty()) {
			size_t reset = 0, never = 0, untraced = 0;
			for (const int s : left_out) {
				const AnimSlotAbsence absence = animation_slot_absence(s);
				reset += absence == AnimSlotAbsence::PlaysReset ? 1 : 0;
				never += absence == AnimSlotAbsence::NotPicked ? 1 : 0;
				untraced += absence == AnimSlotAbsence::Untraced ? 1 : 0;
			}
			std::vector<std::string> parts;
			if (reset) parts.push_back("the game plays it in " + std::to_string(reset) + " of them");
			if (never) parts.push_back("never picks " + std::to_string(never) + " of them without a clip");
			if (untraced) parts.push_back("for " + std::to_string(untraced) + " whether it picks them then is not traced");
			std::string line = "This map leaves out " + std::to_string(left_out.size()) +
			                   " slots, each serving this row's first clip:";
			for (size_t i = 0; i < parts.size(); ++i)
				line += (i == 0 ? " " : i + 1 == parts.size() ? (parts.size() > 2 ? ", and " : " and ") : ", ") + parts[i];
			out.push_back(line + ". A mission's forced animation of any of them plays this row's first clip.");
		}
		out.push_back("A map whose rows register clips but no reset clip crashes the game as it loads (it reads the "
		              "reset clip the map lacks), and an item whose map the game lacks plays default.adm.");
	}
	// A slot an earlier row names: its clips join that slot's turn (a reset clip replaces the head)
	// [orig: AnimMap_RegisterBoneNode @ 0x40C2D0, @ 0x40C37F..0x40C38F].
	for (const auto &other : document.rows()) {
		if (!other || other->id == node->id) break;
		if (animation_key_slot(static_cast<const AnimationMapRow &>(*other).key) != slot) continue;
		out.push_back(slot == 0 ? std::string("An earlier row names the reset slot too: each reset clip replaces the one "
		                                      "before, so the skeleton is the last reset clip that loads, and the slots "
		                                      "left out serve the very first.")
		                        : "An earlier row names this slot too: the game joins their clips into one turn.");
		break;
	}
	return out;
}

} // namespace opennova::editor
