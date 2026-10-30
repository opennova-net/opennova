#pragma once

// The pool's spawners and their entity lifecycle: an entity's LGHT records
// become pool instances once, its one cached EffectWorld word (entity+0x1B4)
// is what the muzzle glow re-arms and what the entity's destruction clears,
// the in-flight round glows follow their rounds, and a containing building's
// BMS id resolves to the owner id its draws declare. The pool and the spawn
// records' witnesses are light_scene.h.

#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include <formats/threedi/threedi_3di3.h>
#include <runtime/renderer/light_scene.h>
#include <runtime/world/present_drains.h>

namespace opennova::renderer {

// The one light-owner id space every spawn and every draw context agrees on,
// or owner gating never matches. Zero stays retail's unowned/world sentinel; a
// decoded wire handle (16-bit, zero valid) and a batched static source index
// each take a tagged nonzero range, disjoint from the embedder's own ids for
// entities that have neither (a preview's models), which stay below 2^48.
struct LightOwnerIds {
	static constexpr uint64_t kWireTag = uint64_t(1) << 48;
	static constexpr uint64_t kStaticTag = uint64_t(2) << 48;
	static constexpr uint64_t kWireHandleMask = 0xFFFF;
	// 0 for a negative input.
	static uint64_t wire(int64_t wire_handle);
	static uint64_t static_source(int64_t source_index);
};

// The entity matrix an entity's LGHT records transform by, once at spawn: the
// presentation-frame basis rows (row-major 3x3) then the origin
// (mission::StaticSourceTransform's layout). A record's model-space offset
// reads model (x, y, z) as presentation (-x, y, z) before the basis.
using LightPlacement = std::array<float, 12>;

// One entity's Entity_SpawnGlowEffects inputs [orig: Entity_SpawnGlowEffects
// @0x56c7c0]: its owner id, its LGHT records, the entity matrix, whether its
// ItemDef type is Building (the blink query is skipped outright then
// [orig: the ItemType_Building gate @0x56c7ec]), and slot 0 of the one blink
// query at the entity's position, its building resolved to an owner id
// (owner 0: no hit, or a building with no owner).
struct EntityLightSpawn {
	uint64_t owner_entity = 0;
	const threedi::ThreediLight *records = nullptr;
	size_t record_count = 0;
	LightPlacement placement{};
	bool building = false;
	uint64_t blink_owner = 0;
	int32_t blink_section = 0;
};

// One LGHT record's pool instance [orig: Entity_SpawnGlowEffects @0x56c7c0]:
// the record's point through the entity matrix once [orig: @0x56c82d..0x56c85d]
// (no node, ROBJ or bone follow exists), radius = atten_end * 65536
// [orig: @0x56c876], a white packed colour with the record's RGB-gen block
// attached (style 0 = none), the owner the record's attach facts decide
// (resolve_model_light_owner, branch order @0x56c89a..0x56c8db), and the three
// authored disables folded to render flags 512/1024/2048 [orig: @0x56c8e7..0x56c91d].
LightSpawnParams model_light_spawn(const threedi::ThreediLight &record,
		const LightPlacement &placement, uint64_t owner_entity, bool building,
		uint64_t blink_owner, int32_t blink_section);

// A mission-space float point as the pool's 16.16 position.
std::array<int32_t, 3> light_position_fixed(const std::array<float, 3> &mission);
// The entity matrix's origin (the entity's position) as the pool's 16.16
// position: an entity's light query centre.
std::array<int32_t, 3> light_placement_origin_fixed(const LightPlacement &placement);

class LightSpawner {
public:
	explicit LightSpawner(LightScene &scene) : scene_(scene) {}

	// An entity's records, walked once: an entity already walked spawns
	// nothing again (duplicate delivery never turns a later free slot into an
	// invented second spawn), and one with no records is not walked. Every
	// spawn overwrites the entity's one cached word, so it keeps the FINAL
	// record's lease [orig: Entity_SpawnGlowEffects @0x56c925..0x56c92c];
	// earlier model lights stay in the pool until the mission resets. Returns
	// the cached word (null when the pool was full).
	LightHandle spawn_entity(const EntityLightSpawn &spawn);
	bool walked(uint64_t owner_entity) const;
	// The entity's destruction clears exactly the lease its word holds, once
	// [orig: Entity_Destroy @0x43e903..0x43e916]: a husk swap is no
	// destruction and touches no authored light.
	void destroy_entity(uint64_t owner_entity);
	// One weapon fire with the ammo MF_Light flag (LightScene::fire_muzzle_glow
	// over the shooter's cached word, which model LGHT shares
	// [orig: Entity_UpdateMuzzleGlowEffect @0x56c965..0x56c9d5]).
	void muzzle_fire(uint64_t shooter, const std::array<int32_t, 3> &muzzle_fixed);
	// The in-flight light_move glows diffed against the live rows: a new
	// round spawns its glow radius/2 above it (mode 1, terrain disabled), a
	// known one follows at its raw position, a gone one despawns
	// [orig: RoundData_SpawnRound @0x4ec8da; the per-tick follow @0x4eaa9f;
	// Projectile_ReleaseEffects]. Row positions are mission space.
	void sync_round_glows(const std::vector<world::RoundGlowRow> &rows);

	// The interior-owner registry: the owner id a building's BMS id resolves to
	// is the one its own draws declare. A static source registers its tagged id;
	// a live entity its own, until it is destroyed. Every building registers
	// before any record walks: a light can spawn inside a building that appears
	// later in the walk, and retail still binds it to that building's owner
	// group, its spawn walk running over the whole entity pools at mission
	// start [orig: Game_StartMission @ 0x525d19 -> Game_SpawnAllEntityGlowEffects
	// @0x5227b0].
	void register_building(int bms_id, uint64_t owner_entity);
	uint64_t owner_for_bms(int bms_id) const;

	// Mission teardown: the pool and every record of it.
	void reset();

	size_t live_round_glows() const { return round_handles_.size(); }
	LightHandle entity_handle(uint64_t owner_entity) const;

private:
	LightScene &scene_;
	// Entity owner id -> its one cached word (retail's entity+0x1B4).
	std::unordered_map<uint64_t, LightHandle> entity_handles_;
	// The entities whose records were walked.
	std::unordered_map<uint64_t, bool> walked_;
	// Building BMS id -> owner id; and each live owner's BMS ids, dropped at
	// its destruction.
	std::unordered_map<int, uint64_t> bms_owner_;
	std::unordered_map<uint64_t, int> owner_bms_;
	// Round presentation id -> pool light.
	std::unordered_map<uint64_t, LightHandle> round_handles_;
};

} // namespace opennova::renderer
