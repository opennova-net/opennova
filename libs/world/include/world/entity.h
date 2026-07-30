// Addressable entity model for the OpenNova runtime world.
//
// Faithful to the original engine's entity addressing (EntityPool_FindByNetId
// @0x4f0a20, Jointops.exe): entities live in fixed-capacity pools; each carries a
// 16-bit net id (the "SSN" WAC/BMS scripts reference). A live entity is named by a
// packed handle (pool<<12 | slot) — 4-bit pool index, 12-bit slot.
#ifndef OPENNOVA_WORLD_ENTITY_H
#define OPENNOVA_WORLD_ENTITY_H

#include <cstdint>
#include <string>
#include <vector>
#include <world/weapon_fsm.h>

#include "world/geom.h"

namespace opennova::world {

// Retail stores several gameplay values in signed 16-bit fields even though our public
// entity model intentionally keeps int32_t carriers for API/network compatibility. Narrow
// explicitly at the storage/mutation seams instead of relying on implementation-defined
// int32_t -> int16_t conversion. The uint64_t mask also makes negative and oversized inputs
// wrap exactly modulo 2^16 before sign extension.
constexpr int32_t retail_signed_i16(int64_t value) noexcept {
    const uint32_t low = static_cast<uint32_t>(static_cast<uint64_t>(value) & 0xFFFFu);
    return low < 0x8000u ? static_cast<int32_t>(low)
                         : static_cast<int32_t>(low) - 0x10000;
}

// Mirrors mission::EntityKind / bms::ItemType. Kept independent so libs/world
// has no dependency on libs/mission (promotion adapts between them).
enum class EntityKind : uint8_t {
    Marker = 0,
    Item = 1,
    Building = 2,
    Organic = 3,
};

// Live entity pools 0..4 [orig: the g_pool_list walk bound @0x431910].
inline constexpr int kEntityPoolCount = 5;

// Packed addressable handle: (pool_index << 12) | (slot_index & 0xFFF).
// [orig: return value of EntityPool_FindByNetId @0x4f0a20; 0xFFFF == not found.]
struct EntityHandle {
    uint16_t packed = kInvalid;
    static constexpr uint16_t kInvalid = 0xFFFF;

    constexpr int pool() const { return (packed >> 12) & 0xF; }
    constexpr int slot() const { return packed & 0xFFF; }
    constexpr bool valid() const { return packed != kInvalid; }

    static constexpr EntityHandle make(int pool, int slot) {
        EntityHandle h;
        h.packed = static_cast<uint16_t>(((pool & 0xF) << 12) | (slot & 0xFFF));
        return h;
    }

