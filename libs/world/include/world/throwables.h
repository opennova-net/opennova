// Throwables: thrown-grenade / satchel / claymore round motors, the placed-device
// entities they convert into, and the device think/detonate chain — the witnessed
// path from "fire released" to "the claymore cuts loose".
// Witness record: docs/world/world-wac-ai-re.md §27 (engine-research 2026-07-20,
// retail Jointops.exe kong IDB).
//
// [orig anchors: the items.def class tag tables — event callbacks (`ai_function`)
// @ 0x813000 rows nade/schl/clym/vmne/lndm @ 0x813138..0x8131A8, per-frame motors
// (`move_function`) @ 0x82abc8; the motors Entity_UpdateGrenadePhysics @ 0x443F50,
// Entity_UpdateSatchelPhysics @ 0x4482A0, Entity_UpdateClaymorePhysics @ 0x4472F0;
// the thinks Entity_SatchelThink @ 0x443670, Entity_ClaymoreThink @ 0x4438C0,
// Entity_AVMineThink @ 0x443BB0, Entity_LandmineThink @ 0x441A40; the cone tests
// Entity_FindEnemyInCone @ 0x43cba0 / Entity_FindEnemyVehicleInCone @ 0x43c9f0;
// round->entity conversion Entity_ConvertRoundToPlacedEntity @ 0x5455B0 +
// Entity_CloneFromTemplateByType @ 0x4398a0 (pool by items.def type) + S2C 0x59;
// the detonator Entity_DetonateSatchelsByOwner @ 0x546ed0 (Health = -1); owner
// cleanup Entity_RemovePlacedDevicesByOwner @ 0x546e00 via
// Server_RemoveEntityAndNotify @ 0x50a270 (S2C 0x12); the per-tick think cadence
// in Entity_UpdatePool1Slot @ 0x4b8dd0 (age -1/tick, think at <= 0); the
// PowerThrow charge chain g_fireChargeStartTick @ 0xB76800 ->
// WeaponSlot_RequestFire @ 0x53efa0 -> RoundData_SpawnRound charge scale
// @ 0x4ec5bb; interned ammo pairs WeaponDef_ResolveAllReferences @ 0x540270 ->
// g_ammo_satchel/..boom/claymore/..killzone/..shrapnel/AV_Mine/..killzone
// @ 0x24E7DC0..0x24E7DD8.]
//
// The host presents; this module simulates and RECORDS (the RoundSim/Destruction
// events precedent) — libs/world stays render-free.
#ifndef OPENNOVA_WORLD_THROWABLES_H
#define OPENNOVA_WORLD_THROWABLES_H

#include <cstdint>
#include <string>
#include <vector>

#include "world/entity.h"
#include "world/geom.h"

namespace opennova::terrain {
struct TerrainHeightField;
}

namespace opennova::world {

class World;
class CollisionWorld;
class RoundSim;
struct FixedVec3;
struct LiveRound;
struct AmmoTableEntry;

// ----------------------------------------------------------------------------
// items.def class tags. Retail scans two 8-byte-name tables with the item's
// `ai_function` (event/think callback [orig: g_EntityClassEventCallbackTable
// @ 0x813000]) and `move_function` (per-frame motor [orig: physics table
// @ 0x82abc8]) tags. JO data authors: flashbang/frag "nade/nade", satchel
// "schl/schl", claymore "clym/clym", AT mine "vmne/schl" (mine think, satchel
// flight), landmine items "lndm".
// ----------------------------------------------------------------------------
enum class ThrowClass : uint8_t {
    kNone = 0,
    kNade,     // thrown grenade: arc + bounce + fuse [orig motor @ 0x443F50]
    kSatchel,  // thrown charge: stick + convert to placed [orig @ 0x4482A0]
    kClaymore, // placed upright: stick + convert to placed [orig @ 0x4472F0]
    kAVMine,   // think only — no motor row in retail [orig fn1 @ 0x443BB0]
    kLandmine, // mission minefield item think [orig fn1 @ 0x441A40; unported]
};

ThrowClass throw_class_from_tag(const char *tag);

// Select the TrcrID item for a viewer. Retail uses the foe item only when the
// teams differ AND a foe id is authored; otherwise it falls back to friendly
// [orig: RoundData_SpawnRound @ 0x4ec787..0x4ec79d].
int32_t throwable_item_for_viewer(int32_t friendly_item, int32_t enemy_item,
                                  uint8_t item_team, uint8_t viewer_team);

// Whether a satchel/claymore impact face is upward-facing enough to accept a
// stick [orig: Entity_UpdateSatchelPhysics @ 0x448858 / claymore @ 0x447802].
bool throwable_surface_accepts_stick(const FixedVec3 &normal_q16);

// Host-fed binding rows: item type id (items.def id - 100000, the TrcrID space)
// -> {think class (ai_function), motor class (move_function)}.
struct ThrowableClassRow {
    int32_t item_id = 0;
    ThrowClass think = ThrowClass::kNone;
    ThrowClass motor = ThrowClass::kNone;
    // items.def hp/armor for the placed entity [orig: Entity_InitFromItemDef
    // @ 0x49e550 Health = def healthMax, Armor = def armorMax].
    int32_t health_max = 0;
    int32_t armor_impact = 0;
    int32_t armor_kz = 0;
};

struct ThrowableClassTable {
    std::vector<ThrowableClassRow> rows;

