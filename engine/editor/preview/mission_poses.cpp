#include <editor/preview/mission_poses.h>

#include <cmath>
#include <map>

#include <base/io/hash.h>
#include <base/io/bam.h>
#include <base/io/fixed.h>
#include <base/io/os_path.h>
#include <base/io/strutil.h>
#include <editor/assets/project_asset_source.h>
#include <editor/graph/asset_graph.h>
#include <editor/preview/mission_ground_facts.h>
#include <editor/preview/mission_scene.h>
#include <editor/preview/mission_world.h>
#include <editor/preview/model_preview_rig.h>
#include <editor/preview/viewport_follow.h>
#include <editor/session/view/session_view.h>
#include <formats/def/def.h>
#include <formats/mission/bms.h>
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
			a.ssn == b.ssn && a.alert == b.alert && a.guarding == b.guarding &&
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
	floor_files_.reset();
	floor_world_.reset();
	floor_bodies_.clear();
	floor_key_ = 0;
	floor_mission_.reset();
	floor_ground_ = nullptr;
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
	const std::string name = io::utf8_file_name(file);
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
		inputs.push_back(MissionPoseInput{ entity.row, entity.item, entity.attributes, entity.route, entity.ssn,
				uint8_t(entity.alert) });
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
			pose.stood = were[i].stood;
			pose.on = were[i].on;
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

int32_t mission_pose_floor_clearance(MissionWorld &world, world::EntityHandle body, const MissionPose &pose,
		const std::vector<MissionEntityMark> &marks, NodeId &on) {
	on = 0;
	const world::AiEntity *row = world.world.ai.for_handle(body);
	if (row == nullptr) return INT32_MAX;
	// The body lifted by its rise; the tail's probe from its height raised to the 6144 grid, down 2.0 u.
	int32_t pos[3] = { row->pos[0], row->pos[1],
		io::bam_add(row->pos[2], int32_t(std::lround(pose.rise * io::kFp16OneD))) };
	const int32_t feet = pos[2] - pose.capsule_bottom;
	pos[2] = world::ground_probe_origin_z(pos[2]);
	world::EntityHandle met;
	const int32_t ground = world.collision.raycast_ground(world.world, body, pos, 0, 0, 0, 0x20000, &met);
	if (const world::Entity *floor = world.world.registry.get(met))
		if (const MissionEntityMark *mark = world.mark_of(*floor, marks)) on = mark->row;
	return feet - ground;
}

std::unique_ptr<MissionWorld> MissionPoses::load_floor_world() const {
	if (!floor_world_ || !floor_mission_ || !floor_ground_) return nullptr;
	auto world = std::make_unique<MissionWorld>();
	world->load(floor_files_, *floor_mission_, *floor_ground_);
	return world;
}

bool MissionPoses::floor_files_moved() const {
	return floor_files_ && source_ && floor_files_->stamped->stamps().moved(*source_);
}