    bool operator==(const EntityHandle &o) const { return packed == o.packed; }
    bool operator!=(const EntityHandle &o) const { return packed != o.packed; }
};

// Spawn-origin provenance word: (kind << 24) | (record index & 0xFFFFFF);
// kSpawnOriginNone = none. GDScript twin: godot/engine/world/spawn_origin.gd.
inline constexpr uint32_t kSpawnOriginNone = 0xFFFFFFFFu;
constexpr uint32_t spawn_origin_pack(uint32_t kind, uint32_t index) {
    return (kind << 24) | (index & 0xFFFFFFu);
}
constexpr int spawn_origin_kind(uint32_t origin) { return static_cast<int>(origin >> 24); }
constexpr int32_t spawn_origin_index(uint32_t origin) {
    return static_cast<int32_t>(origin & 0xFFFFFFu);
}

// Seat class for vehicle/emplacement mounting. The enum values are the original
// seatType codes. [orig: Entity_FindBestSeatSlot @0x4351f0 classifies the seat
// bone name: "sitex"->1, "ctrlx"->2, "UseGun"->3, "drvrx"->5; the armory-point
// leg of the nearest scan reports 4 @0x436417.] An emplaced gun offers a single
// Gunner seat. ArmoryPoint is a scan/label result code, never a mountable seat.
enum class SeatType : uint8_t {
    None = 0,
    Passenger = 1,   // "sitex"
    Controller = 2,  // "ctrlx"
    Gunner = 3,      // "UseGun"
    ArmoryPoint = 4, // "armory*" userpoint on an Armory-attrib item (labels only)
    Driver = 5,      // "drvrx"
};

constexpr bool is_vehicle_control_seat(SeatType type) {
    return type == SeatType::Controller || type == SeatType::Driver;
}

// One seat a vehicle/emplacement offers. Mirrors the original split: the slot's
// seat-bone type lives at model[605+slot] and its occupant handle at
// vehicle[400+2*slot] (0xFFFF = empty). [orig: Entity_FindBestSeatSlot @0x4351f0 /
// Entity_RequestVehicleAttach @0x4364a0.]
struct Seat {
    SeatType type = SeatType::None;
    // Fixed retail mountHandles slot: passenger seats 0..7, control/driver 8,
    // UseGun 9. Runtime gameplay keeps seats densely packed, so this cannot be
    // inferred from the vector index. 0xFF means no retail wire slot.
    uint8_t retail_slot = 0xFF;
    uint8_t bone_index = 0;     // [orig: model[605+slot] seat-bone index]
    uint8_t pose_index = 0;     // `sitexNN`/`ctrlxNN`/`drvrxNN` -> anim_sit + NN
    std::string source_name;     // original seat/userpoint name (`sitex00`, `drvrx01`, `UseGun`)
    Vec3 seat_local;            // seat offset from the vehicle origin (mission space, Z-up)
    int16_t yaw_offset = 0;     // gunner facing offset vs the vehicle yaw [orig: @0x43656c]
    // An items.def addeweap* anchor is not a mount-facing convention. Retail
    // builds the child entity's complete orientation from this userpoint's
    // authored direction and live owning bone every update. The flag keeps
    // that frame distinct while reusing the mounted-pose provider for its
    // position/PANM walk.
    // [orig: Entity_UpdateTransformAndTurret @0x440CA0 -> attachment call
    // @0x44109D; build_bone_attachment_matrix @0x56C630]
    bool attachment_frame = false;
    EntityHandle occupant;      // [orig: vehicle[400+2*slot]] kInvalid = empty
};

// Post-death update callback installed by the retail unitType dispatch. Keeping
// it explicit prevents a husk flag or unitType alone from starting motion when
// Entity_SpawnDeathPieces rejected the death (no husk, already husked, or fully
// submerged).
enum class DeathMotionMode : uint8_t {
    None = 0,
    Generic = 1,
    Falling = 2,
    Static = 3,
    PiecePhysics = 4,
};

// Minimal live-entity state the scripting evaluators read and mutate. This is a
// clean model over the original 172-byte bms record + the pool record's net id;
// the renderer/AI's full entity layout is a separate, deferred concern.
// Named mirrors of the DEF_ITEM_ATTRIB_* bits world/netsim code reads off the
// entity's ItemDefAttrib dword (Entity::item_attrib, and the same dword on
// ai.h's def_attrib profile mirror). libs/world stays def-parser-free; parity
// static_asserts against def.h live in npruntime/src/weapon_table_build.cpp.
// [orig: ItemDef_ParseProperty @0x49eb00; docs/world/itemdef-re.md:147-155]
inline constexpr uint32_t kItemAttribEweap = 0x20u;
inline constexpr uint32_t kItemAttribLandable = 0x200u;
inline constexpr uint32_t kItemAttribAIData = 0x100000u; // §5.6 AI class — gates the 0x0D AI-trailer
inline constexpr uint32_t kItemAttribNoDie = 0x40000000u;

// The retail entity Flags dword bits (Entity::engine_flags + the organic
// low-byte legacy `flags` mirror; entity+36). ONE home for every bit with a
// witnessed meaning; the consolidated per-bit table is
// docs/world/world-wac-ai-re.md § "The entity Flags dword". Known-but-unnamed
// bits stay raw at use sites — do not name: 0x1, 0x80, 0x10000, 0x2000000,
// 0x8000000, and vehicle_motor's Flags-dword 0x8/0x20 writes (vehicle-context
// meanings unwitnessed).
inline constexpr uint32_t kEntityFlagDead = 0x2;              // [orig: kill writes Flags |= 6 @0x43fbf6]
inline constexpr uint32_t kEntityFlagHusk = 0x4;              // items/buildings: husk swap [orig: @0x43fbf6]
inline constexpr uint32_t kEntityFlagNVGWorn = 0x4;           // organics: NVG draw, same bit kind-dependent
                                                              // [orig: draw @0x4e3b54; refresh of bits 2-4 §13.1]
inline constexpr uint32_t kEntityFlagBinoculars = 0x8;        // [orig: draw @0x4e3c04; g_binocularsRaised refresh]
inline constexpr uint32_t kEntityFlagScopeRaised = 0x10;      // [orig: g_weaponScopeActive refresh; test @0x4b5deb]
inline constexpr uint32_t kEntityFlagParachute = 0x20;        // deployed chute (D-INF-20) [orig: radius leg @0x4b3aac]
inline constexpr uint32_t kEntityFlagMounted = 0x40;          // carried/mounted; the AI guard family reads it too
                                                              // [orig: @0x494752; guard @0x4bf5a5-family]
inline constexpr uint32_t kEntityFlagPlayer = 0x100;          // the wire Player class bit; gates held-weapon draws
                                                              // [orig: §5.10b class; draw gate @0x4e5073-family]
inline constexpr uint32_t kEntityFlagReflective = 0x400;      // BMS Reflective(1<<23) [orig: @0x40e9f0]
inline constexpr uint32_t kEntityFlagVehicleLoadoutZone = 0x800;  // type-11 volume touch
inline constexpr uint32_t kEntityFlagInAir = 0x2000;          // airborne/swimming [orig: grounded selector @0x4b78ab]
inline constexpr uint32_t kEntityFlagPriorityTarget = 0x4000; // set on every fire, decays per perception scan
                                                              // [orig: @0x4bf370 set; @0x4bbfa4 clear; §16.2 x6 scoring]
inline constexpr uint32_t kEntityFlagDrowning = 0x8000;       // zeroes vertical swim input [orig: §7 movement]
inline constexpr uint32_t kEntityFlagBuilding = 0x20000;      // [orig: Entity_InitFromModel @0x40e105]
inline constexpr uint32_t kEntityFlagLadderContact = 0x100000; // CL/type-4 touch; locks upper-body pose + skips
                                                               // gravity while aligned [orig: @0x4b3291]
inline constexpr uint32_t kEntityFlagArmoryZone = 0x400000;   // type-6 volume touch [orig: @0x4aea45]
inline constexpr uint32_t kEntityFlagIndoors = 0x800000;      // [orig: accum bit 2 -> Flags @0x4b39xx; render gates §4]
inline constexpr uint32_t kEntityFlagNoShadow = 0x1000000;    // BMS NoShadow(1<<24) [orig: @0x40e9f0]
inline constexpr uint32_t kEntityFlagIndestructible = 0x4000000; // BMS Indestructible(1<<21) or hp==0
                                                                 // [orig: @0x40e9f0; @0x40dc8e]

struct Entity {
    uint16_t net_id = 0;      // SSN; the field WAC/BMS address entities by
    int32_t bms_id = 0;       // file entity id (bms::Entity::id); the host keys placed nodes by this
                              // (MissionEntityRegistry), distinct from the runtime net_id/SSN.
    EntityHandle handle;      // self-handle (assigned at spawn)
    // Monotonic registry lifetime identity. Packed handles intentionally reuse
    // fixed pool slots; this host-only serial distinguishes two entities that
    // occupied the same slot, even when all authored/net fields are identical.
    uint64_t registry_spawn_id = 0;

