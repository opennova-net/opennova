#include <editor/preview/mission_shots.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <unordered_map>

#include <base/io/bam.h>
#include <base/io/fixed.h>
#include <base/io/strutil.h>
#include <base/resource_index/resource_index.h>
#include <formats/def/def.h>
#include <formats/mission/bms.h>
#include <formats/mission/mission.h>
#include <runtime/assets/asset_store.h>
#include <runtime/mission/collision_resolve.h>
#include <runtime/mission/item_traits.h>
#include <runtime/mission/promote.h>
#include <runtime/world/ammo_table_build.h>
#include <runtime/world/collision.h>
#include <runtime/world/destruction.h>
#include <runtime/world/entity_pose.h>
#include <runtime/world/fire_sound.h>
#include <runtime/world/impact_scar.h>
#include <runtime/world/occlusion.h>
#include <runtime/world/present_drains.h>
#include <runtime/world/present_passes.h>
#include <runtime/world/round_sim.h>
#include <runtime/world/world.h>

namespace opennova::editor {

namespace {

using io::JsonValue;
using io::json_number;
using io::json_string;

// The events the envelope carries: the last of a long run.
constexpr size_t kEventsShown = 64;

JsonValue vec3(const double v[3]) {
	JsonValue out = JsonValue::make_array();
	for (int i = 0; i < 3; ++i) out.push(json_number(v[i]));
	return out;
}

JsonValue vec3(const world::Vec3 &v) {
	JsonValue out = JsonValue::make_array();
	out.push(json_number(v.x));
	out.push(json_number(v.y));
	out.push(json_number(v.z));
	return out;
}

bool read_vec3(const JsonValue *json, double out[3]) {
	if (!json || !json->is_array() || json->array.size() != 3) return false;
	for (int i = 0; i < 3; ++i) {
		if (!json->array[size_t(i)].is_number() || !std::isfinite(json->array[size_t(i)].number)) return false;
		out[i] = json->array[size_t(i)].number;
	}
	return true;
}

const char *tag_name(int tag) {
	return tag >= 0 && tag < world::kImpactEffectTagCount ? world::kImpactEffectTagNames[tag] : "";
}

std::string metres(double value) {
	char text[32];
	std::snprintf(text, sizeof(text), "%.2f m", value);
	return text;
}

std::string at_words(const double at[3]) {
	char text[96];
	std::snprintf(text, sizeof(text), "(%.2f, %.2f, %.2f)", at[0], at[1], at[2]);
	return text;
}

int32_t to_bam(double radians) {
	return int32_t(uint32_t(int64_t(std::llround(radians / io::kRadiansPerBam))));
}

// The pool a promoted entity's kind is placed in, as the scene names it.
bool pool_of(world::EntityKind kind, MissionPool &out) {
	switch (kind) {
	case world::EntityKind::Item: out = MissionPool::Item; return true;
	case world::EntityKind::Building: out = MissionPool::Building; return true;
	case world::EntityKind::Marker: out = MissionPool::Marker; return true;
	case world::EntityKind::Organic: out = MissionPool::Organic; return true;
	}
	return false;
}

// The point's distance from the segment a to b.
double segment_distance(const double p[3], const double a[3], const double b[3]) {
	double ab[3], ap[3];
	double len = 0.0, dot = 0.0;
	for (int i = 0; i < 3; ++i) {
		ab[i] = b[i] - a[i];
		ap[i] = p[i] - a[i];
		len += ab[i] * ab[i];
		dot += ab[i] * ap[i];
	}
	const double t = len > 0.0 ? std::clamp(dot / len, 0.0, 1.0) : 0.0;
	double d = 0.0;
	for (int i = 0; i < 3; ++i) {
		const double e = ap[i] - ab[i] * t;
		d += e * e;
	}
	return std::sqrt(d);
}

// Where a shot's round leaves and its unit direction: kMissionShotStandoff short of the point along the eye's line
// (the eye itself where it stands nearer).
void shot_line(const MissionShot &shot, double from[3], double direction[3]) {
	double length = 0.0;
	for (int i = 0; i < 3; ++i) {
		direction[i] = shot.at[i] - shot.eye[i];
		length += direction[i] * direction[i];
	}
	length = std::sqrt(length);
	if (length <= 1e-6) {
		direction[0] = direction[1] = 0.0;
		direction[2] = -1.0;
		length = 0.0;
	} else {
		for (int i = 0; i < 3; ++i) direction[i] /= length;
	}
	const double back = std::min(kMissionShotStandoff, length > 0.0 ? length : kMissionShotStandoff);
	for (int i = 0; i < 3; ++i) from[i] = shot.at[i] - direction[i] * back;
}

} // namespace

bool MissionShot::operator==(const MissionShot &other) const {
	return tick == other.tick && strutil::iequals(ammo, other.ammo) && at[0] == other.at[0] && at[1] == other.at[1] &&
	       at[2] == other.at[2] && eye[0] == other.eye[0] && eye[1] == other.eye[1] && eye[2] == other.eye[2];
}

io::JsonValue mission_shot_to_json(const MissionShot &shot) {
	JsonValue out = JsonValue::make_object();
	out.set("tick", json_number(shot.tick));
	out.set("ammo", json_string(shot.ammo));
	out.set("at", vec3(shot.at));
	out.set("eye", vec3(shot.eye));
	return out;
}

bool read_mission_shot(const io::JsonValue &json, MissionShot &out, std::string &error) {
	MissionShot shot;
	const JsonValue *tick = json.is_object() ? json.get("tick") : nullptr;
	const JsonValue *ammo = json.is_object() ? json.get("ammo") : nullptr;
	int64_t at_tick = 0;
	if (!json.is_object() || (tick && !io::json_whole_in(*tick, 0, kMissionShotLastTick, at_tick)) || !ammo ||
	    !ammo->is_string() || ammo->string.empty() || !read_vec3(json.get("at"), shot.at) ||
	    !read_vec3(json.get("eye"), shot.eye)) {
		error = "A shot is {tick (the clock's, 0 up), ammo (an ammo.def record), at [x, y, z] (the point it is fired at, "
		        "mission metres), eye [x, y, z] (where it is seen from: the line the round flies along)}.";
		return false;
	}
	shot.tick = int32_t(at_tick);
	shot.ammo = ammo->string;
	out = shot;
	return true;
}

const char *mission_shot_event_token(MissionShotEvent::Kind kind) {
	using Kind = MissionShotEvent::Kind;
	switch (kind) {
	case Kind::Fired: return "fired";
	case Kind::Refused: return "refused";
	case Kind::Impact: return "impact";
	case Kind::Sound: return "sound";
	case Kind::Damage: return "damage";
	case Kind::Death: return "death";
	case Kind::Leg: return "leg";
	}
	return "impact";
}

// The world a run fires in, as the game's load makes it, and what the run keeps of it.
struct MissionShots::Run {
	std::shared_ptr<StampedFiles> stamped;
	ResourceIndex index;
	std::unique_ptr<assets::AssetStore> store;
	def::DefItemsFile items{};
	bool items_read = false;
	world::World world;
	world::CollisionWorld collision;
	world::OcclusionWorld occlusion;
	world::EntityPoseProvider pose;
	mission::CollisionResolveState state;
	const terrain::TerrainHeightField *terrain = nullptr;
	float water_height = 0.0f;
	// Each promoted entity's record (by its packed handle): its row, its item, its hit points as last seen.
	struct Held {
		NodeId row = 0;
		int64_t item = 0;
		int32_t health = 0;
		bool dead = false;
	};
	std::unordered_map<uint16_t, Held> held;
	// A death's legs not yet due.
	std::vector<MissionShotEvent> pending;

