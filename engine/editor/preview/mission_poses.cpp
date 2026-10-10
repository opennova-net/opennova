#include <editor/preview/mission_poses.h>

#include <cmath>

#include <base/io/bam.h>
#include <base/io/fixed.h>
#include <base/io/strutil.h>
#include <editor/assets/project_asset_source.h>
#include <editor/graph/asset_graph.h>
#include <editor/preview/mission_scene.h>
#include <editor/preview/model_preview_rig.h>
#include <editor/preview/viewport_follow.h>
#include <editor/session/view/session_view.h>
#include <formats/def/def.h>
#include <runtime/anim/adm_fallback.h>
#include <runtime/anim/adm_root_motion.h>
#include <runtime/assets/asset_store.h>
#include <runtime/mission/collision_resolve.h>
#include <runtime/world/collision.h>
#include <runtime/world/entity.h>
#include <runtime/world/entity_spawn.h>

namespace opennova::editor {

namespace {

using io::JsonValue;
using io::json_number;
using io::json_string;

// A project file by the name the asset source serves it by (a path's file name).
std::string served_name(const std::string &file) {
	const size_t slash = file.find_last_of('/');
	return slash == std::string::npos ? file : file.substr(slash + 1);
}

// The record's field that chose the init's request.
const char *because_of(const world::OrganicSpawnFacts &facts, int state) {
	if (facts.route_channel == 126) return "route_126";
	if (facts.route_channel == 127) return "route_127";
	if (state == world::anim_state::kGuard) return "guard";
	if (state == world::anim_state::kWalkForward) return "route";
	return "idle";
}

bool same_pose(const MissionPose &a, const MissionPose &b) {
	const world::InfantryBodyPose &p = a.pose, &q = b.pose;
	return a.item == b.item && a.status == b.status && a.ai_function == b.ai_function && a.adm == b.adm &&
			a.ai_slot == b.ai_slot && a.state == b.state && a.updates == b.updates && a.rise == b.rise &&
			a.clip == b.clip &&
			a.source_clip == b.source_clip && p.state == q.state && p.phase == q.phase && p.variant == q.variant &&
			p.parked == q.parked && p.blending == q.blending && p.source_state == q.source_state &&
			p.source_phase == q.source_phase && p.source_variant == q.source_variant && p.weight == q.weight;
}

} // namespace


MissionPoses::MissionPoses() = default;
MissionPoses::~MissionPoses() = default;

void MissionPoses::clear() {
	source_.reset();
	files_.reset();
	motion_.reset();
	catalogs_.clear();
	inputs_.clear();
	resolved_.clear();
	graph_ = 0;
	files_generation_ = 0;
	read_ = false;
	poses_.clear();
	rows_.clear();
}

const MissionPose *MissionPoses::pose(NodeId row) const {
	const auto found = rows_.find(row);
	return found == rows_.end() ? nullptr : &poses_[found->second];
}

size_t MissionPoses::posed() const {
	size_t count = 0;
	for (const MissionPose &pose : poses_) count += pose.status == "posed" ? 1 : 0;
	return count;
}

const MissionPoses::Catalog &MissionPoses::catalog_(const std::string &file) {
	Catalog &catalog = catalogs_[file];
	const std::string name = served_name(file);
	const uint64_t stamp = files_->stamp(name);
	if (catalog.stamp == stamp && stamp != 0) return catalog;
	catalog = Catalog();
	catalog.stamp = stamp;
	std::vector<uint8_t> bytes;
	if (!files_->read(name, bytes) || bytes.empty()) return catalog;
	++files_read_;
	def::DefItemsFile items{};
	if (def::def_parse_items_memory(bytes.data(), bytes.size(), &items) == 0) {
		// A type id resolves to its first row (mission::item_defs_by_id).
		for (const auto &entry : mission::item_defs_by_id(items)) {
			const def::DefItemDef &row = *entry.second;
			PersonDefinition definition;
			definition.ai_function = row.ai_function;
			definition.anim_def = row.anim_def;
			definition.attrib = row.attrib;
			definition.sound_profile = row.sound_profile;
			definition.sound_profile_female = row.sound_profile_female;
			definition.move_function = row.move_function;
			const char *ammo[] = { row.ammo_closeattack, row.ammo_easyrocket, row.ammo_advancedrocket, row.ammo_marker3 };
			const char *launch[] = { row.launchups_closeattack, row.launchups_rocket, row.launchups_marker3 };
			for (size_t slot = 0; slot < 4; ++slot) definition.ammo[slot] = ammo[slot];
			for (size_t slot = 0; slot < 3; ++slot) definition.launch[slot] = launch[slot];
			catalog.items.emplace(int64_t(row.id), std::move(definition));
		}
	}
	def::def_free_items(&items);
	return catalog;
}

bool MissionPoses::refresh(const SessionView &view, const MissionScene &scene) {
	std::vector<MissionPoseInput> inputs;
	for (const MissionEntityMark &entity : scene.entities()) {
		if (entity.pool != MissionPool::Organic) continue;
		inputs.push_back(MissionPoseInput{ entity.row, entity.item, entity.attributes, entity.route, entity.ssn });
	}
	const AssetGraph *graph = view.findings.graph.get();
	const uint64_t graph_generation = graph ? graph->generation() : 0;
	const uint64_t generation = view.findings.assets ? view.findings.assets->generation() : 0;
	// A file the poses read moved (a catalog edited, an .adm or a clip exported again): read afresh.
	bool files_moved = view.findings.assets != source_;
	if (!files_moved && generation != files_generation_ && files_ && source_)
		files_moved = files_->stamps().moved(*source_);
	files_generation_ = generation;
	// Another graph (any file's edit takes one, the mission's own included): each item resolved again,
	// the people posed again only where an item resolves to another catalog.
	const bool graph_moved = !read_ || graph_generation != graph_;
	graph_ = graph_generation;
	std::unordered_map<int64_t, std::string> resolved;
	if (!graph_moved) resolved = resolved_;
	for (const MissionPoseInput &input : inputs) {
		if (resolved.count(input.item)) continue;
		const GraphSymbol *symbol =
				graph ? graph->resolve_symbol(ReferenceKind::Item, std::to_string(input.item)) : nullptr;
		resolved.emplace(input.item, symbol ? symbol->file : std::string());
	}
	if (read_ && !files_moved && inputs == inputs_ && resolved == resolved_) return false;
	if (files_moved) {
		source_ = view.findings.assets;
		files_ = source_ ? std::make_shared<StampedFiles>(source_) : nullptr;
		motion_.reset();
		catalogs_.clear();
	}
	resolved_ = std::move(resolved);
	inputs_ = std::move(inputs);
	read_ = true;
	const std::vector<MissionPose> were = std::move(poses_);
	pose_all_();
	// A pose's stamp stands while it does; the serial moves for each that changed.
	bool changed = were.size() != poses_.size();
	for (size_t i = 0; i < poses_.size(); ++i) {
		MissionPose &pose = poses_[i];
		const bool same_row = i < were.size() && were[i].row == pose.row;
		if (same_row) {
			// Where it stood, until stand() stands it again.
			pose.lift = were[i].lift;
			pose.settled = were[i].settled;
		}
		if (same_row && same_pose(were[i], pose)) {
			pose.stamp = were[i].stamp;
			continue;
		}
		pose.stamp = ++serial_;
		changed = true;
	}
	return changed;
}

int32_t mission_pose_clearance(const MissionPose &pose, double x, double y, double z,
		const terrain::TerrainHeightField *terrain) {
	if (terrain == nullptr) return INT32_MAX;
	const auto fixed = [](double units) { return int32_t(std::lround(units * io::kFp16OneD)); };
	// The warmup's ground solve: the body lifted by its rise, its feet's clearance over the terrain alone.
	// [orig: Entity_WarmUpOrganicAnimation @0x4B8BD8..0x4B8BF5]
	const int32_t at[3] = { fixed(x), fixed(y), io::bam_add(fixed(z), fixed(pose.rise)) };
	return world::terrain_settle_clearance(terrain, at, pose.capsule_bottom, false);
}

bool MissionPoses::stand(const MissionScene &scene, const terrain::TerrainHeightField *terrain) {
	const auto fixed = [](double units) { return int32_t(std::lround(units * io::kFp16OneD)); };
	bool moved = false;
	for (MissionPose &pose : poses_) {
		const MissionEntityMark *entity = scene.entity(pose.row);
		if (pose.status != "posed" || entity == nullptr) continue;
		const int32_t rise = fixed(pose.rise);
		double lift = pose.rise;
		bool settled = false;
		// The clearance below one unit taken off, a positive one included.
		const int32_t clearance = mission_pose_clearance(pose, entity->x, entity->y, entity->z, terrain);
		if (clearance < world::kOrganicWarmupSettleQ16) {
			lift = double(io::bam_sub(rise, clearance)) / io::kFp16OneD;
			settled = true;
		}
		if (lift == pose.lift && settled == pose.settled) continue;
		pose.lift = lift;
		pose.settled = settled;
		pose.stamp = ++serial_;
		moved = true;
	}
	return moved;
}

void MissionPoses::pose_all_() {
	++runs_;
	poses_.clear();
	rows_.clear();
	if (!motion_) motion_ = std::make_unique<anim::AdmRootMotion>();
	// The catalog the project's graph resolves the item to, its first row of the id.
	const auto item_of = [this](int64_t item) -> const PersonDefinition * {
		const auto resolved = resolved_.find(item);
		if (!files_ || resolved == resolved_.end() || resolved->second.empty()) return nullptr;
		const Catalog &catalog = catalog_(resolved->second);
		const auto found = catalog.items.find(item);
		return found == catalog.items.end() ? nullptr : &found->second;
	};
	const auto has_file = [this](const std::string &file) { return source_ && !source_->path_of(file).empty(); };
	mission_pose_people(inputs_, item_of, has_file, files_, *motion_, poses_, &rings_);
	for (size_t i = 0; i < poses_.size(); ++i) rows_[poses_[i].row] = i;
}

void mission_pose_people(const std::vector<MissionPoseInput> &inputs,
		const std::function<const PersonDefinition *(int64_t)> &item_of,
		const std::function<bool(const std::string &)> &has_file, const std::shared_ptr<const StampedFiles> &files,
		anim::AdmRootMotion &motion, std::vector<MissionPose> &out, world::AnimVariantRings *rings_out) {
	out.clear();
	const PreviewRigFiles rig_files(files);
	// One ring-head table per .adm, served by every person of it in the file's order (the init runs
	// pool 0's rows in their order) [orig: AnimMap_LoadAdmFile @0x40CC40 reuses the entry by name;
	// the organic inits Entity_SpawnFromBMSRecord @0x40E9F0 -> Entity_InitOrganicAI @0x4BFCC0].
	world::AnimVariantRings rings;
	for (const MissionPoseInput &input : inputs) {
		MissionPose pose;
		pose.row = input.row;
		pose.item = input.item;
		const PersonDefinition *definition = files ? item_of(input.item) : nullptr;
		if (!definition) {
			pose.status = "no_item";
			out.push_back(std::move(pose));
			continue;
		}
		pose_person(*definition, PersonRecord{ input.ssn, input.route, input.attributes }, has_file, *files,
				rig_files.store, motion, rings, pose);
		out.push_back(std::move(pose));
	}
	if (rings_out) *rings_out = rings;
}

void pose_person(const PersonDefinition &definition, const PersonRecord &record,
		const std::function<bool(const std::string &)> &has_file, const StampedFiles &files,
		const anim::RigFiles &rig_files, anim::AdmRootMotion &motion, world::AnimVariantRings &rings,
		MissionPose &pose) {
	pose.ai_function = definition.ai_function;
	pose.ai_slot = (definition.attrib & world::kItemAttribAIData) != 0;
	pose.definition = definition;
	if (!world::organic_init_class(definition.ai_function.c_str())) {
		pose.status = "class";
		return;
	}
	// The definition's .adm, default.adm where the project lacks it; an item naming none binds no
	// map [orig: AnimMap_LoadAdmFile @0x40cc40 — the empty name @0x40cca1, the miss's substitution
	// @0x40cd00..0x40cd25].
	if (definition.anim_def.empty()) {
		pose.status = "no_adm";
		return;
	}
	const std::string named = anim::adm_file_name(definition.anim_def);
	const std::string file = assets::asset_file_name(named, ".adm");
	files.stamp(file); // noted, so the file's coming or going poses again
	pose.adm = has_file(file) ? named : std::string(anim::kDefaultAdmName);
	const int adm_id = motion.register_adm(&rig_files, pose.adm);
	if (adm_id < 0) {
		pose.status = "no_clips";
		return;
	}
	// What the init reads of the record: its route and its Guarding attribute reach it through the
	// AI slot an `aidata` definition takes [orig: Entity_SpawnFromBMSRecord `test [eax+54h],100000h`
	// @0x40ED4E; the fold 2 -> Flags 0x40 @0x40ED9F; slot+140/+148 from the record's waypoint_id].
	// A placed record has no parent and no rotor's wash about it at the load: the mounts and the
	// helicopters' wash zones come after.
	const world::OrganicSpawnFacts facts =
			world::organic_spawn_facts_from_record(pose.ai_slot, record.route, record.attributes);
	pose.status = "posed";
	pose.state = world::organic_spawn_state(facts, &motion, adm_id);
	pose.because = because_of(facts, pose.state);
	pose.updates = world::organic_warmup_updates(uint32_t(record.ssn)) + 1;
	const world::OrganicSpawnBody spawned = world::organic_spawn_pose(facts, uint32_t(record.ssn), &motion, rings, adm_id);
	pose.pose = spawned.pose;
	pose.adm_id = adm_id;
	pose.channels = spawned.channels;
	pose.rise = double(spawned.rise) / io::kFp16OneD;
	pose.capsule_bottom = spawned.capsule_bottom;
	pose.lift = pose.rise; // stood on the terrain by MissionPoses::stand()
	pose.clip = motion.clip_file(adm_id, pose.pose.state, pose.pose.variant);
	if (pose.pose.blending) pose.source_clip = motion.clip_file(adm_id, pose.pose.source_state, pose.pose.source_variant);
}

io::JsonValue mission_pose_json(const MissionPose &pose) {
	JsonValue out = JsonValue::make_object();
	out.set("status", json_string(pose.status));
	if (!pose.ai_function.empty()) out.set("ai_function", json_string(pose.ai_function));
	if (pose.status == "no_item" || pose.status == "class") return out;
	out.set("adm", json_string(pose.adm));
	out.set("ai_slot", JsonValue::make_bool(pose.ai_slot));
	if (pose.status != "posed") return out;
	const world::InfantryBodyPose &body = pose.pose;
	out.set("state", json_number(pose.state));
	out.set("row", json_string(world::infantry_anim_key(pose.state)));
	out.set("because", json_string(pose.because));
	out.set("updates", json_number(double(pose.updates)));
	out.set("rise", json_number(pose.rise));
	out.set("lift", json_number(pose.lift));
	out.set("settled", JsonValue::make_bool(pose.settled));
	JsonValue playing = JsonValue::make_object();
	playing.set("state", json_number(body.state));
	playing.set("row", json_string(world::infantry_anim_key(body.state)));
	playing.set("phase", json_number(body.phase));
	playing.set("variant", json_number(body.variant));
	playing.set("clip", json_string(pose.clip));
	playing.set("parked", JsonValue::make_bool(body.parked));
	out.set("playing", std::move(playing));
	if (body.blending) {
		JsonValue from = JsonValue::make_object();
		from.set("state", json_number(body.source_state));
		from.set("row", json_string(world::infantry_anim_key(body.source_state)));
		from.set("phase", json_number(body.source_phase));
		from.set("variant", json_number(body.source_variant));
		from.set("clip", json_string(pose.source_clip));
		out.set("from", std::move(from));
		out.set("weight", json_number(body.weight));
	}
	return out;
}

} // namespace opennova::editor