    // The owning connection's ConnectionId/dcb (GamePlayerEntity entity+0x78). The joining client's
    // self-scan matches it against its own ConnectionId; a host/dedicated-server reserves dcb 0. This
    // is the runtime home of what the wire models as OrganicSpawnRecord::entity_flags (the 0x0C
    // entity+0x78 field). [orig: Server_PlayerAdd @0x51cbc0 writes entity+0x78 = conn->connection_id;
    // matched in Player_FindLocalPlayerEntity @0x4e0090; net-re §5.2b / D-NET-92/101]
    uint32_t owner_connection_id = 0;

    EntityKind kind = EntityKind::Item;
    int32_t item_id = 0;      // items.def type id
    bool has_item_def = false; // retail entity+0x20 ItemDef pointer is non-null
    uint8_t item_type = 0;    // raw ItemDef+0x5C type (1 vehicle, 3 person)
    bool is_ai_capable = false; // items.def ItemDefAttrib & 0x100000 (AIData / §5.6 AI class). Gates the
                                // 0x0D AI-trailer (D-NET-97). Distinct from ai_flags (BMS). [docs/world/itemdef-re.md]
    // The §5.10b wire replication class, resolved from the item's items.def *_function class
    // tag (ai_function, else move_function -> ItemDef+356 serialize callback) and stamped by
    // the host's post-promotion item-traits sweep. Stored as an OPAQUE code (the novaworld
    // EntityClass value; libs/world stays net-agnostic) — 0xFF = unresolved, netsim falls back
    // to its minimal heuristic. Load-bearing: a pool-1 item that is NOT a vehicle class (e.g.
    // ai_function ewep emplacements) must NOT be serialized with the vehicle compact record or
    // the client desyncs mid-frame (retail-join v13, 2026-07-02).
    uint8_t net_class_code = 0xFF;

    Vec3 position;            // mission space (Z-up)
    int16_t yaw = 0;
    int16_t pitch = 0;
    int16_t roll = 0;

    // Runtime entity flags — the GamePlayerEntity `Flags` at entity+36. Bit 1 is the
    // movement gate cleared at spawn and checked before the C2S 0x0C input uplink
    // [orig: Entity_ResetToSpawnState @0x4B9610 / Player_BuildTag0CInputBody @0x42A550;
    // docs/net/novaworld-net-re.md §5.2b/§5.6]. Distinct from ai_flags (BMS attributes).
    uint32_t flags = 0;
    // Spawn-point backup of position, written by world::entity_reset_to_spawn_state
    // [orig: Entity_ResetToSpawnState backs Position into pad9[124/128/132]].
    Vec3 spawn_position;

    uint8_t team = 0;
    // GamePlayerEntity.playerClass (entity+0x294) — the soldier class 5..9. The joiner's client
    // resolves its body-anim model from THIS at round-load [orig: Game_ReloadEntityModelsAndCallbacks
    // @0x522830 -> AnimMap_GetSlotPropertyInt(playerClass) @0x4127b0 -> ADM -> AnimMap_RegisterEntity
    // @0x40bb60 writes animChannelB(+0x188)]. The MP branch preloads classes 5..9 only; class 0 maps
    // to slot 15 -> empty ADM -> no anim channel -> Entity_UpdateInfantryPlayerBody @0x4b40e0 bails,
    // so the player cannot move/crouch/prone. Set from the player's loadout at spawn (default a valid
    // class for players); 0 = unset / non-player. [orig: re-grill 2026-06-28; net-re §5.2b/§5.23]
    uint8_t player_class = 0;
    uint8_t group_id = 0;     // named-group membership
    uint8_t waypoint_id = 0;  // wplist / route this entity follows
    int32_t wp_number = 0;    // position along that route