	~Run() {
		if (items_read) def::def_free_items(&items);
	}
};

MissionShots::MissionShots() = default;
MissionShots::~MissionShots() = default;

std::string MissionShots::title_of_(NodeId row) const {
	const auto found = setup_.titles.find(row);
	return found != setup_.titles.end() ? found->second : std::string();
}

bool MissionShots::files_moved() const {
	return setup_.files && !reads_.empty() && reads_.moved(*setup_.files);
}

void MissionShots::configure(MissionShotsSetup setup) {
	const bool moved = setup.files != setup_.files || setup.key != built_key_ ||
	                   (setup.files && !reads_.empty() && reads_.moved(*setup.files));
	setup_ = std::move(setup);
	if (moved) {
		dirty_ = true;
		if (tick_ > 0 || run_) reset_();
	}
}

void MissionShots::set_shots(std::vector<MissionShot> shots) {
	std::stable_sort(shots.begin(), shots.end(), [](const MissionShot &a, const MissionShot &b) { return a.tick < b.tick; });
	if (shots == shots_) return;
	shots_ = std::move(shots);
	base_ = shots_.empty() ? 0 : shots_.front().tick;
	// Another shot may bring other entities into the world: the run starts again over what it holds.
	dirty_ = true;
	reset_();
}

void MissionShots::clear() {
	shots_.clear();
	setup_ = MissionShotsSetup();
	built_key_ = 0;
	run_.reset();
	reads_.clear();
	events_.clear();
	spawns_.clear();
	why_.clear();
	tick_ = 0;
	base_ = 0;
	dirty_ = true;
	++serial_;
}

void MissionShots::reset_() {
	++serial_;
	++runs_;
	tick_ = 0;
	events_.clear();
	spawns_.clear();
	run_.reset();
	entities_loaded_ = 0;
	entities_collidable_ = 0;
}

bool MissionShots::build_(Run &run) {
	why_.clear();
	if (!setup_.files) {
		why_ = "No project is open.";
		return false;
	}
	if (!setup_.mission || !setup_.ground) {
		why_ = "No mission is shown.";
		return false;
	}
	run.stamped = std::make_shared<StampedFiles>(setup_.files);
	run.index.mount_source(run.stamped);
	run.store = std::make_unique<assets::AssetStore>(&run.index);
	world::World &world = run.world;
	// ammo.def and items.def as the mission load reads them [orig: AmmoDef_LoadAll @ 0x40B0B0; the item list's load].
	std::vector<uint8_t> bytes;
	def::DefAmmoFile ammo{};
	if (!run.stamped->read("ammo.def", bytes) || bytes.empty() ||
	    def::def_parse_ammo_memory(bytes.data(), bytes.size(), &ammo) != 0) {
		why_ = "The project has no ammo.def the game reads: no round flies.";
		reads_ = run.stamped->stamps();
		return false;
	}
	world.tables.ammo = world::build_ammo_table(ammo);
	def::def_free_ammo(&ammo);
	bytes.clear();
	if (run.stamped->read("items.def", bytes) && !bytes.empty() &&
	    def::def_parse_items_memory(bytes.data(), bytes.size(), &run.items) == 0)
		run.items_read = true;
	// The mission as it stands, the entities near no shot's line loaded empty in their place (their record's
	// index kept, so every other record lands where the game's load puts it).
	bms::File mission = *setup_.mission;
	std::vector<std::vector<double>> lines; // from (3), to (3) per shot
	for (const MissionShot &shot : shots_) {
		double from[3], direction[3];
		shot_line(shot, from, direction);
		std::vector<double> line(6);
		for (int i = 0; i < 3; ++i) {
			line[size_t(i)] = from[i];
			line[size_t(3 + i)] = shot.at[i] + direction[i] * kMissionShotReach;
		}
		lines.push_back(std::move(line));
	}
	const auto near_a_shot = [&](const MissionEntityMark &mark) {
		const auto found = setup_.bounds.find(mark.item);
		const float bound = found != setup_.bounds.end() ? found->second : 0.0f;
		const double reach = bound > 0.0f ? double(bound) + kMissionShotStandoff : kMissionShotReach;
		const double at[3] = {mark.x, mark.y, mark.z};
		for (const std::vector<double> &line : lines)
			if (segment_distance(at, line.data(), line.data() + 3) <= reach) return true;
		return false;
	};
	std::map<std::pair<int, int>, const MissionEntityMark *> marks;
	for (const MissionEntityMark &mark : setup_.entities) marks[{int(mark.pool), mark.index}] = &mark;
	const auto thin = [&](std::vector<bms::Entity> &records, MissionPool pool) {
		for (size_t i = 0; i < records.size(); ++i) {
			const auto found = marks.find({int(pool), int(i)});
			if (found == marks.end() || !near_a_shot(*found->second)) records[i].type_id = -1;
		}
	};
	thin(mission.items, MissionPool::Item);
	thin(mission.buildings, MissionPool::Building);
	thin(mission.markers, MissionPool::Marker);
	thin(mission.organics, MissionPool::Organic);
	// Promoted as the game's load spawns a mission's records [orig: Mission_LoadBMSFile @ 0x40F4E0], their items'
	// traits [orig: Entity_InitFromItemDef @ 0x49E550] and collision models (BVOL/BPLN, CFAC) attached.
	mission::PromoteOptions options;
	if (run.items_read) {
		const def::DefItemsFile *items = &run.items;
		options.item_attributes = [items](int32_t type_id) -> uint32_t {
			const def::DefItemDef *def = mission::find_item_def(*items, int(type_id) + int(mission::kItemIdOffset));
			return def ? def->attrib : 0u;
		};
	}
	mission::promote_mission(mission, world, options);
	world.ai.is_authority = true;
	if (run.items_read) mission::resolve_item_traits(world, run.items, [](int32_t) -> uint8_t { return 0; });
	run.terrain = setup_.ground->height_field();
	run.water_height = setup_.ground->water() ? float(setup_.ground->water_height()) : 0.0f;
	world.tables.surface_map = setup_.ground->surface_sampler();
	world.env.water_z = setup_.ground->water() ? int32_t(std::lround(setup_.ground->water_height() * io::kFp16OneD)) : 0;
	run.pose.set_assets(run.store.get());
	run.collision.terrain = run.terrain;
	run.collision.set_pose_provider(&run.pose);
	world.collision = &run.collision;
	world.pose_provider = &run.pose;
	world.ai.collision = &run.collision;
	if (run.items_read) {
		const mission::CollisionResolveDeps deps{run.collision, run.occlusion, run.pose, *run.store};
		entities_collidable_ = mission::resolve_collision_instances(world, run.items, run.state, deps);
	}
	run.collision.build_initial_tables(world);
	// Each entity loaded of the mission's, by its record (the promote's back-reference: its pool and index).
	world.registry.for_each([&](const world::Entity &entity) {
		if (entity.item_id < 0) return;
		MissionPool pool = MissionPool::Item;
		if (!pool_of(entity.kind, pool)) return;
		const auto found = marks.find({int(pool), int(world::spawn_origin_index(entity.spawn_origin))});
		if (found == marks.end()) return;
		Run::Held held;
		held.row = found->second->row;
		held.item = found->second->item;
		held.health = entity.health;
		run.held[entity.handle.packed] = held;
		++entities_loaded_;
	});
	reads_ = run.stamped->stamps();
	return true;
}

void MissionShots::run_to(int32_t tick) {
	// The run's own tick: the clock's from the first shot.
	tick = std::clamp(tick - base_, 0, kMissionShotsMostTicks);
	if (tick < tick_ || dirty_) {
		reset_();
		dirty_ = false;
		built_key_ = setup_.key;
		if (!shots_.empty()) {
			run_ = std::make_unique<Run>();
			if (!build_(*run_)) run_.reset();
		}
	}
	if (!run_) {
		if (tick_ != tick) ++serial_;
		tick_ = tick;
		return;
	}
	const int32_t from = tick_;
	while (tick_ < tick) step_();
	if (tick_ != from) ++serial_;
}

void MissionShots::step_() {
	Run &run = *run_;
	world::World &world = run.world;
	// The clock's tick (each event's, each spawn's), the run's world counting from the first shot.
	const int32_t tick = base_ + tick_;
	using Kind = MissionShotEvent::Kind;
	// The frame's head: the pending sounds count down [orig: Game_ProcessMainFrame @ 0x5263F0 -> Sound_TickPendingSlots
	// @ 0x529310].
	world.logic_tick = uint32_t(tick_);
	world.out.fire_sounds.tick();
	// The scars' rings as they stand, and the trail's cursor: what this tick writes is told apart.
	std::vector<uint32_t> cursors;
	cursors.push_back(world.out.scars.world_ring().cursor);
	for (const world::ScarRing &ring : world.out.scars.entity_rings()) cursors.push_back(ring.in_use ? ring.cursor : 0u);
	const int trail_before = world.round_sim.debug_trail_next;
	// The entity update: the rounds in flight [orig: Entity_UpdateAllEntities -> Weapon_UpdateAllProjectiles @ 0x4EC020],
	// then the explosion queue [orig: Projectile_ProcessExplosionQueue @ 0x4ead80].
	world.round_sim.tick(world, run.terrain, &run.collision);
	world::DestructionEvents destruction;
	world.explosions.process(world, &run.collision, run.terrain, run.water_height, destruction);
	world.logic_tick = uint32_t(tick_ + 1);
	// What the rounds struck, in the trail's order, matched to the impacts by their place.
	std::vector<world::RoundDebugEvent> struck;
	for (int i = trail_before; i != world.round_sim.debug_trail_next; i = (i + 1) % world::RoundSim::kDebugTrailCap)
		struck.push_back(world.round_sim.debug_trail[size_t(i)]);
	// The scars this tick wrote, by owner.
	struct Written {
		uint16_t owner = 0xFFFF;
		uint8_t texture = 0;
		int32_t radius = 0;
	};
	std::vector<Written> scars;
	const auto collect = [&](const world::ScarRing &ring, uint32_t before) {
		for (uint32_t c = before; c != ring.cursor; c = (c + 1) % uint32_t(world::kScarsPerEntity)) {
			const world::ScarSlot &slot = ring.slots[c];
			if (slot.live) scars.push_back({slot.owner.packed, slot.texture, slot.radius_q16});
		}
	};
	collect(world.out.scars.world_ring(), cursors[0]);
	for (size_t i = 0; i < world.out.scars.entity_rings().size(); ++i) {
		const world::ScarRing &ring = world.out.scars.entity_rings()[i];
		if (ring.in_use) collect(ring, i + 1 < cursors.size() ? cursors[i + 1] : 0u);
	}
	// The impacts the flight resolved, each its ammo's row as the presenter picks it.
	const std::vector<world::RoundImpact> raw = world.round_sim.impacts;
	std::vector<uint16_t> struck_now;
	std::vector<world::RoundImpactPresentation> presented;
	world::drain_round_impact_rows(world, presented);
	for (const world::RoundImpact &impact : raw) {
		MissionShotEvent event;
		event.tick = tick;
		event.kind = Kind::Impact;
		event.at[0] = impact.position.x;
		event.at[1] = impact.position.y;
		event.at[2] = impact.position.z;
		event.direction = impact.direction;
		event.pick = world::round_impact_row(world.tables.ammo, impact);
		if (const world::AmmoTableEntry *ammo = world.tables.ammo.by_index(impact.ammo_index)) event.ammo = ammo->name;
		for (const world::RoundImpactPresentation &row : presented)
			if (row.source_order == impact.source_order) {
				event.effect = row.effect;
				event.set = row.sound;
			}
		const world::RoundDebugEvent *hit = nullptr;
		for (const world::RoundDebugEvent &each : struck)
			if (each.hit.x == impact.position.x && each.hit.y == impact.position.y && each.hit.z == impact.position.z)
				hit = &each;
		const world::Entity *entity = hit && hit->entity != 0xFFFF ? world.registry.get(world::EntityHandle{hit->entity})
		                                                          : nullptr;
		if (hit && hit->kind == world::RoundDebugEvent::kOrganic) {
			event.on = "person";
		} else if (hit && (hit->kind == world::RoundDebugEvent::kItemFace || hit->kind == world::RoundDebugEvent::kItemSphere)) {
			event.on = "object";
			event.surface = hit->kind == world::RoundDebugEvent::kItemFace ? int(hit->material) : -1;
		} else {
			// The terrain's class there in the game's words (DI-07): the char map, a tile's square, the ocean; the
			// water handler's row where the plane was crossed first.
			event.ground = setup_.ground->terrain_at(event.at[0], event.at[1], event.at[2]);
			event.surface = event.ground.surface;
			event.on = impact.effect_tag == world::kWaterImpactEffectTag && event.ground.surface + 4 != impact.effect_tag
			                   ? "water"
			                   : "terrain";
		}
		if (entity) {
			const auto found = run.held.find(entity->handle.packed);
			if (found != run.held.end()) {
				event.row = found->second.row;
				event.item = found->second.item;
				event.record = title_of_(event.row);
			}
			for (const Written &scar : scars)
				if (scar.owner == entity->handle.packed) {
					event.scar = world::scar_texture_strip_name(scar.texture);
					event.scar_radius = float(scar.radius) / 65536.0f;
				}
		}
		const std::string row = tag_name(event.pick.tag);
		event.words = "The round stops on " +
		              (event.on == "object" ? (event.record.empty() ? std::string("an object") : event.record) +
		                                              (event.surface >= 0 ? "'s face of material " + std::to_string(event.surface)
		                                                                  : std::string("'s bound"))
		               : event.on == "person"  ? (event.record.empty() ? std::string("a person") : event.record)
		               : event.on == "water"   ? std::string("the water")
		                                       : "the terrain (" + mission_surface_words(event.surface) + ", surface " +
		                                                std::to_string(event.surface) + ")") +
		              ": the " + row + " row (" + (event.effect.empty() ? std::string("no effect") : event.effect) + ", " +
		              (event.set.empty() ? std::string("no sound") : event.set) + ")" +
		              (event.pick.from == world::ImpactRowFrom::NullBank
		                       ? std::string(", ammo def 0's bank at place ") + std::to_string(event.pick.tag) +
		                                 (event.pick.bank_tag ? std::string(" (its ") + tag_name(event.pick.bank_tag) + " row)"
		                                                      : std::string(" (no row)"))
		                       : std::string()) +
		              (event.scar.empty() ? std::string() : ", a scar " + event.scar + " " + metres(event.scar_radius) + " radius") +
		              ".";
		if (!event.effect.empty())
			spawns_.push_back({event.effect, {event.at[0], event.at[1], event.at[2]}, event.direction, tick, "impact"});
		if (entity && event.row) struck_now.push_back(entity->handle.packed);
		events_.push_back(std::move(event));
	}
	// A struck object whose hit points stand: what its record and the round say of it (the damage pass's own gates
	// decided; these are their facts in words).
	for (const uint16_t packed : struck_now) {
		const world::Entity *entity = world.registry.get(world::EntityHandle{packed});
		const auto found = run.held.find(packed);
		if (!entity || found == run.held.end() || entity->health != found->second.health) continue;
		MissionShotEvent none;
		none.tick = tick;
		none.kind = Kind::Damage;
		none.row = found->second.row;
		none.item = found->second.item;
		none.record = title_of_(none.row);
		none.at[0] = entity->position.x;
		none.at[1] = entity->position.y;
		none.at[2] = entity->position.z;
		none.health_before = none.health_after = entity->health;
		none.health_max = entity->health_max;
		none.indestructible = (entity->engine_flags & world::kEntityFlagIndestructible) != 0;
		std::string why;
		if (!entity->has_item_def) why = "no item of the catalog draws it";
		else if (none.indestructible) why = "it is indestructible (no hit points, or its record's Indestructible attribute)";
		else if (entity->armor_impact == -1) why = "its items.def armor is -1: rounds never hurt it";
		else why = "its items.def armor " + std::to_string(entity->armor_impact) + " against the round's penetration";
		none.words = (none.record.empty() ? std::string("The object") : none.record) + " takes no damage: " + why + ".";
		events_.push_back(std::move(none));
	}
	// The damage the rounds and the blasts dealt: each held entity's hit points as they stand.
	for (auto &[packed, held] : run.held) {
		const world::Entity *entity = world.registry.get(world::EntityHandle{packed});
		if (!entity || entity->health == held.health) continue;
		MissionShotEvent damage;
		damage.tick = tick;
		damage.kind = Kind::Damage;
		damage.row = held.row;
		damage.item = held.item;
		damage.record = title_of_(held.row);
		damage.at[0] = entity->position.x;
		damage.at[1] = entity->position.y;
		damage.at[2] = entity->position.z;
		damage.health_before = held.health;
		damage.health_after = entity->health;
		damage.health_max = entity->health_max;
		damage.indestructible = (entity->engine_flags & world::kEntityFlagIndestructible) != 0;
		damage.words = (damage.record.empty() ? std::string("An entity") : damage.record) + " takes " +
		               std::to_string(held.health - entity->health) + " damage: " + std::to_string(entity->health) + " of " +
		               std::to_string(entity->health_max) + " hit points left.";
		held.health = entity->health;
		const bool dead = entity->health <= 0 || ((entity->flags | entity->engine_flags) & world::kEntityFlagDead) != 0;
		events_.push_back(damage);
		if (!dead || held.dead) continue;
		held.dead = true;
		// Its death as DI-10 plans it: the item's class, its husk, the legs in order from this tick.
		MissionShotEvent death = damage;
		death.kind = Kind::Death;
		const def::DefItemDef *def =
				run.items_read ? mission::find_item_def(run.items, int(held.item)) : nullptr;
		if (!def) {
			death.words = damage.record + " is destroyed.";
			events_.push_back(std::move(death));
			continue;
		}
		const DamageItem item = damage_item_of(*def, "items.def");
		DamageModels models;
		const std::string husk = world::husk_render_graphic(item.husk, item.huskfinal);
		const std::string pieces = world::death_piece_graphic(item.husk, item.huskfinal);
		if (!husk.empty())
			if (const assets::Model model = run.store->model(husk)) note_damage_husk(*model, models);
		if (!pieces.empty())
			if (const assets::Model model = run.store->model(pieces)) note_damage_pieces(*model, models);
		const DamagePlan plan = damage_plan(item, models);
		death.words = (damage.record.empty() ? item.name : damage.record) + " is destroyed: " + plan.class_words +
		              (plan.swaps && !plan.husk.empty() ? " (" + plan.husk + " swapped in " +
		                                                          std::to_string(plan.swap_tick) + " ticks on)"
		                                                : std::string()) +
		              ".";
		events_.push_back(death);
		for (const DamageLeg &leg : plan.legs) {
			MissionShotEvent due = death;
			due.kind = Kind::Leg;
			due.tick = tick + leg.tick;
			due.words = leg.words;
			due.effect = leg.kind == "effect" && leg.at_item ? leg.name : std::string();
			due.set = leg.kind == "sound" ? leg.name : std::string();
			due.at[2] += double(leg.above);
			run.pending.push_back(std::move(due));
		}
	}
	// The death legs now due.
	for (auto it = run.pending.begin(); it != run.pending.end();) {
		if (it->tick > tick) {
			++it;
			continue;
		}
		if (!it->effect.empty())
			spawns_.push_back({it->effect, {it->at[0], it->at[1], it->at[2]}, world::Vec3{}, it->tick, "death"});
		if (!it->set.empty()) {
			MissionShotEvent sound = *it;
			sound.kind = Kind::Sound;
			sound.words = it->set + " (the death) at " + at_words(it->at) + ".";
			events_.push_back(*it);
			events_.push_back(std::move(sound));
		} else {
			events_.push_back(*it);
		}
		it = run.pending.erase(it);
	}
	// The shots of this tick: each round spawned short of its point along the eye's line, as the game spawns a round
	// [orig: RoundData_SpawnRound @ 0x4EC0D0], no shooter and no launch (the tool's own choice).
	int number = 0;
	for (const MissionShot &shot : shots_) {
		++number;
		if (shot.tick != tick) continue;
		MissionShotEvent fired;
		fired.tick = tick;
		fired.shot = number;
		fired.ammo = shot.ammo;
		const int ammo = world.tables.ammo.index_of(shot.ammo.c_str());
		double from[3], direction[3];
		shot_line(shot, from, direction);
		for (int i = 0; i < 3; ++i) fired.at[i] = from[i];
		if (ammo < 0) {
			fired.kind = Kind::Refused;
			fired.words = shot.ammo + " is not in ammo.def: the game spawns no round of it.";
			events_.push_back(std::move(fired));
			continue;
		}
		world::RoundSpawnParams params;
		params.origin = world::Vec3{float(from[0]), float(from[1]), float(from[2])};
		params.dir_yaw_bam = to_bam(std::atan2(direction[1], direction[0]));
		params.dir_pitch_bam = to_bam(std::atan2(direction[2], std::hypot(direction[0], direction[1])));
		params.ammo_index = ammo;
		params.launch_presented = true;
		world.out.fire_sounds.set_listener(params.origin);
		world.round_sim.spawn(world, params);
		fired.kind = Kind::Fired;
		fired.direction = world::Vec3{float(direction[0]), float(direction[1]), float(direction[2])};
		fired.words = "Shot " + std::to_string(number) + ": " + shot.ammo + " fired at " + at_words(shot.at) + " from " +
		              metres(kMissionShotStandoff) + " short of it.";
		events_.push_back(std::move(fired));
	}
	// The sounds the queue readied, each at its place.
	for (const world::ReadyFireSound &ready : world.out.fire_sounds.drain()) {
		MissionShotEvent sound;
		sound.tick = tick;
		sound.kind = Kind::Sound;
		sound.set = ready.set_name;
		sound.at[0] = ready.pos.x;
		sound.at[1] = ready.pos.y;
		sound.at[2] = ready.pos.z;
		sound.words = ready.set_name + " at " + at_words(sound.at) + ".";
		events_.push_back(std::move(sound));
	}
	// The rest the game hands its presenter is not this tool's (the launch, the death's own legs the class runs).
	world.round_sim.fired.clear();
	++tick_;
}

renderer::ScarDrawList MissionShots::scars() const {
	renderer::ScarDrawList out;
	if (!run_) return out;
	const Run &run = *run_;
	renderer::ScarViewContext context;
	context.fog_distance = 1.0e6f;
	struct Lookup {
		const world::World *world;
		const world::CollisionWorld *collision;
	} lookup{&run.world, &run.collision};
	context.user = &lookup;
	context.section_matrix = [](uint16_t owner, int section, world::CollisionMatrix &matrix, void *user) {
		const auto *self = static_cast<const Lookup *>(user);
		return self->collision->entity_section_matrix(*self->world, world::EntityHandle{owner}, section, matrix);
	};
	renderer::compile_scar_draws(run.world.out.scars, context, out);
	return out;
}

int MissionShots::scar_count() const {
	if (!run_) return 0;
	int count = 0;
	for (const world::ScarSlot &slot : run_->world.out.scars.world_ring().slots) count += slot.live ? 1 : 0;
	for (const world::ScarRing &ring : run_->world.out.scars.entity_rings())
		if (ring.in_use)
			for (const world::ScarSlot &slot : ring.slots) count += slot.live ? 1 : 0;
	return count;
}

io::JsonValue MissionShots::to_json() const {
	JsonValue out = JsonValue::make_object();
	JsonValue shots = JsonValue::make_array();
	for (const MissionShot &shot : shots_) shots.push(mission_shot_to_json(shot));
	out.set("shots", std::move(shots));
	out.set("tick", json_number(base_ + tick_));
	out.set("why", json_string(why_));
	out.set("entities_loaded", json_number(entities_loaded_));
	out.set("entities_collidable", json_number(entities_collidable_));
	out.set("scars", json_number(scar_count()));
	JsonValue events = JsonValue::make_array();
	const size_t first = events_.size() > kEventsShown ? events_.size() - kEventsShown : 0;
	for (size_t i = first; i < events_.size(); ++i) {
		const MissionShotEvent &event = events_[i];
		JsonValue row = JsonValue::make_object();
		row.set("tick", json_number(event.tick));
		row.set("kind", json_string(mission_shot_event_token(event.kind)));
		row.set("shot", json_number(event.shot));
		row.set("at", vec3(event.at));
		if (event.kind == MissionShotEvent::Kind::Impact) {
			row.set("on", json_string(event.on));
			row.set("surface", json_number(event.surface));
			if (event.surface >= 0) row.set("surface_words", json_string(mission_surface_words(event.surface)));
			if (event.on == "terrain" || event.on == "water") {
				row.set("from", json_string(mission_surface_from_token(event.ground.from)));
				if (event.ground.tile >= 0) row.set("tile", json_number(event.ground.tile));
			}
			row.set("tag", json_number(event.pick.tag));
			row.set("row", json_string(tag_name(event.pick.tag)));
			row.set("row_from", json_string(event.pick.from == world::ImpactRowFrom::Own ? "own" : "bank"));
			if (event.pick.from == world::ImpactRowFrom::NullBank) row.set("bank_tag", json_number(event.pick.bank_tag));
			row.set("direction", vec3(event.direction));
			row.set("scar", json_string(event.scar));
			row.set("scar_radius", json_number(event.scar_radius));
		}
		if (!event.ammo.empty()) row.set("ammo", json_string(event.ammo));
		if (!event.effect.empty()) row.set("effect", json_string(event.effect));
		if (!event.set.empty()) row.set("set", json_string(event.set));
		if (event.row) {
			row.set("record_row", json_number(double(event.row)));
			row.set("record", json_string(event.record));
			row.set("item", json_number(double(event.item)));
		}
		if (event.kind == MissionShotEvent::Kind::Damage || event.kind == MissionShotEvent::Kind::Death) {
			row.set("health_before", json_number(event.health_before));
			row.set("health_after", json_number(event.health_after));
			row.set("health_max", json_number(event.health_max));
			row.set("indestructible", JsonValue::make_bool(event.indestructible));
		}
		row.set("words", json_string(event.words));
		events.push(std::move(row));
	}
	out.set("events", std::move(events));
	out.set("event_count", json_number(double(events_.size())));
	return out;
}

} // namespace opennova::editor
