// The ground the game puts a mission's entities on (mission_ground_rules.h, DI-28).

#include <editor/preview/mission_ground_rules.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <base/io/bam.h>
#include <base/io/fixed.h>
#include <base/io/strutil.h>
#include <editor/preview/mission_ground_facts.h>
#include <editor/preview/mission_items.h>
#include <editor/preview/mission_scene.h>
#include <editor/preview/viewport_follow.h>
#include <formats/def/def.h>
#include <formats/mission/bms.h>
#include <formats/threedi/threedi_3di3.h>
#include <runtime/anim/adm_root_motion.h>
#include <runtime/assets/asset_store.h>
#include <runtime/mission/collision_resolve.h>
#include <runtime/world/angle.h>
#include <runtime/world/collision.h>
#include <runtime/world/entity_spawn.h>
#include <runtime/world/model_geometry.h>

namespace opennova::editor {

namespace {

using io::JsonValue;
using io::json_number;
using io::json_string;

constexpr double kUnit = 65536.0;
// The start's settle probes from one unit over the body [orig: Game_StartMission @0x525FC1, height 0x10000].
constexpr int32_t kProbeUp = 0x10000;
// How near another entity's volume a static's box comes and still touches it: a quarter unit.
constexpr int32_t kTouchMargin = 0x4000;
// A placed entity's place in the grid the probes look it up in: 64-unit cells.
constexpr int kCellShift = 22;

int32_t fixed(double units) { return bms::to_fixed_16_16(units); }
double units(int32_t q16) { return double(q16) / kUnit; }

// The rows of the physics table whose update keeps the entity's height (mission_ground_rules.h); every
// other row moves it. A name the table lacks takes row 0, null [orig: EntityDef_LookupPhysicsCallback
// @0x4a9240, stricmp over the rows of g_EntityClassPhysicsTable @0x82abc8].
constexpr const char *kKeepingRows[] = { "null", "envs", "ewep", "door", "genx", "upfx", "org0", "chld" };
constexpr const char *kTableRows[] = { "null", "envs", "ewep", "ele0", "door", "towr", "genx", "org0", "org1",
	"org2", "upfx", "nade", "rock", "schl", "clym", "arti", "squib", "CHel", "cveh", "ctank", "cbike", "cbot", "catv",
	"cpln", "ctrn", "chld", "aflr", "gflr", "rokt", "stng", "hlfr", "jvln", "arty", "psec" };

// The row a move_function binds ("null" for a name the table lacks).
std::string move_row(const std::string &move_function) {
	for (const char *row : kTableRows)
		if (strutil::iequals(row, move_function)) return row;
	return "null";
}
bool row_keeps_height(const std::string &row) {
	for (const char *keeping : kKeepingRows)
		if (strutil::iequals(keeping, row)) return true;
	return false;
}

// The volumes a body stands on, leans on or hangs from: the generic solid (CB, 1), the ladder (CL, 4), the
// vehicles' solid (VC, 7) and the players' (CP, 19) (docs/world/world-wac-ai-re.md 15.4); not the blink,
// trigger, armory, damage or occlusion boxes.
bool touch_type(int32_t type) { return type == 1 || type == 4 || type == 7 || type == 19; }

std::string metres(double value) {
	char text[32];
	std::snprintf(text, sizeof(text), "%.1f m", value);
	return text;
}
std::string height_words(double value) {
	char text[32];
	std::snprintf(text, sizeof(text), "%.2f", value);
	return text;
}

// The placement matrix of a record (16.16 position, its eulers whole degrees) at an item's scale, as the
// game's collision places an entity (world::collision_matrix_from_placement, the matrix
// CollisionWorld::target_view builds; BoneCallback_Simple @0x4e2600 copies it into every section).
world::CollisionMatrix placement(const MissionEntityMark &entity, int32_t scale_q16) {
	const int32_t at[3] = { fixed(entity.x), fixed(entity.y), fixed(entity.z) };
	return world::collision_matrix_from_placement(double(entity.yaw), double(entity.pitch), double(entity.roll), at,
			scale_q16);
}

// The mission's drawn items and buildings, each placed by its record: the type-1 solids the ground probe
// clips, and the boxes of every volume a body touches (touch_type), each looked up by a 64-unit grid.
class Placed {
public:
	struct Entry {
		NodeId row = 0;
		std::string name;
		const world::CollisionModel *model = nullptr;
		std::vector<world::CollisionMatrix> mats;
		int32_t x = 0, y = 0, reach = 0;
		bool solid = false;
	};