    uint8_t alert_state = 0;  // green/yellow/red
    int32_t ai_state = 0;     // AI component state
    int32_t ai_target = -1;   // net id of current AI target, -1 = none
    // Targeted-by refcount (entity+530): ++ when an AI acquires this entity, -- (clamp 0)
    // when it retargets/clears. The target scorer reads it as the anti-pile-on saturation
    // gate (<=16) and score decay. [orig: Entity_SetAITarget @0x45d760 maintains it;
    // AI_FindBestTargetB @0x466f60 consumes; world-wac-ai-re §16.3]
    int16_t ai_target_refcount = 0;
    // Per-shooter tracer cadence counter: ++ per fired round, wraps at the ammo
    // tracer_rate, tracer on wrap. Stands in for the original's per-WEAPON-SLOT byte
    // (weaponSlot+0x80) — one weapon per NPC today, so behavior is identical; the
    // counter surviving a player weapon swap is the tracked delta.
    // [orig: RoundData_SpawnRound @0x4ec199-0x4ec1bb]
    uint8_t tracer_shot_counter = 0;
    // Per-player replicated damage class, indexed by AmmoDef file index. C2S
    // loadout entry byte 4 writes it; 1 = x0.9, 2 = x1.1, other = x1.
    std::vector<uint8_t> ammo_damage_class;
    int32_t health = 100;     // signed i16 retail storage carried sign-extended; <=0 -> dead
    // items.def hp (itemDef+0x17C healthMax), stamped by the host's item-traits sweep
    // (0 = unresolved). The original spawns entities at Health = healthMax
    // [orig: Entity_InitFromItemDef @0x49e550]; the sweep mirrors that by lifting health
    // to hp for entities still at their spawn default. Feeds the §5.10 field-17 tier
    // denominator and the §5.13 vehicle health word.
    int32_t health_max = 0;   // signed i16 retail storage carried sign-extended
    // Raw items.def ItemDefAttrib dword (itemDef+84), stamped by the host trait
    // sweep. Combat keeps this value on the entity because damage targets are
    // not necessarily AI entities. In particular bit 0x40000000 is NoDie:
    // weapon damage may reduce health only as far as 1.
    // The kItemAttrib* constants below name the bits world/netsim code reads
    // (same dword on ai.h's def_attrib profile mirror).
    uint32_t item_attrib = 0;
    // Signed impact/KZ armor classes and vehicle occupant-reduction factors
    // from ItemDef +0x190/+0x192 and +0x188/+0x18C.
    int32_t armor_impact = 0; // signed i16 retail storage carried sign-extended
    int32_t armor_kz = 0;     // signed i16 retail storage carried sign-extended
    float damage_reduc_pp = 0.0f;
    float damage_reduc_max = 0.0f;
    // Effective uniform model scale in signed Q16.16. Zero is retail's sentinel
    // for the ordinary unscaled/rigid inverse. The host adapter resolves the
    // entity+0x158 override before the itemDef+0x1B8 fallback.
    int32_t uniform_scale_q16 = 0;
    bool alive = true;
    // Retail entity+0x124 damage-disabled/respawn state: 0 damageable, 620
    // join-pending countdown, -1 dead. Projectile damage gates on nonzero.
    int32_t damage_state = 0;
    // The pending death-anim selection (GamePlayerEntity +0x2C0 deathAnimStateId):
    // written at DAMAGE time by the kill (RoundSim bullet selection [orig:
    // Entity_HandleDamageTrigger @0x407483]), consumed once by the infantry death
    // edge into anim_state, then cleared [orig: @0x4b9cc9..0x4b9d38]. 0 = none ->
    // the edge falls back to 174 death_pungi.
    int32_t death_anim_state = 0;
    // The corpse timer (entity +0x148): seeded from the item's deathtime at the
    // death edge, decremented per dead tick; 0 -> despawn (SP holds while the local
    // player can see the corpse, 62-tick retries). [orig: @0x4b9c7f / @0x4b9e6a]
    int32_t corpse_timer = 0;
    // items.def 'deathtime' in ticks ((62*v or 496) + 62 at parse [orig:
    // ItemDef_ParseProperty @0x49fa96 -> def+0x890]), stamped by the item-traits
    // sweep. 0 = no token -> the corpse expires on the first dead tick (watch-check
    // permitting), matching the original's zero-init def field.
    int32_t deathtime_ticks = 0;
    // items.def attrib LeaveCorpse (0x400000): the corpse never despawns.
    // [orig: the @0x4b9e54 skip of the whole timer/despawn block]
    bool leave_corpse = false;
    uint32_t ai_flags = 0;    // BmsiAttributeFlags
    int32_t move_speed_kph = 0;
    int32_t engage_min = 0;
    int32_t engage_max = 0;
    int32_t attack_max = 0;
    // The body-anim CLIP the present pass plays (kBodyAnim*, body_anim.h), selected by the
    // infantry motor / AI brain each tick; -1 = no clip / hold rest. RENAMED from `anim_slot`:
    // this is presentation state, NOT the retail entity+0x374 `animSlot` below — echoing it
    // onto the wire was the D-NET-146 DBuggy-shadow bug.
    int32_t body_anim_slot = -1;
    // GamePlayerEntity.animSlot (entity+0x374) — the character-model/anim-set selector: the
    // BMS AnimSlot spawn property, or a player's per-side avatar (the joiner's VCA/VCB 0x42
    // join vars picked by ASSIGNED team, host default 1). Serialized raw as field 13 of the
    // 0x0C organic / 0x18 full-entity spawn records. [orig: Entity_SpawnFromAnimSlotProperty
    // @0x43c390 (+0x374 write @0x43c522); Server_PlayerAdd @0x51cbc0 (@0x51d0b1);
    // Server_InitAllPlayerEntitiesForRound @0x516aa0 (@0x516b8e); net-re §5.23 D-NET-146]
    uint8_t anim_slot = 0;
    // Players only: the wire NetId (entity+0x15C) = the minimap/character-slot id, picked per
    // assigned team from the joiner's CI0/CI1 join vars (low u16 of the atol). 0 = unassigned
    // (the encoder falls back to its D-NET-137 shim). Non-players serialize Entity::net_id
    // (the WAC SSN space) there instead. [orig: Server_PlayerAdd @0x51cbc0 slot+440 ->
    // entity+0x15C; NapiNetConfig_LoadFromConnTags @0x4c7260 jsp[56]/jsp[58]]
    uint16_t minimap_net_id = 0;
    // MoveOrder bit layout (the reconstructed word = net_move_input | net_stance_bits << 8;
    // [orig: Player_PackInputStateToEntity @0x4df68f-0x4df741; stance reads
    // Entity_UpdateInfantryPlayerBody @0x4b4165-0x4b4181]):
    static constexpr uint32_t kMoveOrderDirMask = 0x7;    // 8-way dir F=0..FR=7 (§5.38)
    static constexpr uint32_t kMoveOrderMoving = 0x8;
    static constexpr uint32_t kMoveOrderFreeLook = 0x10;  // [orig: steer-source pick @0x48b4a8]
    static constexpr uint32_t kMoveOrderLeanLeft = 0x40;  // [orig: lean ramp @0x4b7dbf]
    static constexpr uint32_t kMoveOrderLeanRight = 0x80; // [orig: lean ramp @0x4b7dd6]
    static constexpr uint32_t kMoveOrderProne = 0x100;    // stance bit 8 (see net_stance_bits below)
    static constexpr uint32_t kMoveOrderCrouch = 0x200;   // stance bit 9
    // The wire movement-INPUT byte (entity+0x12C low): the owning client uplinks it every frame
    // (§5.10 extended C2S 0x0C) and the host echoes it in that player's 0x0A compact record —
    // remote players are motor-driven from replicated input, NOT from an anim slot [orig: case-2
    // apply @0x4c11ec; consumers Entity_UpdatePlayerInfantryMovement @0x48496d,
    // check_bone_ground_contact @0x441ba4 (stance bits 8-9)]. Written by apply_player_intent for
    // remote peers; mirrored from the packed local input for the host's own player (bits 0-2 =
    // 8-way move_direction_index, bit 3 = moving [orig: Player_PackInputStateToEntity @0x4df68f]).
    uint8_t net_move_input = 0;
    // MoveOrder bits 8-9 (entity+0x12C >> 8): bit0 = prone (0x100), bit1 = crouch (0x200). The
    // stance the server-side body-anim selection consumes for THIS player [orig:
    // Entity_UpdateInfantryPlayerBody @0x4b4165-0x4b4181 reads MoveOrder&0x300]. A remote player's
    // stance arrives as the C2S 0x1D STANCE-CHANGE code (169 crouch / 170 prone / 172 stand)
    // [orig: NapiNPServerMsg_HandleStanceChange @0x501C60 rewrites MoveOrder bits 8-9]; vehicle
    // attach/detach clears it [orig: @0x435c54 / @0x43561e]. Echoed to the OWNING client in its
    // 0x0A header-tail state byte bits 0-1 (the client re-latches its own stance from that byte
    // EVERY frame [orig: NapiNPClientMsg_0x00A tail read @0x4303e5 -> latch @0x430562/@0x430570]
    // — a hardcoded 0 tail force-stands a crouched retail client, the pre-v32 crouch/prone bug).
    uint8_t net_stance_bits = 0;
    // Wire body-anim state (entity+0x2BC animStateId) + the queued arbitration target
    // (entity+0x2B8 pendingAnimStateId) + the anim-channel elapsed-ticks-in-loop, mirrored from
    // the infantry motor each tick for the 0x0A player record bytes 14/15 (emit reads
    // pending ?: current [orig: @0x4c0cc7]; ratio = trunc ticks clamp 255 [orig:
    // AnimChannel_AdvancePlayback @0x40B140 via @0x4c0cf2]). Spawn default 44 (idle2)
    // [orig: Entity_ResetToSpawnState @0x4b9714]; 43 = idle collapse.
    uint8_t net_anim_state = 44;
    uint8_t net_anim_pending = 0;
    uint8_t net_anim_phase = 0;
    // Analog control axes (entity+0x130..+0x132, the extended-uplink off-21..23 bytes):
    // joystick steering/throttle. The vehicle motor consumes the CONTROLLING occupant's
    // axes — analogX scales throttle, max(|analogY|,|analogZ|) steers [orig: the analog
    // branch of Entity_UpdateVehiclePhysics @0x48b783-0x48b7c6; pack site
    // Player_PackInputStateToEntity @0x4df450 pad7[16..19]]. Signed byte semantics.
    int8_t net_analog_x = 0;
    int8_t net_analog_y = 0;
    int8_t net_analog_z = 0;
    // Equipped-weapon AdmDef index (entity+0x2B0), echoed at this player's 0x0A off-16
    // (anim_def_index). 0xFF = none — the apply-skip sentinel the client honors (0 is a VALID
    // index: the "null" def). Ingested from the owner's extended C2S 0x0C uplink gated
    // AdmDefs[idx].category < 11 [orig: case-4 store @0x4C20A3]; host-spawned players default
    // to the WPN_M4AUTO table index [orig: PlayerClass_InitEntity @0x4B1116 resolves by name].
    // (D-NET-143)
    uint8_t equipped_adm_index = 0xFF;
    // UseGun temporarily replaces EquippedSlot with the parent's embedded slot.
    // Preserve the personal AdmDef byte. Detach restores it for a player-classified
    // occupant and clears the equipped byte for an NPC.
    // [orig: Entity_AttachToUseGunSlot @0x546c42; Entity_DetachFromVehicle
    //  restore @0x435671-0x435687, NPC clear @0x435694-0x4356aa]
    uint8_t pre_use_gun_equipped_adm_index = 0xFF;
    bool use_gun_slot_swapped = false;
    bool hidden = false;
    bool held = false;
    bool disabled = false;

