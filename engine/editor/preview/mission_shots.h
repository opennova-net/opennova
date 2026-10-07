#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <base/io/json.h>
#include <base/vfs/file_source.h>
#include <editor/model/node.h>
#include <editor/preview/mission_ground_facts.h>
#include <editor/preview/mission_scene.h>
#include <editor/preview/model_damage.h>
#include <editor/preview/viewport_follow.h>
#include <runtime/renderer/scar_draw_list.h>
#include <runtime/world/ammo_table.h>
#include <runtime/world/geom.h>

namespace opennova::bms {
struct File;
}

namespace opennova::editor {

// The mission view's Shoot tool (ADR 0046 DI-23; CONTEXT.md "Shoot tool"): a click on the terrain or an object fires
// the picked ammo there, and the impact plays as the game plays it. Nothing here decides an impact: the shots run in
// a world::World of the tool's own, built from the mission as it stands by the game's own load (the mission promoted
// [orig: Mission_LoadBMSFile @ 0x40F4E0 -> Entity_SpawnFromBMSRecord @ 0x40E9F0] (mission::promote_mission), its
// items' traits (mission::resolve_item_traits) and their collision models (mission::resolve_collision_instances)),
// over the mission's terrain, char map, placed tiles and water as the game reads them (DI-07's MissionGround and
// DI-29's sampler) and ammo.def as the load builds it. Each tick runs the game's legs in its frame's order: the
// pending sounds' countdown [orig: Sound_TickPendingSlots @ 0x529310], the rounds in flight (world::RoundSim::tick:
// the terrain stop and its class + 4, an object's bullet face and its material + 4, the water handler, the person
// pass; the impact row through the presenter's pick, its sound through the distance gate, its scar on the struck
// entity's ring; the damage [orig: Weapon_UpdateAllProjectiles @ 0x4EC020]), the explosion queue [orig:
// Projectile_ProcessExplosionQueue @ 0x4ead80]. An item the damage destroys plays its death as DI-10 plans it
// (preview/model_damage: its class's legs in order from the tick it died).
//
// The editor's choices beside the game's: the round leaves kMissionShotStandoff metres short of the point the click
// met, along the camera's line (a round from the camera itself would drop and drift over a few hundred metres and
// miss what was clicked), with no shooter and no launch (only the impact is the tool's); the world holds the entities
// near the shots' lines alone (each within its item's bound, kMissionShotReach past it where none is known), every
// other record of the file loaded empty in its place; nothing ticks but the rounds and the explosions (no brain
// moves, no person is posed: a person is struck through the torso sphere the round pass falls back to without its
// rig); the listener stands where the shot leaves.

inline constexpr double kMissionShotStandoff = 2.0;
inline constexpr double kMissionShotReach = 20.0;
// How long a run is followed from its first shot: ten minutes of the game's ticks (the shots stand on the preview
// clock's ticks, the run's world counting from the first).
inline constexpr int32_t kMissionShotsMostTicks = 62 * 600;
inline constexpr int32_t kMissionShotLastTick = 0x3FFFFFFF;

// A shot of the tool: the clock's tick it fires on, the ammo (an ammo.def record), the point the click met (mission
// metres, x east, y north, z up) and the camera's eye it was seen from (the line the round flies along).
struct MissionShot {
	int32_t tick = 0;
	std::string ammo;
	double at[3] = {0.0, 0.0, 0.0};
	double eye[3] = {0.0, 0.0, 0.0};
	bool operator==(const MissionShot &other) const;
	bool operator!=(const MissionShot &other) const { return !(*this == other); }
};
// On the wire: {tick, ammo, at, eye}.
io::JsonValue mission_shot_to_json(const MissionShot &shot);
bool read_mission_shot(const io::JsonValue &json, MissionShot &out, std::string &error);

// One thing a run did, on the clock's tick it did it.
struct MissionShotEvent {
	enum class Kind : uint8_t {
		Fired,   // a round left (its shot's line)
		Refused, // a shot the game spawns no round for (an ammo the table lacks)
		Impact,  // a round stopped: the row it played there, the scar it left
		Sound,   // a sound the fire-sound queue readied, at its place
		Damage,  // an entity took damage: its hit points before and after
		Death,   // an entity's damage destroyed it: its death as DI-10 plans it
		Leg,     // a leg of a death, on its tick
	};
	int32_t tick = 0;
	Kind kind = Kind::Impact;
	int shot = 0; // the shot's number (from 1)
	double at[3] = {0.0, 0.0, 0.0};
	world::Vec3 direction; // an impact's spawn orientation (zero: none, the descriptor's +Y)
	std::string on;        // an impact's: "terrain", "water", "object", "person"
	int surface = -1;      // the class it struck: the terrain's (the sampler's), an object's face material
	MissionGroundFacts ground; // a terrain impact's ground, DI-07's words
	world::ImpactRowPick pick; // the row played (an impact's)
	std::string ammo;
	std::string effect;
	std::string set;
	NodeId row = 0;     // the struck entity's row (0: none)
	std::string record; // and its title
	int64_t item = 0;
	int health_before = 0, health_after = 0, health_max = 0;
	bool indestructible = false;
	std::string scar; // the scar's texture ("" none)
	float scar_radius = 0.0f;
	std::string words;
};
const char *mission_shot_event_token(MissionShotEvent::Kind kind);

// An effect a run spawned: its name, where (mission metres) and its orientation, its tick.
struct MissionShotSpawn {
	std::string effect;
	double at[3] = {0.0, 0.0, 0.0};
	world::Vec3 direction;
	int32_t tick = 0;
	std::string source; // "impact", "death"
};

// What the shots are fired in: the project's files (the open documents standing in), a key that moves when the
// mission or its ground moved (the run starts again), the mission as it stands (its file composed), its ground
// (DI-07's: the terrain, the char map, the tiles, the water; held by the caller, read again only where the key moves),
// its entities (the scene's: each record's pool, index, row and item), each item's bound (metres) and each row's
// title.
struct MissionShotsSetup {
	std::shared_ptr<const FileSource> files;
	uint64_t key = 0;
	std::shared_ptr<const bms::File> mission;
	const MissionGround *ground = nullptr;
	std::vector<MissionEntityMark> entities;
	std::unordered_map<int64_t, float> bounds;
	std::unordered_map<NodeId, std::string> titles;
};

class MissionShots {
public:
	MissionShots();
	~MissionShots();
	MissionShots(const MissionShots &) = delete;
	MissionShots &operator=(const MissionShots &) = delete;