	void add(Entry entry, const world::CollisionMatrix &matrix) {
		const size_t index = entries_.size();
		entries_.push_back(std::move(entry));
		const Entry &e = entries_.back();
		if (e.solid) {
			const int32_t x0 = (e.x - e.reach) >> kCellShift, x1 = (e.x + e.reach) >> kCellShift;
			const int32_t y0 = (e.y - e.reach) >> kCellShift, y1 = (e.y + e.reach) >> kCellShift;
			for (int32_t cx = x0; cx <= x1; ++cx)
				for (int32_t cy = y0; cy <= y1; ++cy) solids_[key(cx, cy)].push_back(index);
		}
		for (const world::CollisionVolume &volume : e.model->volumes) {
			if (!touch_type(volume.type)) continue;
			const int32_t local[6] = { volume.min_x, volume.min_y, volume.min_z, volume.max_x, volume.max_y,
				volume.max_z };
			Box box;
			box.entry = index;
			world::collision_matrix_box_bounds(matrix, local, box.min, box.max);
			const size_t at = boxes_.size();
			boxes_.push_back(box);
			for (int32_t cx = box.min[0] >> kCellShift; cx <= (box.max[0] >> kCellShift); ++cx)
				for (int32_t cy = box.min[1] >> kCellShift; cy <= (box.max[1] >> kCellShift); ++cy)
					box_cells_[key(cx, cy)].push_back(at);
		}
	}

	// The highest solid of another entity than `self` a column at (x, y) from `top` down to `bottom` meets
	// (16.16): true with its height and the entity, as the game's probe clips its ray at the first volume it
	// enters [orig: Entity_RaycastCollision @0x413760, the candidate loop @0x4138c1..0x413abc ->
	// Entity_RaycastCollisionModel @0x413060, type-1 volumes alone].
	bool probe(int32_t x, int32_t y, int32_t top, int32_t bottom, NodeId self, int32_t &height, const Entry *&on) const {
		on = nullptr;
		if (bottom >= top) return false;
		const auto cell = solids_.find(key(x >> kCellShift, y >> kCellShift));
		if (cell == solids_.end()) return false;
		world::CollisionRay ray;
		ray.start[0] = ray.end[0] = x;
		ray.start[1] = ray.end[1] = y;
		ray.start[2] = top;
		ray.end[2] = bottom;
		ray.refresh();
		for (const size_t index : cell->second) {
			const Entry &e = entries_[index];
			if (e.row == self) continue;
			const double dx = double(x) - e.x, dy = double(y) - e.y;
			if (dx * dx + dy * dy > double(e.reach) * double(e.reach)) continue;
			world::CollisionTargetView view;
			view.model = e.model;
			view.matrices = e.mats.data();
			const int32_t before = ray.end[2];
			if (world::collision_raycast_model(view, ray) && ray.end[2] != before) on = &e;
		}
		if (!on) return false;
		height = ray.end[2];
		return true;
	}