    // --- destruction state (world/destruction.h; world-wac-ai-re §24) ---
    // Bound-sphere radius (entity+0 boundRadius), host-stamped from the placed
    // model's collision bounds (0 = unstamped; the damage sweeps substitute the
    // organic stand-in radius). Feeds the blast range tests and the kz fallback
    // radius. [orig: entity+0, read throughout the explosion sweep @ 0x4ead80]
    float bound_radius = 0.0f;
    // Kill-credit attacker (entity+0x178 lastAttacker): stamped by the damage
    // paths; the explosion sweep only fills an EMPTY slot with its resolved
    // attacker [orig: @ 0x4eb319/@ 0x4eb593; the dead-attacker chain walk
    // @ 0x4eae95..0x4eaece].
    EntityHandle last_attacker;
    // The blast center a queued explosion stamped before this entity died — the
    // section-debris launch origin (entity+0x80 savedLivePose reuse; only
    // destructible-class entities receive it) [orig: @ 0x4eb553-0x4eb569].
    Vec3 death_blast_center;
    // The death tick (entity+0x1AC, first write wins) [orig:
    // Entity_ProcessDestructibleDeath @ 0x43fc0c / AI_TransitionToDestroyed_Vehicle].
    uint32_t death_tick = 0;
    // Hidden/dismembered skeletal sections (entity+0x134): a set bit removes
    // the matching ordinal bone from person collision and presentation.
    // Distinct from spawned_piece_mask at +0x138.
    uint32_t section_mask = 0;
    // Husk sections that left as death pieces (entity+0x138): the husk renders
    // and collides WITHOUT these sections. Bit 0 (the hull) never sets.
    // [orig: Entity_SpawnDeathPieces @ 0x493983]
    uint32_t spawned_piece_mask = 0;
    DeathMotionMode death_motion = DeathMotionMode::None;