	// What the run is fired in: a run that reached anything runs again where the key or the files moved.
	void configure(MissionShotsSetup setup);
	// The key it was last configured with, and whether a file its world read moved since.
	uint64_t key() const { return setup_.key; }
	bool files_moved() const;
	// The shots, in tick order: the run again from tick 0 where it reached a tick at or past the first that changed.
	void set_shots(std::vector<MissionShot> shots);
	const std::vector<MissionShot> &shots() const { return shots_; }
	// The run to the clock's tick (bounded by kMissionShotsMostTicks): stepped on, or run again from tick 0.
	void run_to(int32_t tick);
	void clear();

	int32_t tick() const { return base_ + tick_; }
	uint64_t serial() const { return serial_; }
	uint64_t runs() const { return runs_; }
	// Why the run does nothing ("" it runs): no project, no ammo.def, no mission.
	const std::string &why() const { return why_; }
	// The files its world was read from (ammo.def, items.def, the models), each with its stamp.
	const FileStamps &reads() const { return reads_; }
	const std::vector<MissionShotEvent> &events() const { return events_; }
	const std::vector<MissionShotSpawn> &spawns() const { return spawns_; }
	// The scars as the game's ring cache holds them, every ring compiled into the mission's frame (the entity rings
	// through their owners' section matrices), and how many there are.
	renderer::ScarDrawList scars() const;
	int scar_count() const;
	// How many entities the world holds of the mission's, and how many have a collision model.
	int entities_loaded() const { return entities_loaded_; }
	int entities_collidable() const { return entities_collidable_; }

	// The envelope's `shots`: the shots, the run's tick, the last events, the scars' count.
	io::JsonValue to_json() const;

private:
	struct Run;
	void reset_();
	bool build_(Run &run);
	std::string title_of_(NodeId row) const;
	void step_();

	MissionShotsSetup setup_;
	uint64_t built_key_ = 0;
	std::vector<MissionShot> shots_;
	std::unique_ptr<Run> run_;
	std::string why_;
	FileStamps reads_;
	int32_t tick_ = 0; // the run's own, from the first shot's tick
	int32_t base_ = 0; // the first shot's tick on the clock
	std::vector<MissionShotEvent> events_;
	std::vector<MissionShotSpawn> spawns_;
	uint64_t serial_ = 0;
	uint64_t runs_ = 0;
	bool dirty_ = true;
	int entities_loaded_ = 0;
	int entities_collidable_ = 0;
};

} // namespace opennova::editor