	// Another entity than `self` whose volume a body touches the box (min, max) comes within kTouchMargin of:
	// what a static leaning on it, hanging from it or standing in it touches (null: none).
	const Entry *touched(const int32_t min[3], const int32_t max[3], NodeId self) const {
		for (int32_t cx = (min[0] - kTouchMargin) >> kCellShift; cx <= ((max[0] + kTouchMargin) >> kCellShift); ++cx)
			for (int32_t cy = (min[1] - kTouchMargin) >> kCellShift; cy <= ((max[1] + kTouchMargin) >> kCellShift); ++cy) {
				const auto cell = box_cells_.find(key(cx, cy));
				if (cell == box_cells_.end()) continue;
				for (const size_t at : cell->second) {
					const Box &box = boxes_[at];
					const Entry &e = entries_[box.entry];
					if (e.row == self) continue;
					bool apart = false;
					for (int axis = 0; axis < 3 && !apart; ++axis)
						apart = min[axis] > box.max[axis] + kTouchMargin || max[axis] < box.min[axis] - kTouchMargin;
					if (!apart) return &e;
				}
			}
		return nullptr;
	}

private:
	struct Box {
		size_t entry = 0;
		int32_t min[3] = { 0, 0, 0 }, max[3] = { 0, 0, 0 };
	};
	static uint64_t key(int32_t cx, int32_t cy) { return (uint64_t(uint32_t(cx)) << 32) | uint32_t(cy); }
	std::vector<Entry> entries_;
	std::vector<Box> boxes_;
	std::unordered_map<uint64_t, std::vector<size_t>> solids_, box_cells_;
};

} // namespace

const char *mission_ground_rule_token(MissionGroundRule rule) {
	switch (rule) {
	case MissionGroundRule::None: return "none";
	case MissionGroundRule::Static: return "static";
	case MissionGroundRule::Person: return "person";
	case MissionGroundRule::Mover: return "mover";
	}
	return "none";
}

const char *mission_support_token(MissionSupport on) {
	switch (on) {
	case MissionSupport::Nothing: return "nothing";
	case MissionSupport::Terrain: return "terrain";
	case MissionSupport::Water: return "water";
	case MissionSupport::Record: return "record";
	}
	return "nothing";
}

const char *mission_ground_state_token(MissionGroundState state) {
	switch (state) {
	case MissionGroundState::Grounded: return "grounded";
	case MissionGroundState::Floats: return "floats";
	case MissionGroundState::Buried: return "buried";
	case MissionGroundState::Falls: return "falls";
	case MissionGroundState::Hangs: return "hangs";
	}
	return "grounded";
}

io::JsonValue mission_ground_verdict_json(const MissionGroundVerdict &verdict) {
	JsonValue out = JsonValue::make_object();
	out.set("row", json_number(double(verdict.row)));
	out.set("item", json_number(double(verdict.item)));
	if (!verdict.name.empty()) out.set("name", json_string(verdict.name));
	out.set("rule", json_string(mission_ground_rule_token(verdict.rule)));
	if (!verdict.why.empty()) out.set("why", json_string(verdict.why));
	out.set("state", json_string(mission_ground_state_token(verdict.state)));
	if (verdict.on != MissionSupport::Nothing) {
		out.set("base", json_number(verdict.base));
		out.set("support", json_number(verdict.support));
		out.set("on", json_string(mission_support_token(verdict.on)));
		if (verdict.on_row != 0) {
			out.set("on_row", json_number(double(verdict.on_row)));
			out.set("on_name", json_string(verdict.on_name));
		}
		out.set("terrain", json_number(verdict.terrain));
	}
	if (verdict.rule == MissionGroundRule::Static && verdict.top != verdict.bottom) {
		out.set("bottom", json_number(verdict.bottom));
		out.set("top", json_number(verdict.top));
	}
	if (verdict.off() || verdict.state == MissionGroundState::Falls) {
		out.set("fix_z", json_number(verdict.fix_z));
		out.set("by_anchor", JsonValue::make_bool(verdict.by_anchor));
	}
	return out;
}

// --- the reads -------------------------------------------------------------------------------------

void MissionGroundReads::clear() {
	items_stamp_ = 0;
	items_read_ = false;
	items_.clear();
	models_.clear();
}

const MissionGroundReads::Item *MissionGroundReads::item(const FileSource &files, int64_t id) {
	const uint64_t stamp = files.stamp("items.def");
	if (!items_read_ || stamp != items_stamp_) {
		items_read_ = true;
		items_stamp_ = stamp;
		items_.clear();
		std::vector<uint8_t> bytes;
		if (stamp != 0 && files.read("items.def", bytes) && !bytes.empty()) {
			++parsed_;
			def::DefItemsFile parsed{};
			if (def::def_parse_items_memory(bytes.data(), bytes.size(), &parsed) == 0) {
				// A type id resolves to its first row (mission::item_defs_by_id).
				for (const auto &entry : mission::item_defs_by_id(parsed)) {
					const def::DefItemDef &row = *entry.second;
					Item item;
					item.name = row.display_name;
					item.graphic = row.graphic;
					item.move_function = row.move_function;
					item.type = row.type;
					item.scale_q16 = row.scale_q16;
					item.person.ai_function = row.ai_function;
					item.person.anim_def = row.anim_def;
					item.person.attrib = row.attrib;
					items_.emplace(int64_t(row.id), std::move(item));
				}
			}
			def::def_free_items(&parsed);
		}
	}
	const auto found = items_.find(id);
	return found == items_.end() ? nullptr : &found->second;
}

const MissionGroundReads::Model *MissionGroundReads::model(const FileSource &files, const std::string &graphic) {
	if (graphic.empty()) return nullptr;
	const std::string name = assets::asset_file_name(graphic, ".3di");
	const uint64_t stamp = files.stamp(name);
	if (stamp == 0) return nullptr;
	Kept &kept = models_[name];
	if (kept.stamp == stamp) return kept.model.read ? &kept.model : nullptr;
	kept = Kept();
	kept.stamp = stamp;
	std::vector<uint8_t> bytes;
	if (!files.read(name, bytes) || bytes.empty()) return nullptr;
	++parsed_;
	threedi::Threedi3di3 parsed{};
	if (threedi::threedi_3di3_read_memory(bytes.data(), bytes.size(), &parsed) == 0) {
		Model &model = kept.model;
		model.read = true;
		float anchor[3] = { 0.0f, 0.0f, 0.0f };
		if (threedi::threedi_3di3_ground_anchor(&parsed, anchor)) mission_model_point(anchor, model.anchor);
		if (parsed.collision != nullptr) {
			const threedi::ThreediCollisionModelData &data = parsed.collision->model_data;
			model.bounds = true;
			for (int i = 0; i < 6; ++i)
				model.box[i] = data.has_bbox_fp16 ? data.bbox_fp16[i] : io::float_to_fp16_16_round_sat(data.bbox[i]);
			auto collision = std::make_shared<world::CollisionModel>();
			if (world::collision_model_from_3di(parsed.collision, *collision) && !collision->volumes.empty()) {
				collision->finalize_sections();
				model.solid = collision->has_solid_volume;
				// The solids' horizontal reach: their type-1 volumes' farthest corner from the origin.
				double reach = 0.0;
				for (const world::CollisionVolume &volume : collision->volumes) {
					if (volume.type != 1) continue;
					for (const int32_t vx : { volume.min_x, volume.max_x })
						for (const int32_t vy : { volume.min_y, volume.max_y })
							reach = std::max(reach, std::sqrt(double(vx) * vx + double(vy) * vy));
				}
				model.reach_q16 = int32_t(std::min(reach + 1.0, double(INT32_MAX / 4)));
				model.collision = std::move(collision);
			}
		}
	}
	threedi::threedi_3di3_free(&parsed);
	return kept.model.read ? &kept.model : nullptr;
}

// --- the verdicts ----------------------------------------------------------------------------------

std::vector<MissionGroundVerdict> mission_ground_verdicts(const MissionScene &scene, const MissionGround &ground,
		MissionGroundReads &reads, const std::shared_ptr<const StampedFiles> &files) {
	std::vector<MissionGroundVerdict> out;
	const std::vector<MissionEntityMark> &entities = scene.entities();
	out.reserve(entities.size());
	const terrain::TerrainHeightField *field = ground.height_field();
	const bool water = ground.water();
	const int32_t water_z = water ? fixed(ground.water_height()) : 0;
	const int32_t slack = fixed(kMissionGroundSlack);
	const FileSource &source = *files;

	// Each entity's item and model, read once; every drawn item and building with collision volumes placed.
	struct Read {
		const MissionGroundReads::Item *item = nullptr;
		const MissionGroundReads::Model *model = nullptr;
		world::CollisionMatrix matrix;
	};
	std::vector<Read> read(entities.size());
	Placed placed;
	for (size_t i = 0; i < entities.size(); ++i) {
		const MissionEntityMark &entity = entities[i];
		if (entity.pool == MissionPool::Marker) continue;
		Read &r = read[i];
		r.item = reads.item(source, entity.item);
		if (!r.item || entity.pool == MissionPool::Organic) continue;
		r.model = reads.model(source, r.item->graphic);
		if (!r.model) continue;
		r.matrix = placement(entity, r.item->scale_q16);
		if (!r.model->collision) continue;
		Placed::Entry entry;
		entry.row = entity.row;
		entry.name = r.item->name;
		entry.model = r.model->collision.get();
		entry.mats.assign(entry.model->sections.size(), r.matrix);
		entry.x = fixed(entity.x);
		entry.y = fixed(entity.y);
		const double scale = r.item->scale_q16 != 0 ? std::fabs(double(r.item->scale_q16) / kUnit) : 1.0;
		entry.reach = int32_t(std::min(double(r.model->reach_q16) * scale, double(INT32_MAX / 4)));
		entry.solid = r.model->solid;
		placed.add(std::move(entry), r.matrix);
	}

	// The people, posed as the game spawns them (mission_poses.h), every .adm and clip read through `files`.
	std::vector<MissionPoseInput> inputs;
	for (const MissionEntityMark &entity : entities)
		if (entity.pool == MissionPool::Organic)
			inputs.push_back(MissionPoseInput{ entity.row, entity.item, entity.attributes, entity.route, entity.ssn });
	std::vector<MissionPose> poses;
	if (!inputs.empty()) {
		anim::AdmRootMotion motion;
		const auto item_of = [&](int64_t id) -> const PersonDefinition * {
			const MissionGroundReads::Item *item = reads.item(source, id);
			return item ? &item->person : nullptr;
		};
		const auto has_file = [&](const std::string &name) { return source.stamp(name) != 0; };
		mission_pose_people(inputs, item_of, has_file, files, motion, poses);
	}
	std::unordered_map<NodeId, const MissionPose *> pose_of;
	for (const MissionPose &pose : poses) pose_of[pose.row] = &pose;

	const auto stood_on = [](MissionGroundVerdict &verdict, const Placed::Entry *on) {
		verdict.on = on ? MissionSupport::Record : MissionSupport::Terrain;
		verdict.on_row = on ? on->row : 0;
		verdict.on_name = on ? on->name : std::string();
	};

	for (size_t i = 0; i < entities.size(); ++i) {
		const MissionEntityMark &entity = entities[i];
		const Read &r = read[i];
		MissionGroundVerdict verdict;
		verdict.row = entity.row;
		verdict.kind = entity.kind;
		verdict.item = entity.item;
		if (r.item) verdict.name = r.item->name;
		const int32_t x = fixed(entity.x), y = fixed(entity.y), z = fixed(entity.z);
		if (entity.pool == MissionPool::Marker) {
			verdict.why = "marker";
			out.push_back(std::move(verdict));
			continue;
		}
		if (!r.item) {
			verdict.why = "no_item";
			out.push_back(std::move(verdict));
			continue;
		}
		const std::string row = move_row(r.item->move_function);
		int32_t terrain = 0;
		const bool has_terrain = world::terrain_column_height(field, x, y, terrain);
		verdict.terrain = has_terrain ? units(terrain) : 0.0;

		if (entity.pool == MissionPool::Organic) {
			const auto found = pose_of.find(entity.row);
			const MissionPose *pose = found == pose_of.end() ? nullptr : found->second;
			if (!pose || pose->status != "posed") {
				verdict.why = pose ? pose->status : "no_item";
				out.push_back(std::move(verdict));
				continue;
			}
			verdict.rule = MissionGroundRule::Person;
			// A flying organic of an AI definition holds its altitude: the spawn sets Flags 0x80, whose org1
			// climb mode chases the spawn height in place of gravity [orig: Entity_SpawnFromBMSRecord
			// @0x40EE2A..0x40EE33, inside the AI branch `test [eax+54h],100000h` @0x40ED4E].
			if (pose->ai_slot && (entity.attributes & uint32_t(bms::BmsiAttributeFlags::FlyingOrganic)) != 0) {
				verdict.why = "flying";
				out.push_back(std::move(verdict));
				continue;
			}
			if (!has_terrain) {
				verdict.why = "no_terrain";
				out.push_back(std::move(verdict));
				continue;
			}
			// The warmup's ground solve over the terrain: a body under one unit over it, or under it, is set down.
			const int32_t clearance = mission_pose_clearance(*pose, entity.x, entity.y, entity.z, field);
			const int32_t origin = io::bam_add(z, fixed(pose->rise));
			const int32_t feet = io::bam_sub(origin, pose->capsule_bottom);
			verdict.base = units(feet);
			verdict.support = units(terrain);
			verdict.on = MissionSupport::Terrain;
			if (clearance < world::kOrganicWarmupSettleQ16) {
				verdict.why = "settled";
				out.push_back(std::move(verdict));
				continue;
			}
			// In the air: the motor's ground tail stands it on the solid its column meets from the probe's
			// origin (the body's height raised to the 6144 grid [orig: Entity_MovementCollisionResolver
			// @0x4B3D6E..0x4B3DA9]), else on the terrain; over the water plane, at its surface.
			int32_t support = terrain;
			const Placed::Entry *on = nullptr;
			int32_t hit = 0;
			if (placed.probe(x, y, world::ground_probe_origin_z(origin), terrain, entity.row, hit, on) && hit > support) {
				support = hit;
				stood_on(verdict, on);
			}
			if (water && water_z > support) {
				support = water_z;
				verdict.on = MissionSupport::Water;
				verdict.on_row = 0;
				verdict.on_name.clear();
			}
			verdict.support = units(support);
			const int32_t drop = io::bam_sub(feet, support);
			if (drop < world::kOrganicWarmupSettleQ16) {
				verdict.why = "standing";
				out.push_back(std::move(verdict));
				continue;
			}
			// Org1 and org2 fall under their motors' gravity [orig: Entity_UpdateInfantryAI @0x4b9910;
			// Entity_UpdateInfantryPlayerBody @0x4b40e0], landing where the game puts them; another class has
			// no update and leaves the body in the air.
			verdict.why = "move:" + row;
			verdict.state = (row == "org1" || row == "org2") ? MissionGroundState::Falls : MissionGroundState::Hangs;
			verdict.fix_z = units(io::bam_sub(z, drop));
			out.push_back(std::move(verdict));
			continue;
		}

		if (!row_keeps_height(row)) {
			verdict.rule = MissionGroundRule::Mover;
			verdict.why = "move:" + row;
			out.push_back(std::move(verdict));
			continue;
		}
		if (!r.model) {
			verdict.why = "no_model";
			out.push_back(std::move(verdict));
			continue;
		}
		verdict.rule = MissionGroundRule::Static;
		if (!has_terrain) {
			verdict.why = "no_terrain";
			out.push_back(std::move(verdict));
			continue;
		}
		// Its ground anchor's height and its model's span as the record places it (its collision box; the
		// anchor alone without one).
		const double anchor = r.model->anchor[2];
		const int32_t base = fixed(entity.z + anchor);
		int32_t box_min[3] = { x, y, base }, box_max[3] = { x, y, base };
		if (r.model->bounds) world::collision_matrix_box_bounds(r.matrix, r.model->box, box_min, box_max);
		const int32_t bottom = box_min[2], top = box_max[2];
		verdict.base = units(base);
		verdict.bottom = units(bottom);
		verdict.top = units(top);
		// What the column under the record meets from a unit over its lowest point: another entity's solid,
		// else the terrain.
		int32_t support = terrain;
		const Placed::Entry *on = nullptr;
		int32_t hit = 0;
		stood_on(verdict, nullptr);
		if (placed.probe(x, y, io::bam_add(std::max(bottom, base), kProbeUp), terrain, entity.row, hit, on) &&
				hit > support) {
			support = hit;
			stood_on(verdict, on);
		}
		verdict.support = units(support);
		// Where the fix stands it: its anchor on what lies under it, as the original editor's placer sets an
		// item down (the height alone), where that leaves its model on it; else its lowest point on it.
		const auto stand_on = [&](int32_t under) {
			const int32_t by_anchor = io::bam_sub(under, fixed(anchor));
			const int32_t lift = io::bam_sub(by_anchor, z);
			const bool fits = io::bam_add(bottom, lift) <= io::bam_add(under, slack) && io::bam_add(top, lift) > under;
			verdict.by_anchor = fits;
			verdict.fix_z = units(fits ? by_anchor : io::bam_sub(z, io::bam_sub(bottom, under)));
		};
		// Wholly under the terrain, touching nothing (a tunnel's contents stand in its pieces): the game draws
		// it there, out of sight.
		if (top < terrain) {
			if (const Placed::Entry *touching = placed.touched(box_min, box_max, entity.row)) {
				verdict.why = "touches";
				stood_on(verdict, touching);
				out.push_back(std::move(verdict));
				continue;
			}
			stood_on(verdict, nullptr);
			verdict.support = units(terrain);
			verdict.state = MissionGroundState::Buried;
			stand_on(terrain);
			out.push_back(std::move(verdict));
			continue;
		}
		if (bottom <= io::bam_add(support, slack)) {
			out.push_back(std::move(verdict));
			continue;
		}
		// Over the water plane, its surface is what stands under it (the start's settle takes the higher of
		// the two [orig: Game_StartMission @0x525FE0..0x525FED]): on it within the slack, or in it.
		if (water && water_z > support) {
			if (bottom <= io::bam_add(water_z, slack)) {
				verdict.on = MissionSupport::Water;
				verdict.on_row = 0;
				verdict.on_name.clear();
				verdict.support = units(water_z);
				out.push_back(std::move(verdict));
				continue;
			}
			support = water_z;
			verdict.on = MissionSupport::Water;
			verdict.on_row = 0;
			verdict.on_name.clear();
			verdict.support = units(water_z);
		}
		// Its model reaches nothing under it: unless it leans on, hangs from or stands in another entity's
		// volume (a sign on a wall, a wire between poles, a crate inside a building), it floats.
		if (const Placed::Entry *touching = placed.touched(box_min, box_max, entity.row)) {
			verdict.why = "touches";
			stood_on(verdict, touching);
			verdict.support = units(support);
			out.push_back(std::move(verdict));
			continue;
		}
		verdict.state = MissionGroundState::Floats;
		stand_on(support);
		out.push_back(std::move(verdict));
	}
	return out;
}

// --- the words -------------------------------------------------------------------------------------

namespace {

std::string under_words(const MissionGroundVerdict &verdict) {
	switch (verdict.on) {
	case MissionSupport::Terrain: return "the terrain";
	case MissionSupport::Water: return "the water";
	case MissionSupport::Record: return verdict.on_name.empty() ? std::string("the entity under it") : verdict.on_name;
	default: return "the ground";
	}
}

} // namespace

std::string mission_ground_message(const MissionGroundVerdict &verdict, const std::string &title) {
	switch (verdict.state) {
	case MissionGroundState::Floats:
		return title + " floats " + metres(verdict.bottom - verdict.support) + " above " + under_words(verdict) +
				", touching nothing: the game draws an item or a building where its record stands it, and nothing "
				"sets it down.";
	case MissionGroundState::Buried:
		return title + " is buried: the top of its model stands " + metres(verdict.support - verdict.top) +
				" under the terrain, touching nothing, and the game draws it where its record stands it, out of "
				"sight.";
	case MissionGroundState::Falls:
		return title + " starts " + metres(verdict.base - verdict.support) + " above " + under_words(verdict) +
				": the game sets a person down only where his feet stand under a metre over the terrain, so he "
				"falls there at the mission's start.";
	case MissionGroundState::Hangs:
		return title + " starts " + metres(verdict.base - verdict.support) + " above " + under_words(verdict) +
				": the game sets a person down only where his feet stand under a metre over the terrain, and his "
				"class (" + verdict.why.substr(verdict.why.find(':') + 1) + ") has no motor to move him, so he stays "
				"in the air.";
	default: return std::string();
	}
}

void mission_ground_fix_words(const MissionGroundVerdict &verdict, std::string &label, std::string &detail) {
	const std::string z = height_words(verdict.fix_z);
	const std::string under = under_words(verdict);
	label = verdict.on == MissionSupport::Record ? "Set it on " + under
			: verdict.on == MissionSupport::Water ? std::string("Set it on the water")
												  : std::string("Set it on the terrain");
	if (verdict.rule == MissionGroundRule::Person)
		detail = "Sets its z to " + z + ", its feet on " + under + " under them, where the game's spawn stands a person.";
	else if (verdict.by_anchor)
		detail = "Sets its z to " + z + ", its ground anchor on " + under +
				" under it, as the original editor's placer sets an item down (the height alone).";
	else
		detail = "Sets its z to " + z + ", the lowest point of its model on " + under +
				" under it (its ground anchor stands too high in it to set it down by).";
}

} // namespace opennova::editor