    // The retail entity Flags dword (entity+36) as composed at spawn — the 0x10 static record
    // streams it RAW as its flag-0x20 i32 (the field the early RE misread as "parentSlot",
    // D-NET-147/150). Composed from mission attributes + item-def traits:
    //   BMS Indestructible(1<<21) -> 0x4000000, Reflective(1<<23) -> 0x400,
    //   NoShadow(1<<24) -> 0x1000000            [orig: Entity_SpawnFromBMSRecord @0x40e9f0]
    //   kind Building                -> 0x20000  [orig: Entity_InitFromModel @0x40e105]
    //   items.def hp == 0            -> 0x4000000 (+ sub_type 0xFF) [orig: @0x40dc8e]
    // Organic low-byte runtime/wire state also has a legacy flags mirror. Retail
    // consumers of player 0x100 and guarding/mounted 0x40 keep both views coherent.
    uint32_t engine_flags = 0;
    // entity+290 low byte <- BMS record byte 81; always-present byte of the 0x10 static record
    // (golden buildings carry 0xFF). [orig: Entity_SpawnFromBMSRecord @0x40e9f0]
    uint8_t ammo_count = 0;
    // entity+532 subType — 0xFF when the item def is indestructible (hp 0) [orig:
    // Entity_InitFromModel @0x40dc85]; the 0x10 record's flag-0x80 byte.
    uint8_t sub_type = 0;
    // entity+533 refNum <- BMS record byte 153; the 0x10 record's flag-0x40 byte (D-NET-94).
    uint8_t ref_num = 0;

    // --- Advance & Secure zone fields (net-re §5.61) ---
    // entity+538 <- BMS record byte 155 (.mis "lfp_group") — the authored AS zone number;
    // 0 = not a chain zone (plain flag/base). [orig: Entity_SpawnFromBMSRecord @0x40e9f0]
    uint8_t zone_number = 0;
    // entity+350 (0x15E) <- BMS record word 14 (wp_distance low u16) — the capture-zone /
    // proximity radius. Streamed as the 0x0D record's 0x2000/0x8000-gated u16 (golden ASH_I5A
    // bunkers: 70) and read by the client's zone-radius consumers (CaptureZone_* /
    // render_minimap_slot_blip). [orig: Entity_SpawnFromBMSRecord @0x40e9f0; 0x0D writer
    // serialize_entity_pool_to_packet_0 @0x503ecc/@0x503f29; net-re §5.11/§5.61]
    uint16_t zone_radius = 0;
    // entity+540 — the 16.16 SECURE/control fraction 0..0x10000. A numbered zone accepts
    // spawns only at >= 0x10000; flips reset it to 0 and the owner re-secures. Seeded by the
    // chain latch (zone_chain_latch_control): 1.0 when the enemy frontier cannot reach it.
    // [orig: Server_UpdateCaptureZoneEntities @0x519690 latch @0x519764; clamp @0x501499]
    int32_t zone_control = 0;
    // ItemDefAttrib & 0x20000 "ChangeTeam" — capture-trigger volume (joins the zone chain).
    // [orig: ZoneSlotChain_BuildFromMission @0x4a2de0 def+84 & 0x20000 gate]
    bool is_capture_trigger = false;
    // ItemDefAttrib & 0x40000 "SpawnPoint" — deploy-selectable spawn target (0x0E picks).
    // [orig: Server_ResolveSpawnTargetHandle @0x4fe110 def+84 & 0x40000 gate]
    bool is_spawn_point = false;