bool MissionPoses::stand(const MissionScene &scene, const MissionPeopleFloor &floor) {
	const auto fixed = [](double units) { return int32_t(std::lround(units * io::kFp16OneD)); };
	const terrain::TerrainHeightField *terrain = floor.ground ? floor.ground->height_field() : floor.terrain;
	// The mission's world the solve probes: the posed people, and the items and buildings within
	// reach of their candidates (a pool-0 body's slice reaches its bound and 4.0 u past it to a
	// candidate's bound [orig: Entity_BuildProximityListsFromPools @0x4B8F02..0x4B8F0B], two units
	// more over the static table's padded radius word; an item of no known bound within
	// kMissionFloorReach), every other record loaded empty in its place. The files the floors read
	// noted afresh where one moved, the world built or not (else the move stands and the people are
	// stood again every frame).
	if (source_ && (!floor_files_ || floor_files_moved())) {
		floor_files_ = std::make_shared<MissionFiles>(source_);
		floor_world_.reset();
	}
	MissionWorld *world = nullptr;
	if (terrain && floor.mission && floor.ground && source_) {
		const auto bound = [&](int64_t item) -> double {
			if (!floor.bounds) return 0.0;
			const auto found = floor.bounds->find(item);
			return found != floor.bounds->end() ? double(found->second) : 0.0;
		};
		std::vector<const MissionEntityMark *> people;
		for (const MissionPose &pose : poses_)
			if (pose.status == "posed")
				if (const MissionEntityMark *mark = scene.entity(pose.row)) people.push_back(mark);
		const auto keep = [&](const MissionEntityMark &mark) {
			if (mark.pool == MissionPool::Organic) {
				const MissionPose *posed = this->pose(mark.row);
				return posed && posed->status == "posed";
			}
			if (mark.pool != MissionPool::Item && mark.pool != MissionPool::Building) return false;
			const double own = bound(mark.item);
			for (const MissionEntityMark *person : people) {
				const double reach =
						own > 0.0 ? own + bound(person->item) + kMissionFloorPad + 2.0 : kMissionFloorReach;
				const double dx = mark.x - person->x, dy = mark.y - person->y, dz = mark.z - person->z;
				if (dx * dx + dy * dy + dz * dz <= reach * reach) return true;
			}
			return false;
		};
		if (!people.empty()) {
			// The world made again only where what it holds moved: the records it keeps (each one's
			// row, place, item, transform and stamp), its ground, the files it reads. A drag of a
			// record no person reaches keeps it.
			const auto mix = [](uint64_t hash, uint64_t value) {
				return io::fnv1a64_bytes(hash, &value, sizeof(value));
			};
			uint64_t key = mix(io::kFnv1a64Offset, floor.ground->revision());
			key = mix(key, uint64_t(scene.entities().size()));
			for (const MissionEntityMark &mark : scene.entities()) {
				if (!keep(mark))
					continue;
				key = mix(key, mark.row);
				key = mix(key, uint64_t(uint32_t(mark.index)));
				key = mix(key, uint64_t(mark.item));
				key = mix(key, mark.stamp);
			}
			if (!floor_world_ || key != floor_key_) {
				floor_mission_ = std::make_unique<bms::File>(*floor.mission);
				mission_world_thin(*floor_mission_, scene.entities(), keep);
				floor_ground_ = floor.ground;
				floor_world_ = std::make_unique<MissionWorld>();
				floor_world_->load(floor_files_, *floor_mission_, *floor.ground);
				floor_bodies_.clear();
				floor_world_->world.registry.for_each_in_pool(0, [&](const world::Entity &entity) {
					if (const MissionEntityMark *mark =
									floor_world_->mark_of(entity, scene.entities()))
						floor_bodies_[mark->row] = entity.handle;
				});
				floor_key_ = key;
				++floor_serial_;
			}
			world = floor_world_.get();
		}
	}
	const std::map<NodeId, world::EntityHandle> none;
	const std::map<NodeId, world::EntityHandle> &bodies = world ? floor_bodies_ : none;
	bool moved = false;
	for (MissionPose &pose : poses_) {
		const MissionEntityMark *entity = scene.entity(pose.row);
		if (pose.status != "posed" || entity == nullptr) continue;
		const int32_t rise = fixed(pose.rise);
		double lift = pose.rise;
		bool settled = false;
		NodeId on = 0;
		// The clearance below one unit taken off, a positive one included.
		const auto body = bodies.find(pose.row);
		const int32_t clearance = body != bodies.end()
				? mission_pose_floor_clearance(*world, body->second, pose, scene.entities(), on)
				: mission_pose_clearance(pose, entity->x, entity->y, entity->z, terrain);
		if (clearance < world::kOrganicWarmupSettleQ16) {
			lift = double(io::bam_sub(rise, clearance)) / io::kFp16OneD;
			settled = true;
		} else {
			on = 0;
		}
		const bool stood = terrain != nullptr;
		if (lift == pose.lift && settled == pose.settled && stood == pose.stood && on == pose.on) continue;
		pose.lift = lift;
		pose.settled = settled;
		pose.stood = stood;
		pose.on = on;
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
		pose_person(*definition, PersonRecord{ input.ssn, input.route, input.attributes, input.alert }, has_file, *files,
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
	pose.ssn = record.ssn;
	pose.alert = pose.ai_slot ? record.alert : 0;
	pose.guarding = (facts.flags & world::kEntityFlagMounted) != 0;
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
	out.set("alert", json_number(pose.alert));
	out.set("lift", json_number(pose.lift));
	out.set("settled", JsonValue::make_bool(pose.settled));
	if (pose.on) out.set("on", json_number(double(pose.on)));
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