    const ThrowableClassRow *get(int32_t item_id) const {
        for (const auto &r : rows)
            if (r.item_id == item_id) return &r;
        return nullptr;
    }
    void set(const ThrowableClassRow &row) {
        for (auto &r : rows)
            if (r.item_id == row.item_id) {
                r = row;
                return;
            }
        rows.push_back(row);
    }
    void clear() { rows.clear(); }
};

// ----------------------------------------------------------------------------
// One placed device — the world-side record beside the registry entity the
// resting round converted into [orig: the pool-1 clone; the round's first 0x2B4
// bytes carry over via Entity_ConvertRoundToPlacedEntity @ 0x5455B0].
// ----------------------------------------------------------------------------
struct PlacedDevice {
    bool active = false;
    EntityHandle entity;          // the registry entity (shootable; health lives there)
    uint64_t entity_spawn_id = 0; // host lifetime identity for the reusable handle
    EntityHandle owner;           // [orig: +368] kill credit / detonator / cleanup key
    uint64_t owner_spawn_id = 0;
    uint16_t owner_handle = 0xFFFF;
    uint16_t hit_word = 0;        // [orig: +442] rides the detonation descriptor
    int32_t ammo_index = -1;      // [orig: +620] the PLACED ammo (satchel/claymore/AV_Mine)
    int32_t item_friendly = 0;    // [orig: ammo +16] the frndlyTrcrID/base item
    int32_t item_enemy = 0;       // [orig: ammo +20] foe variant; 0 -> friendly fallback
    uint8_t team = 0xFF;          // [orig: +354]
    ThrowClass think = ThrowClass::kNone;
    Vec3 pos;                     // mission units
    int32_t yaw_bam = 0;          // [orig: +16/+20/+24] facing (claymore cone axis)
    int32_t pitch_bam = 0;
    int32_t roll_bam = 0;
    EntityHandle parent;          // stuck-to entity [orig: +40 groundEntity]
    uint64_t parent_spawn_id = 0;
    Vec3 parent_offset;           // device pos in the parent's yaw frame at stick time
    int32_t parent_yaw_at_stick = 0;
    int32_t device_yaw_at_stick = 0;
    // Remaining ticks until the think starts — the ARM DELAY. [orig: the clone
    // keeps the round's un-aged +684 (noage skipped aging) = ammo max_age, and
    // Entity_UpdatePool1Slot decrements 1/tick, thinking every tick at <= 0.]
    int32_t think_delay_ticks = 0;
};

// ----------------------------------------------------------------------------
// Host-presentation events (drained per tick by the throwable present pass).
// ----------------------------------------------------------------------------
struct ThrowableEvents {
    struct DeviceSpawn {
        uint16_t entity = 0xFFFF;  // packed registry handle
        int32_t item_friendly = 0; // the viewer picks by team [orig: S2C 0x59
        int32_t item_enemy = 0;    //  carries both TrcrID words]
        uint8_t team = 0xFF;
        Vec3 pos;
        int32_t yaw_bam = 0;
        int32_t pitch_bam = 0;
        int32_t roll_bam = 0;
    };
    struct DeviceRemove {
        uint16_t entity = 0xFFFF;
    };
    std::vector<DeviceSpawn> spawns;
    std::vector<DeviceRemove> removes;

    void clear() {
        spawns.clear();
        removes.clear();
    }
};

// ----------------------------------------------------------------------------
// ThrowableSim — placed devices + the think/detonate chain. Motors for the
// FLYING phase live as free functions called from RoundSim::tick (the rounds
// own that state); the rest conversion lands here.
// ----------------------------------------------------------------------------
class ThrowableSim {
public:
    static constexpr int kCapacity = 64;