    uint32_t spawn_origin = 0; // back-ref to the BMS (kind,index) it was promoted from
    std::string name;          // named markers/areas

    // --- vehicle/emplacement mounting (AttachToEmplaced) ---
    // Seats this entity OFFERS as a vehicle/emplacement (mirrors vehicle[400..] + model[605..]).
    // Empty for plain entities; an emplaced gun seeds one Gunner seat.
    std::vector<Seat> seats;
    // "armory*" userpoint locals of an Armory-attrib item (items.def attrib 0x80000) —
    // the floating armory-label anchors and the armory leg of the nearest scan. Non-empty
    // ONLY for armory sources (the host feeds points only when the attrib is set, matching
    // the original's attrib gate). [orig: the attrib & 0x80000 gate @0x4361ee/@0x5a36f5 +
    // the "armory" userpoint walk @0x436226/@0x5a372b]
    std::vector<Vec3> armory_points;
    // items.def 'primary_weapon' — the weapon.def entry this ewep emplacement mounts (the
    // gun entity's slot-0 weapon; the USEGUN attach label resolves its attachtextid).
    // Empty for non-emplacements. [orig: ItemDef+0x54B primaryWeapon; label consumer
    // draw_vehicle_seat_and_armory_labels @0x5a351d via Entity_GetWeaponSlots slot0]
    std::string primary_weapon;
    // Child entity created from its parent's items.def addeweap* slot. All
    // variants share the parent/userpoint carry relation; G/C remain explicit
    // metadata (G flag bit 2, C flag bit 1 in retail) for their distinct
    // mount/HUD consumers. A zero bone is the witnessed parent-root fallback.
    EntityHandle emplacement_parent;
    uint64_t emplacement_parent_spawn_id = 0;
    Vec3 emplacement_local;
    int16_t emplacement_yaw_offset = 0;
    uint8_t emplacement_bone = 0;
    uint8_t emplacement_kind = 0;
    uint8_t emplacement_slot = 0;
    uint8_t emplacement_attachment_flags = 0;
    uint8_t emplacement_angle_count = 0;
    int32_t emplacement_down_limit_bam = 0;
    int32_t emplacement_up_limit_bam = 0;
    int32_t emplacement_right_limit_bam = 0;
    int32_t emplacement_left_limit_bam = 0;
    // The emplacement's embedded MountSlot (parent+0x2B4). A UseGun occupant borrows
    // this slot: the AI update only queues nextAction=FIRE, then the later global
    // weapon-action pump owns cadence/ammo and attributes the round to slot.owner.
    // [orig: Entity_AttachToUseGunSlot @0x546b80; WeaponAction_ProcessAllEntities
    //  @0x542690; WeaponAction_Fire @0x542b10]
    WeaponSlotState primary_weapon_slot;
    uint8_t primary_weapon_slot_adm = 0xFF;
    EntityHandle primary_weapon_owner;
    // Host-fed posed muzzle/userpoint on this entity. For a UseGun shot this is
    // sampled from the emplacement model, while primary_weapon_owner remains the
    // organic shooter for damage/network attribution.
    // [orig: Entity_CalcWeaponFirePosition parentSlot 3 from WeaponAction_Fire]
    int32_t posed_muzzle_world[3] = {};
    uint32_t posed_muzzle_tick = 0;
    bool posed_muzzle_valid = false;
    // The single tracked FIRST occupant (entity+368 occupantEntity): claimed at attach by
    // ctrlx/drvrx (empty-or-same) and UseGun (only when empty), never by sitex; cleared only
    // when THE claimant detaches — a remaining second controller does not inherit it. This
    // is the retail engine-running latch: the PlayerControl occupancy effect (and the engine
    // start/stop sounds) key off it, not off any-control-seat occupancy.
    // [orig: Entity_AttachToVehicleSlot @0x4946d0 writes +368 @0x4947d2/@0x4948d8/@0x49495e;
    //  Entity_DetachFromVehicle @0x4355f0 stop leg @0x4356e9..0x435759 + clear @0x43577c;
    //  spawner gate @0x48faad in entity_update_damage_accumulator_and_shadow @0x48fa70]
    EntityHandle primary_occupant;
    // Target-side mounted skeletal/clip configuration: items.def phrase_set at
    // itemDef+0x86C. Explicit validity keeps absent metadata distinct from the
    // witnessed config 0 branch.
    bool emplaced_config_valid = false;
    int32_t emplaced_config = 0;
    // Occupant side: this entity is RIDING mount_target's seat mount_seat. The host bool
    // represents the parent relationship for every seat. Retail's generic vehicle attach
    // also sets Flags 0x40 [orig: Entity_AttachToVehicleSlot @0x494752], while UseGun
    // deliberately does not [orig: Entity_AttachToUseGunSlot @0x546c5c].
    // mounted == false => the rest are unset.
    EntityHandle mount_target;          // kInvalid = not mounted
    // Stable identity of mount_target at attach time. The handle can stop resolving before
    // occupant teardown; these fields preserve the control-stop notification payload.
    uint16_t mount_target_net_id = 0;
    int32_t mount_target_bms_id = 0;
    uint32_t mount_target_spawn_origin = 0;
    int8_t mount_seat = -1;
    SeatType mount_type = SeatType::None;
    bool mounted = false;
    // Occupant-side copy of the target configuration. This is the active selector
    // metadata consumed by animation/collision/presentation and serialized for
    // remote presentation. Dismount clears it; registry snapshots value-copy it.
    bool mounted_config_valid = false;
    int32_t mounted_config = 0;
    // The RAW wire seat-bone index this occupant attached by (entity+0x157 attachBoneId,
    // 1-based into the vehicle MODEL's bone table) — the value the C2S 0x26 carried and the
    // 0x0A mounted player record echoes as byte 0 (clients resolve their own seat from it).
    // [orig: Entity_AttachToVehicleSlot @0x4946d0 common tail @0x494752-75 writes 0x157;
    // record write @0x4c0a1a]
    uint8_t mount_bone = 0;