    std::vector<PlacedDevice> devices;
    ThrowableEvents events;

    // Host-fed class bindings for the TrcrID items (ai_function/move_function).
    ThrowableClassTable classes;

    // MP host rule [orig: dword_24D1E34 & 0x8000, "TeamTriggerClaymore" admin
    // set @ 0x405f16]: set -> same-team actors also trip claymores/mines.
    bool team_trigger_claymore = false;

    // The witnessed PRNG shapes, world-local streams (the destruction-port
    // precedent: stream identity with retail is not reproducible, the
    // generator is). [orig: PRNG_Next16 @ 0x6130a0 stream dword_31BFBB0 for
    // the bounce spin kicks; the rol4(s + rol11(s)) ^ 1 inline stream
    // dword_31BFBB8 for the shrapnel fan.]
    uint32_t prng16_state = 0x2B0749C1u;
    uint32_t fan_prng_state = 0x2B0749C1u;
    int32_t prng16();
    uint16_t fan_prng();

    // One 62 Hz think pass over the placed devices [orig: Entity_UpdatePool1Slot
    // @ 0x4b8dd0 — arm-delay countdown, then think every tick].
    void tick(World &world, CollisionWorld *collision,
              const terrain::TerrainHeightField *terrain);

    // Rest-conversion: the satchel/claymore round becomes a placed device + a
    // registry entity [orig: the authority rest leg of the schl/clym motors ->
    // Entity_CloneFromTemplateByType @ 0x4398a0 + Entity_ConvertRoundToPlacedEntity
    // @ 0x5455B0 + S2C 0x59]. Returns false when the pool is full (the round
    // then just expires).
    bool place_from_round(World &world, const LiveRound &round,
                          const AmmoTableEntry &ammo);

    // The DETONATOR [orig: RoundData_SpawnRound ammo flag 0x20 Detonatesatchels
    // -> Entity_DetonateSatchelsByOwner @ 0x546ed0]: every placed SATCHEL-ammo
    // device owned by `owner` gets Health = -1; the next think detonates it.
    void detonate_satchels_by_owner(World &world, EntityHandle owner);

    // Owner death/leave cleanup [orig: Server_ProcessPlayerDeath @ 0x5178d8 ->
    // Entity_RemovePlacedDevicesByOwner @ 0x546e00 -> Server_RemoveEntityAndNotify
    // @ 0x50a270]: devices are REMOVED silently, never detonated.
    void remove_devices_by_owner(World &world, EntityHandle owner);

    void reset() noexcept {
        devices.clear();
        events.clear();
        prng16_state = 0x2B0749C1u;
        fan_prng_state = 0x2B0749C1u;
    }

private:
    void detonate_device(World &world, PlacedDevice &device,
                         const char *boom_ammo_name);
    void remove_device(World &world, PlacedDevice &device);
    bool enemy_in_cone(World &world, CollisionWorld *collision,
                       const terrain::TerrainHeightField *terrain,
                       const PlacedDevice &device, float max_range_units,
                       int32_t cone_half_bam, bool vehicles);
};

// ----------------------------------------------------------------------------
// The PowerThrow charge [orig: press gate @ 0x4e08fd (weapon def Flags sign bit
// 0x80000000), release @ 0x4e07e9: held < 31 ticks -> 255 (full), else
// clamp((held - 31) / 93, 0.1, 1.0) * 255; charge byte 0/255 = unscaled full
// speed, 1..254 scale ammo velocity by charge/256 @ 0x4ec5bb].
// ----------------------------------------------------------------------------
uint8_t power_throw_charge_from_hold(int32_t held_ticks);

// ----------------------------------------------------------------------------
// Round-motor entry: one 62 Hz step for a useownmove round bound to a motor
// class. Called from RoundSim::tick in place of the stock ballistic path
// [orig: the flags & 0x2000 leg of Projectile_UpdatePhysics @ 0x4e9f06 calls
// the round's +452 class motor]. Returns false when the round converted or
// died and the slot must release.
// ----------------------------------------------------------------------------
bool throwable_motor_tick(World &world, RoundSim &sim, LiveRound &round,
                          const AmmoTableEntry &ammo, CollisionWorld *collision,
                          const terrain::TerrainHeightField *terrain,
                          bool allow_consequences = true);

} // namespace opennova::world

#endif // OPENNOVA_WORLD_THROWABLES_H