    // The packed blink-box hits this entity currently sits inside (up to 4), refreshed by
    // the collision pass: ((section & 0x1F) | (pool_index << 8)) << 12. The renderer's
    // interior-lighting group selection reads these; hit presence + accum flag bit 2 set
    // the Flags 0x800000 "indoors" bit. [orig: the per-entity blink quad stamped by
    // Entity_BuildProximityList @ 0x4b406b / movement collision resolver
    // @ 0x4b36f0 from g_BlinkHitSlot0..3 @ 0xB57C74]
    uint32_t blink_hits[4] = {};

    // Render-occlusion three-ray latch countdown (entity+342): while nonzero the
    // entity renders and the byte counts down per render frame; at 0 the outdoors
    // three-ray terrain probe re-decides and re-arms it to (rand16 & 7) + 16.
    // [orig: the latch bytes in the visible-entity collectors @ 0x5c7125-0x5c7162 /
    // 0x5c6cd9-0x5c6d0d; docs/render/render-occlusion-re.md §3.4]
    uint8_t occlusion_latch = 0;

    // Ground/carrier reference (entity+0x28 groundEntity): the entity this one stands on
    // — a building floor or vehicle deck, any pool — as maintained by the ground probe.
    // CL/type-4 ladder contact also uses this field transiently with Flags 0x100000;
    // that is ladder bookkeeping, not a platform volume [orig: @0x4b3291]. For
    // READ-APPLIED peers apply_player_intent mirrors the
    // carrier the owning client uplinked (§5.10; D-NET-151) and the 0x0A echo re-emits
    // it (mount wins over ground [orig: NetPacket_SerializePlayerState op1 @0x4c0a08]).
    EntityHandle ground_target;         // kInvalid = free-standing

    // The shooter's last claimed fire target (entity+104 -> +12): stamped per accepted
    // C2S 0x06 [orig: Server_ClientFiredRound @0x50c2ad stores the resolved target ptr],
    // read LIVE at 0x0A tag-2 serialize time — a set handle adds the wire 0x40 flag +
    // target word [orig: NetPacket_SerializeRoundEvent @0x50485a]. (D-NET-152)
    EntityHandle last_fire_target;      // kInvalid = no target claimed

    // --- vehicle motor state (pool-1 PlayerControl vehicles; world/vehicle_motor.h) ---
    // The original keeps this state across the entity struct and the 812-B per-entity
    // AI/physics component (`vehicleData` = *(entity+100)); the slot comments name the
    // original homes. [orig: Entity_UpdateVehiclePhysics @0x48af00]
    struct VehicleMotorState {
        int32_t yaw_bam = 0;          // 32-bit engine-frame heading (entity+0x10). Entity::yaw
                                      // (mission deg) mirrors (90 - bam/deg) each motor tick —
                                      // steering accumulates sub-degree BAM deltas.
        bool yaw_seeded = false;      // yaw_bam initialized from Entity::yaw on first tick
        int32_t speed = 0;            // currentSpeed, 16.16 u/tick [orig: entity+0x29C]
        int32_t speed_accel = 0;      // per-tick speed delta [orig: entity+0x2A0 speedAccel]
        int32_t cmd_speed = 0;        // commanded/target speed [orig: vehicleData+544]
        int32_t steer_target_bam = 0; // steering target heading [orig: vehicleData+528]
        int32_t steer_ramp_bam = 0;   // key-steer ramp offset [orig: vehicleData+548]
        int32_t steer_state = 0;      // smoothed wheel deflection [orig: entity->aiState reuse]
        int32_t wheel_rate_bam = 0;   // grounded yaw rate [orig: entity->modelPtr0 reuse]
        int32_t vel_x = 0;            // world velocity, 16.16 u/tick — persists airborne
        int32_t vel_y = 0;            // (ballistic) [orig: entity velocityX/Y +0x98/+0x9C]
        int32_t slide_z = 0;          // vertical velocity, 16.16 [orig: slideDecay +0xA0]
        bool reverse_sound_latched = false; // movement-sound direction bit
                                            // [orig: vehicleData+0x318 bit 2]
        uint32_t sound_anchor_until_tick = 0; // keep residual lanes attached after claimant loss
        bool grounded = true;         // wheel contact [orig: BYTE2(entity->aiRef0) reuse];
                                      // vehicles spawn RESTING (contact resolved at init),
                                      // so the default is grounded — the first motor tick
                                      // re-derives it from the terrain clamp
    };
    VehicleMotorState veh;
};

} // namespace opennova::world

#endif // OPENNOVA_WORLD_ENTITY_H
