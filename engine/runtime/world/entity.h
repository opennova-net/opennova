// Addressable entity model for the OpenNova runtime world.
//
// Faithful to the original engine's entity addressing (EntityPool_FindByNetId
// @0x4f0a20, Jointops.exe): entities live in fixed-capacity pools; each carries a
// 16-bit net id (the "SSN" WAC/BMS scripts reference). A live entity is named by a
// packed handle (pool<<12 | slot) — 4-bit pool index, 12-bit slot.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <runtime/world/weapon_fsm.h>
#include <runtime/world/ai_target_selectors.h>

#include <runtime/world/geom.h>

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

// Retail's signed Q16.16 multiply used by Entity_InitFromModel when it folds
// ItemDef/entity scale into the bbox midpoint and model bound. The x86 sequence
// is `imul; add eax,0x8000; adc edx,0; shrd eax,edx,16`: the +0x8000 bias is
// applied for both signs, then the signed product shifts arithmetically.
// [orig: Entity_InitFromModel @ 0x40dc30]
constexpr int32_t retail_q16_mul_rhu(int32_t value_q16,
                                    int32_t scale_q16) noexcept {
    const int64_t product = static_cast<int64_t>(value_q16) * scale_q16;
    const int64_t shifted = (product + 0x8000LL) >> 16;
    return static_cast<int32_t>(static_cast<uint32_t>(shifted));
}

// The retail signed-16 storage domain those fields live in (health, health_max,
// armor classes, ...): the debug/edit surfaces clamp to it.
inline constexpr int32_t kRetailI16Min = -32768;
inline constexpr int32_t kRetailI16Max = 32767;

// Mirrors mission::EntityKind / bms::ItemType. Kept independent so engine/runtime/world
// has no dependency on engine/runtime/mission (promotion adapts between them).
enum class EntityKind : uint8_t {
    Marker = 0,
    Item = 1,
    Building = 2,
    Organic = 3,
};

// Live entity pools 0..4 [orig: the g_pool_list walk bound @0x431910].
inline constexpr int kEntityPoolCount = 5;
// Fixed g_pool_list capacities, used by mission promotion and as the memory-safe
// bound behind the retail load handlers' unchecked pool indexing
// (Pool_GetEntryUnchecked walks a fixed-capacity pool; the serving side never
// exceeds its own capacity). Witnessed per pool: organics 256, items 1200,
// buildings 1200, pool-3 768, effects 128.
// [orig: EntityPool_Allocate @0x442168 — capacity stores @0x4421cd (pool 0, 256),
//  @0x44219c/@0x4421a2 (pools 1/2, 1200), @0x442203 (pool 3, 768),
//  @0x44221e (pool 4, 128)]
inline constexpr std::size_t kRetailPoolCapacity[kEntityPoolCount] = {
    256, 1200, 1200, 768, 128};
constexpr std::size_t retail_pool_capacity(int pool) {
    return pool >= 0 && pool < kEntityPoolCount
            ? kRetailPoolCapacity[pool]
            : 0;
}

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
// kSpawnOriginNone = none.
inline constexpr uint32_t kSpawnOriginNone = 0xFFFFFFFFu;
constexpr uint32_t spawn_origin_pack(uint32_t kind, uint32_t index) {
    return (kind << 24) | (index & 0xFFFFFFu);
}
constexpr int spawn_origin_kind(uint32_t origin) { return static_cast<int>(origin >> 24); }
constexpr int32_t spawn_origin_index(uint32_t origin) {
    return static_cast<int32_t>(origin & 0xFFFFFFu);
}
// The decoded halves of kSpawnOriginNone as they appear wherever provenance is
// carried split (the present rows' PF_KIND/PF_INDEX): a wire-only row with no
// local BMS record decodes to kind 255 / index 0xFFFFFF. Named so no consumer
// re-derives the "no local record" sentinel by hand.
inline constexpr int kSpawnOriginKindNone = spawn_origin_kind(kSpawnOriginNone);      // 255
inline constexpr int32_t kSpawnOriginIndexNone = spawn_origin_index(kSpawnOriginNone); // 0xFFFFFF

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
    PiecePitchSettle = 5,
    CraneFalling = 6, // @0x43FB30; also runs before the paired section dies
    BuildingEffects = 7, // upfx @0x4A92E0
    PalmPiece = 8, // psec @0x53BE10
    SectionFalling = 9, // towr @0x4A8340
    SectionSettled = 10, // @0x4A8220
};

// Minimal live-entity state the scripting evaluators read and mutate. This is a
// clean model over the original 172-byte bms record + the pool record's net id;
// the renderer/AI's full entity layout is a separate, deferred concern.
// Named mirrors of the DEF_ITEM_ATTRIB_* bits world/replication code reads off the
// entity's ItemDefAttrib dword (Entity::item_attrib, and the same dword on
// ai.h's def_attrib profile mirror). engine/runtime/world stays def-parser-free; parity
// static_asserts against def.h live in runtime/world/weapon_table_build.cpp.
// [orig: ItemDef_ParseProperty @0x49eb00; docs/world/itemdef-re.md:147-155]
inline constexpr uint32_t kItemAttribMoveCallback = 0x1u;
inline constexpr uint32_t kItemAttribPowerup = 0x2u;
inline constexpr uint32_t kItemAttribEweap = 0x20u;
inline constexpr uint32_t kItemAttribPlayerControl = 0x40u;
inline constexpr uint32_t kItemAttribObjectiveTarget = 0x8000u;
inline constexpr uint32_t kItemAttribLandable = 0x200u;
inline constexpr uint32_t kItemAttribChangeTeam = 0x20000u;
inline constexpr uint32_t kItemAttribSpawnPoint = 0x40000u;
inline constexpr uint32_t kItemAttribArmory = 0x80000u;
inline constexpr uint32_t kItemAttribAIData = 0x100000u; // §5.6 AI class — gates the 0x0D AI-trailer
inline constexpr uint32_t kItemAttribLeaveCorpse = 0x400000u;
inline constexpr uint32_t kItemAttribNoDismember = 0x800000u;
inline constexpr uint32_t kItemAttribNoHud = 0x20000000u;
inline constexpr uint32_t kItemAttribNoDie = 0x40000000u;

// The "no ADM slot" sentinel every mount/equip byte uses (primary_weapon_slot_adm,
// equipped_adm_index): 0xFF = none selected.
inline constexpr uint8_t kAdmSlotNone = 0xFF;

// The retail entity Flags dword bits (Entity::engine_flags + the organic
// low-byte legacy `flags` mirror; entity+36). ONE home for every bit with a
// witnessed meaning; the consolidated per-bit table is
// docs/world/world-wac-ai-re.md § "The entity Flags dword". Remaining
// known-but-unnamed bits stay raw at use sites — do not name: 0x10000, 0x2000000,
// 0x8000000, and vehicle_motor's Flags-dword 0x8/0x20 writes (vehicle-context
// meanings unwitnessed).
inline constexpr uint32_t kEntityFlagCarried = 0x1;          // hidden while attached [orig: Entity_AttachToVehicle @0x43C130]
inline constexpr uint32_t kEntityFlagDead = 0x2;              // [orig: kill writes Flags |= 6 @0x43fbf6]
inline constexpr uint32_t kEntityFlagHusk = 0x4;              // items/buildings: husk swap [orig: @0x43fbf6]
inline constexpr uint32_t kEntityFlagNVGWorn = 0x4;           // organics: NVG draw, same bit kind-dependent
                                                              // [orig: draw @0x4e3b54; refresh of bits 2-4 §13.1]
inline constexpr uint32_t kEntityFlagBinoculars = 0x8;        // [orig: draw @0x4e3c04; g_binocularsRaised refresh]
inline constexpr uint32_t kEntityFlagScopeRaised = 0x10;      // [orig: g_weaponScopeActive refresh; test @0x4b5deb]
inline constexpr uint32_t kEntityFlagParachute = 0x20;        // deployed chute (D-INF-20) [orig: radius leg @0x4b3aac]
inline constexpr uint32_t kEntityFlagAiClimb = 0x80;          // org1 ladder-climb chase mode: gravity becomes the
                                                              // sixteenth-step Z chase to the AI move target (floor
                                                              // -16384); set at spawn by the BMS record's attribute
                                                              // bits 0x4000 / 0x20000 [orig: Entity_SpawnFromBMSRecord
                                                              // @0x40EE2A..0x40EE33, @0x40EE70..0x40EE79]
                                                              // [orig: test @0x4bf6c1; chase @0x4bf6d2-0x4bf6e5]
inline constexpr uint32_t kEntityFlagMounted = 0x40;          // carried/mounted; the AI guard family reads it too
                                                              // [orig: @0x494752; guard @0x4bf5a5-family]
inline constexpr uint32_t kEntityFlagPlayer = 0x100;          // the wire Player class bit; gates held-weapon draws
                                                              // [orig: §5.10b class; draw gate @0x4e5073-family]
inline constexpr uint32_t kEntityFlagQueuedMount = 0x200;     // the queued Co-op spawn-marker mount: set with
                                                              // +0x16C/+0x180 = the marker's parent by the no-pick
                                                              // team-2 marker arm, consumed (toggle, then cleared)
                                                              // by the first org2 body update; the toggle's
                                                              // FindBestSeatSlot(groundEntity) arm keys on it
                                                              // [orig: Server_PositionPlayerForSpawn @0x50D44D;
                                                              //  Entity_UpdateInfantryPlayerBody @0x4B424A..0x4B4272;
                                                              //  Entity_TryEnterNearestVehicle @0x4368CF]
inline constexpr uint32_t kEntityFlagReflective = 0x400;      // BMS Reflective(1<<23) [orig: @0x40e9f0]
inline constexpr uint32_t kEntityFlagVehicleLoadoutZone = 0x800;  // type-11 volume touch
inline constexpr uint32_t kEntityFlagInAir = 0x2000;          // airborne/swimming [orig: grounded selector @0x4b78ab]
inline constexpr uint32_t kEntityFlagPriorityTarget = 0x4000; // set on every fire, decays per perception scan
                                                              // [orig: @0x4bf370 set; @0x4bbfa4 clear; §16.2 x6 scoring]
inline constexpr uint32_t kEntityFlagDrowning = 0x8000;       // zeroes vertical swim input [orig: §7 movement]
inline constexpr uint32_t kEntityFlagBuilding = 0x20000;      // [orig: Entity_InitFromModel @0x40e105]
inline constexpr uint32_t kEntityFlagNoEngage = 0x80000;      // org1 combat think: skips the attack-stance aim
                                                              // block [orig: @0x4bc958] and the reaction/approach
                                                              // arm [orig: @0x4bc054, unported there]; the writer
                                                              // is unwitnessed (world-wac-ai-re.md §17 open item 7)
inline constexpr uint32_t kEntityFlagLadderContact = 0x100000; // CL/type-4 touch; locks upper-body pose + skips
                                                               // gravity while aligned [orig: @0x4b3291]
inline constexpr uint32_t kEntityFlagArmoryZone = 0x400000;   // type-6 volume touch [orig: @0x4aea45]
inline constexpr uint32_t kEntityFlagIndoors = 0x800000;      // [orig: accum bit 2 -> Flags @0x4b39xx; render gates §4]
inline constexpr uint32_t kEntityFlagNoShadow = 0x1000000;    // BMS NoShadow(1<<24) [orig: @0x40e9f0]
inline constexpr uint32_t kEntityFlagIndestructible = 0x4000000; // BMS Indestructible(1<<21) or hp==0
inline constexpr uint32_t kEntityFlagScriptDisabled = 0x10000000; // WAC disableSSN; the vehicle motors test it with
                                                                  // the dead bit (10000002h) as their driver-input
                                                                  // gate [orig: WacCmd_DisableSsn @0x4F76DD;
                                                                  //  Entity_UpdateVehiclePhysics @0x48B980;
                                                                  //  Entity_UpdateAircraftPhysics @0x490F1E]
// The entity+0x2C dword (Entity::cause_flags) bit WAC holdSSN sets and
// unholdSSN clears; the org1 think holds the NPC in place while it is set.
// [orig: WacCmd_HoldSsn @0x4F785D; Entity_UpdateInfantryAI @0x4BD235]
inline constexpr uint32_t kCauseFlagScriptHold = 0x2000;

// The BMS-attribute part of a streamed Flags dword, mapped back onto the
// record attribute bits the placement traits read (Reflective 1<<23,
// NoShadow 1<<24, Indestructible 1<<21): the exact inverse of the spawn
// mapping [orig: Entity_SpawnFromBMSRecord @0x40e9f0, the three attrib tests
// @0x40ed14 / @0x40ed29 / @0x40ed36: 0x200000 -> 0x4000000, 0x800000 ->
// 0x400, 0x1000000 -> 0x1000000]. A joiner's 0x10
// static record streams the dword raw, so this is how its placed statics
// recover the mirror/shadow attributes the host reads off the file.
constexpr uint32_t bms_attributes_from_entity_flags(uint32_t flags) {
    uint32_t attrib = 0;
    if (flags & kEntityFlagReflective) attrib |= 0x00800000u;
    if (flags & kEntityFlagNoShadow) attrib |= 0x01000000u;
    if (flags & kEntityFlagIndestructible) attrib |= 0x00200000u;
    return attrib;
}
                                                                 // [orig: @0x40e9f0; @0x40dc8e]

// One shared bank of sixteen persistent movement emitters. W1/W2 on land
// and W3/W4 in water address these same slots, selected by the userpoint mask.
// [orig: Entity_UpdateBoneTrailEffects @0x4589C0, entity+0x408..0x444]
struct VehicleTrailPoint {
	uint8_t definition = 0;
	uint32_t magnitude_q16 = 0;
	uint32_t source_tick = 0;
	Vec3 position;
	Vec3 direction;
};
struct VehicleTrailState {
	VehicleTrailPoint points[16];
	uint16_t effect_state[2] = {};
};

// The lndm entity overlay. Zero-filled, unauthored slots still participate.
// [orig: Entity_InitHardpoints @ 0x4417D0; Entity_LandmineThink @ 0x441A40]
struct MinefieldState {
    static constexpr int kSlots = 14;
    bool initialized = false;
    bool think = false;
    bool render = false;
    int32_t age = 0;
    uint32_t ammo_small = 0, ammo_large = 0;
    int32_t placement[16] = {};
    int16_t offsets[kSlots][3] = {};
    uint8_t types[kSlots] = {};
    uint8_t rotations[kSlots] = {};
    std::string small_marker, large_marker;
};

struct Entity {
    MinefieldState minefield;
    // [orig: door slot base +0x2B8, section touch mask +0x2B4;
    //  ItemDef door/target event classes @0x8131F8/@0x813210]
    int16_t door_slot = -1;
    int8_t door_count = 0;
    int8_t door_first_bone = 0;
    bool door_initialized = false;
    bool door_event = false;
    bool door_motion = false;
    uint32_t door_touch_mask = 0;
    char door_open_sound[25] = {};
    char door_close_sound[25] = {};
    uint16_t net_id = 0;      // SSN; the field WAC/BMS address entities by
    int32_t bms_id = 0;       // file entity id (bms::Entity::id); the host keys placed nodes by this
                              // (MissionEntityRegistry), distinct from the runtime net_id/SSN.
    EntityHandle handle;      // self-handle (assigned at spawn)
    // Monotonic registry lifetime identity. Packed handles intentionally reuse
    // fixed pool slots; this host-only serial distinguishes two entities that
    // occupied the same slot, even when all authored/net fields are identical.
    uint64_t registry_spawn_id = 0;
    // Item class callback countdown, retail entity+0x2AC. Pool 1 subtracts
    // one after its callback; pool 2/3 positive clocks subtract 8/64 at their
    // matching slot cohort, and expired clocks run without a trailing subtract.
    int32_t class_think_ticks = 0;

    // The owning connection's ConnectionId/dcb (GamePlayerEntity entity+0x78). The joining client's
    // self-scan matches it against its own ConnectionId; a host/dedicated-server reserves dcb 0. This
    // is the runtime home of what the wire models as OrganicSpawnRecord::entity_flags (the 0x0C
    // entity+0x78 field). [orig: Server_PlayerAdd @0x51cbc0 writes entity+0x78 = conn->connection_id;
    // matched in Player_FindLocalPlayerEntity @0x4e0090; net-re §5.2b / D-NET-92/101]
    uint32_t owner_connection_id = 0;

    EntityKind kind = EntityKind::Item;
    int32_t item_id = 0;      // items.def type id
    // entity+0x1C ItemTypeIndex: the item's ordinal in the items.def load order,
    // the first row whose type id matches and 0 when none does (the "Null" row is
    // ordinal 0 too). Stamped by the item-traits sweep.
    // [orig: Entity_SpawnFromBMSRecord `mov [esi+1Ch],ebp` @0x40EBFC, ebp from
    //  ItemList_FindIndexByTypeId @0x49E100]
    int32_t item_type_index = 0;
    bool has_item_def = false; // retail entity+0x20 ItemDef pointer is non-null
	bool render_sway = false;
	uint8_t item_type = 0; // raw ItemDef+0x5C type (1 vehicle, 3 person)
	// Whether the live graphic model carries the +0xE0 portal/occlusion pointer
    // used by Entity_ClassifyForMinimap for ordinary Building entries. Kept as
    // a resolved entity trait because engine/runtime/world deliberately does not own .3di
    // assets. Armory/zone/etc. classifiers do not require it.
    bool has_minimap_model_marker = false;
    // The graphic model's XY half-extents (mission axes, 16.16), stamped by
    // the same model-resolve seam. The minimap blip drawer sizes footprint-
    // class blips from these; 0 = unstamped (the 10-wu class fallback).
    // [orig: draw_minimap_blip @0x5979a2..0x5979b8 — model+176 bound block,
    //  half = (max - min) >> 1 per axis; fallback 655360 @0x5979cb]
    int32_t minimap_half_x_q16 = 0;
    int32_t minimap_half_y_q16 = 0;
    // Model collision-bbox CENTER (entity+0x1FC/+0x200/+0x204), host-stamped
    // from the placed CMDL bounds in MODEL-LOCAL axes and added RAW
    // (unrotated) wherever retail offsets a ray endpoint by it — the cat-2
    // trigger LOS endpoints. Zero when unstamped (no collision block, or the
    // retail powerup rule attrib&0x20 && type 6) — the ray then leaves the
    // raw position, matching retail's zeroed pool memory. The effective
    // entity/item scale is already folded here with retail's signed-Q16
    // +0x8000 multiply before any consumer sees the value.
    // [orig: Entity_InitFromModel center @0x40df1e..0x40df4a, powerup zero
    //  @0x40df0a, scale @0x40dfd6..0x40e018; the LOS reads
    //  @0x4f1880..0x4f18c5 (sub 45) and @0x4f1728..0x4f176f (sub 44)]
    Vec3 bbox_center;
    // The def "TARGET" userpoint (def+1350, a 1-based byte index into the
    // model's userpoint table; 0 = none), resolved when the entity's model
    // attaches [orig: Entity_InitFromModel @0x40dd04 resolves the hardcoded
    // name TARGET]. The non-person aim/LOS origin transforms it by the
    // placement matrix [orig: Entity_ComputeWeaponFireOrigin @0x43b5d4..0x43b5f6].
	uint8_t look_userpoint_byte = 0; // def+1351, LOOK; fire-validation LOS origin
	uint8_t target_userpoint_byte = 0;
	// The mounted first-person camera sources (world/local_player_view.h,
	// local_player_mounted_camera). An EWEAP resolves the "CAMERA" userpoint of
	// its OWN model (entity+0x318, 1-based, 0 = none). A carrier whose def
	// authors `virtualdisplay <model> <userpoint>` carries that userpoint's
	// record position out of the VIRTUAL-DISPLAY model; retail keeps the byte
	// def+0x1C0 and reads the record at use. input_class is the def's
	// `input_function` row: 0 null, 1 troop, 2 tank.
	// [orig: Entity_InitBoneReferences @0x4414A9..0x4414B4;
	//  EntityDef_LoadModelsAndCallbacks @0x43A5D3..0x43A644; rows @0x829DA8]
	uint8_t camera_userpoint_byte = 0;
	uint8_t input_class = 0;
	bool virtual_display_camera = false;
	int32_t virtual_display_camera_q16[3] = {};
	std::string virtual_display_model; // the cockpit graphic key, lowercased
	int32_t item_unit_type = 0; // raw ItemDef unit_type; vehicle minimap icon selector
	// ItemDef+0x194 signed word `score`: the kill value. Zero (every Player
	// definition) keeps the victim out of the kill accounting entirely.
	// [orig: Score_ProcessKillEvent @0x4FD422]
	int32_t item_score = 0;
    bool is_ai_capable = false; // items.def ItemDefAttrib & 0x100000 (AIData / §5.6 AI class). Gates the
                                // 0x0D AI-trailer (D-NET-97). Distinct from ai_flags (BMS). [docs/world/itemdef-re.md]
    // The §5.10b wire replication class, resolved from the item's items.def *_function class
    // tag (ai_function, else move_function -> ItemDef+356 serialize callback) and stamped by
    // the host's post-promotion item-traits sweep. Stored as an OPAQUE code (the novaworld
    // EntityClass value; engine/runtime/world stays net-agnostic) — 0xFF = unresolved, replication falls back
    // to its minimal heuristic. Load-bearing: a pool-1 item that is NOT a vehicle class (e.g.
    // ai_function ewep emplacements) must NOT be serialized with the vehicle compact record or
    // the client desyncs mid-frame (retail-join v13, 2026-07-02).
    uint8_t net_class_code = 0xFF;

    uint16_t facial_slot = 0; // entity+440, GRM slot index | 0x8000
    bool facial_checked = false; // model initialization attempted for this allocation
    Vec3 position;            // mission space (Z-up)
    int16_t yaw = 0;
    int16_t pitch = 0;
    int16_t roll = 0;
    // The eye/camera-offset triple (entity+0x6C/+0x70/+0x74, 16.16): the
    // stance-driven eye above the entity origin, restamped each body tick.
    // NPCs take the capsule extent tilted by lean (lateral = delta*sin(lean)*
    // 3/4 rotated by heading [orig: Entity_UpdateInfantryAI @0x4bf078..
    // 0x4bf14c]); the local player takes the exact posed-head-minus-Position
    // triple when the shell feeds a skeleton sample, the head z terrain-floored
    // on foot [orig: Entity_UpdateInfantryPlayerBody on-foot
    // @0x4b6bb3..0x4b6cc8; mounted @0x4b6908..0x4b696c]. The retail camera
    // adds the triple to the tracked entity's position [orig:
    // Camera_ComputeThirdPersonView @0x437fa5..0x437fb7]; the friendly-tag
    // anchor reads z only (anchor z = z + this + 0x4000 [orig:
    // HUD_DrawEntityLabel @0x5a3a84..0x5a3a98]); replication's water line derives
    // its own z copy. 0 = never stamped (no anim channel), matching the retail
    // spawn value; the local-player seeds (0xB333 [orig: Player_InitPlayer
    // @0x4e18a1], deploy 0xD000 [orig: NapiNPClientMsg_0x00A @0x42ffc9]) are
    // subsumed by the first body tick. Witness: docs/interface/hud-re.md
    // D-HUD-20.
    int32_t eye_offset_x = 0;
    int32_t eye_offset_y = 0;
    int32_t eye_offset_z = 0;

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
    // GamePlayerEntity.Name (entity+0xF4): BMS copies 15 characters; WAC
    // ssnname copies 31 [orig: WacCmd_SsnName @0x4F7230]. The display name the
    // friendly-tag drawer reads. Authored at BMS promote from the record's
    // name_index through the mission RTXT [PeopleNames] STRNAME%03i entry;
    // empty resolves the compiled-in fallback table at draw time.
    // [orig: Entity_SpawnFromBMSRecord @0x40ecbf..0x40ed0a; read
    //  HUD_DrawEntityLabel @0x5a4021]
    std::string display_name;
    uint8_t group_id = 0;     // BMS commandGroup (+284), not the WAC named-group table
    uint8_t waypoint_id = 0;  // wplist / route this entity follows
    int32_t wp_number = 0;    // position along that route

    AiTargetSelectors target_selectors;
    uint8_t alert_state = 0;  // green/yellow/red
    int32_t ai_state = 0;     // AI component state
    int32_t ai_target = -1;   // net id of current AI target, -1 = none
    // Targeted-by refcount (entity+530): ++ when an AI acquires this entity, -- (clamp 0)
    // when it retargets/clears. The target scorer reads it as the anti-pile-on saturation
    // gate (<=16) and score decay. [orig: Entity_SetAITarget @0x45d760 maintains it;
    // AI_FindBestTargetB @0x466f60 consumes; world-wac-ai-re §16.3]
    int16_t ai_target_refcount = 0;
    // Tracer cadence byte for SLOT-LESS producers (NPC fire): ++ per fired round,
    // wraps at the ammo tracer_rate, tracer on wrap. Stands in for the original's
    // per-WEAPON-SLOT byte (weaponSlot+0x80) on rows without per-slot weapon
    // state: slot-less organic fire passes weaponSlot 0 [orig:
    // WeaponSlot_FireAndSpawnEffects @0x53F477 -> Entity_FireWeaponAndSendPacket
    // @0x42bd80], so one entity byte serves all four organic ammo ids
    // (AiProfile::OrganicWeapons) — the D-AI-8 (a) residual. The
    // local player's fire passes its ACTIVE slot's WeaponSlotState byte instead
    // (RoundSpawnParams::tracer_counter), so each weapon keeps its own phase
    // across switches. [orig: RoundData_SpawnRound @0x4ec199-0x4ec1bb]
    uint8_t tracer_shot_counter = 0;
    // Entity+341: ammo2ssn alternates the first two model userpoints with this
    // byte. The transient projectile class uses the same offset as bounce_count.
    // [orig: WacScript_EntityFireAtTarget @0x4F259E..0x4F25C3]
    uint8_t script_fire_userpoint_counter = 0;
    // Per-player replicated damage class, indexed by AmmoDef file index. C2S
    // loadout entry byte 4 writes it; 1 = x0.9, 2 = x1.1, other = x1.
    std::vector<uint8_t> ammo_damage_class;
    // Loadout-derived entity+44 bits 8 (armor) and 16 (parachute).
    // [orig: WeaponSlotTable_LoadAllFromDefs @0x541503..0x5415ba]
    uint32_t carry_flags = 0;
	std::string hud_image; // item definition sprite, read by the HUD
	// The shooter's hit-feedback latch, modelled as a serial each connection
	// consumes once. [orig: entity+0x2C |= 0x1000 @0x4E9962; test/clear
	//  @0x4FF7C5..0x4FF7D9]
	uint32_t hud_hit_feedback_serial = 0;
    int32_t health = 100;     // signed i16 retail storage carried sign-extended; <=0 -> dead
    // items.def hp (itemDef+0x17C healthMax), stamped by the host's item-traits sweep
    // (0 = unresolved). The original spawns entities at Health = healthMax
    // [orig: Entity_InitFromItemDef @0x49e550]; the sweep mirrors that by lifting health
    // to hp for entities still at their spawn default. Feeds the §5.10 field-17 tier
    // denominator and the §5.13 vehicle health word.
    int32_t health_max = 0;   // signed i16 retail storage carried sign-extended
    int16_t mana = 0;     // entity+288, ItemDef 'mana' [orig: @0x4B97D6]
    int16_t mana_max = 0; // ItemDef+382 signed word
    int32_t critical_hp = 0;  // itemDef+384 signed word [orig: WacCmd_SsnCritical @0x4F1BF0]

    // items.def radarsig/heatsig (def+0x178/+0x17A u16), stamped by the host's
    // item-traits sweep. The AI acquisition feed reads them as the candidate's
    // per-candidate engage-range caps: primary-FOV leg capped by radar_sig,
    // secondary by heat_sig — an unauthored (0) signature makes the entity
    // undetectable to the SM class walk, exactly like retail. [orig: the copy
    // entity+422 = def+376 / entity+420 = def+378 in Entity_InitFromModel
    // @0x40e136-0x40e15d; the uint16 cap reads @0x46723e/@0x467277]
    int32_t radar_sig = 0;    // u16 world-unit cap (primary FOV leg)
    int32_t heat_sig = 0;     // u16 world-unit cap (secondary FOV leg)
    // Raw items.def ItemDefAttrib dword (itemDef+84), stamped by the host trait
    // sweep. Combat keeps this value on the entity because damage targets are
    // not necessarily AI entities. In particular bit 0x40000000 is NoDie:
    // weapon damage may reduce health only as far as 1.
    // The kItemAttrib* constants below name the bits world/replication code reads
    // (same dword on ai.h's def_attrib profile mirror).
    uint32_t item_attrib = 0;
    uint32_t item_attrib2 = 0; // raw ItemDefAttrib2 dword (ItemDef+88)
    // The building-interior daylight transfer (ItemDef+0x218, the def's
    // light_transfer x 0.01), stamped by the item-traits sweep. The rain
    // ambient inside a pool-2 building scales by (transfer x 0.5 + 0.5)
    // [orig: Entity_UpdateInfantryPlayerBody @ 0x4b4747..0x4b490e].
    float light_transfer = 0.0f;
    int16_t reverb = 0; // ItemDef+432, zero inherits mission [orig: @0x4B5FC3]
    // Signed impact/KZ armor classes and vehicle occupant-reduction factors
    // from ItemDef +0x190/+0x192 and +0x188/+0x18C.
    int32_t armor_impact = 0; // signed i16 retail storage carried sign-extended
    int32_t armor_kz = 0;     // signed i16 retail storage carried sign-extended
    float damage_reduc_pp = 0.0f;
    float damage_reduc_max = 0.0f;
    // Effective uniform model scale in signed Q16.16. Zero is retail's sentinel
    // for the ordinary unscaled/rigid inverse. The shell binding resolves the
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
    // WAC SSNSpawn/GroupSpawn store the low signed word; 100+ is unlimited.
    // [orig: entity+0x35E; WacCmd_SsnSpawn @0x4F7A80]
    int16_t npc_respawns = 0;
    // Optional control-point link read by the org1 respawn/hidden gates.
    // [orig: entity+0x354; Entity_UpdateInfantryAI @0x4B99DB/0x4B9FA0]
    EntityHandle npc_respawn_zone;
    // entity+0x2AC. Organic helper = 63, vehicle AI cycles 0..15, and the PLAYER
    // body's think cadence: the org2 body update fires the plyr class callback
    // (event 0) whenever it is <= 0 and decrements it every tick; the callback
    // re-arms it to 64 on every event that passes its dead return (hit, blast,
    // think). [orig: Entity_UpdateInfantryPlayerBody @0x4b4bc9..0x4b4be9;
    // Entity_HandleDamageAndTriggerZones @0x407b5e / @0x407c71]
    // The same dword is a pool-1 VEHICLE BRAIN's think countdown: the pool-1
    // visit runs the class event callback (the brain machine) only while it is
    // <= 0 and decrements it every visit; the machine re-arms it to brain[7]
    // after each update and zeroes it on a committed transition; the class init
    // seeds the 0..15 stagger (World::vehicle_ai_spawn_phase). Item class
    // callbacks read the slot as `class_think_ticks` above.
    // [orig: Entity_UpdatePool1Slot @0x4B8E1B / @0x4B8EA0;
    //  EntityAI_ProcessVehicleStateMachine @0x458568 / @0x4585B4;
    //  Entity_InitVehicleAIFromDef @0x46891C..0x468945]
    int32_t spawn_phase = 0;
    // The kill-cause bits of the retail entity+0x2C dword (bits 8..11), latched at
    // HIT time and read at the death edge by GameEvent_PlayerDeath: 0x800 the
    // critical/head-zone hit [orig: Weapon_CalcImpactDamage @0x4ec994 / @0x4ec9c6],
    // 0x100 the same-projectile multi-kill [orig: Projectile_ProcessDamageOnTarget
    // @0x4e8169..0x4e816b], 0x400 the kind-1 melee applier (D-ITEM-6 residual, no
    // producer here). The plyr class callback clears bits 8..11 on every event
    // outside {1,3,4,5} (the 64-tick think and the blast) [orig: @0x407b4d..0x407b4f];
    // the consumer clears the bit it reports [orig: GameEvent_PlayerDeath @0x5171ca /
    // @0x5171e8 / @0x517206]. RoundDeath::event_flags snapshots the 0xF00 mask.
    // Bit 0x2000 (kCauseFlagScriptHold) is the WAC holdSSN latch, outside it.
    uint32_t cause_flags = 0;
    int32_t spawn_heading = 0; // BAM32, entity+0x330 [orig: @0x4B965C]
    uint32_t spawn_flags = 0;  // entity+0x334, excludes dead [orig: @0x4B9662]
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
    uint8_t radio_request = 0; // entity+885 [orig: @0x430C50]
    uint8_t radio_request_seconds = 0; // entity+886
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
    static constexpr uint32_t kMoveOrderJump = 0x20;      // held jump key [orig: g_inputFlags
                                                          //  0x1000 -> bit 5 @0x4df6fa; the
                                                          //  jump gate @0x4b7eaf]
    static constexpr uint32_t kMoveOrderLeanLeft = 0x40;  // [orig: lean ramp @0x4b7dbf]
    static constexpr uint32_t kMoveOrderLeanRight = 0x80; // [orig: lean ramp @0x4b7dd6]
    static constexpr uint32_t kMoveOrderProne = 0x100;    // stance bit 8 (see net_stance_bits below)
    static constexpr uint32_t kMoveOrderCrouch = 0x200;   // stance bit 9
    // The wire movement-INPUT byte (entity+0x12C low): the owning client uplinks it every frame
    // (§5.10 extended C2S 0x0C) and the host echoes it in that player's 0x0A compact record —
    // remote players are motor-driven from replicated input, NOT from an anim slot [orig: case-2
    // apply @0x4c11ec; consumers Entity_UpdateLightVehiclePhysics @0x483fe0 (leg @0x48496d),
    // check_bone_ground_contact @0x441ba4 (stance bits 8-9)]. Written by apply_player_intent for
    // remote peers; mirrored from the packed local input for the host's own player (bits 0-2 =
    // 8-way move_direction_index, bit 3 = moving [orig: Player_PackInputStateToEntity @0x4df68f]).
    uint8_t net_move_input = 0;
    // The upper look/turn bits never fit the uplink's movement byte. They
    // remain local inputs [orig: Player_PackInputStateToEntity @0x4df741].
    uint16_t local_view_input = 0;
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
	int8_t analog_throttle = 0; // local fourth axis +0x133; not carried by C2S 0x0C
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

    // --- destruction state (world/destruction.h; world-wac-ai-re §24) ---
    // Bound-sphere radius (entity+0 boundRadius), host-stamped from the placed
    // model's collision bounds (0 = unstamped; the damage sweeps substitute the
    // organic stand-in radius). Feeds the blast range tests and the kz fallback
    // radius. [orig: entity+0, read throughout the explosion sweep @ 0x4ead80]
    float bound_radius = 0.0f;
    // Kill-credit attacker (entity+0x178 lastAttacker): stamped by the damage
    // paths. WAC ammo2ssn also overwrites this word with its target (0x4F25D4).
    // The explosion sweep only fills an EMPTY slot with its resolved
    // attacker [orig: @ 0x4eb319/@ 0x4eb593; the dead-attacker chain walk
    // @ 0x4eae95..0x4eaece].
    EntityHandle last_attacker;
    // The blast center a queued explosion stamped before this entity died — the
    // section-debris launch origin (entity+0x80 savedLivePose reuse; only
    // destructible-class entities receive it) [orig: @ 0x4eb553-0x4eb569].
    Vec3 death_blast_center;
    // Per-intact-model glass userpoint one-shot state. Retail marks the exact
    // point broken before rolling its four presentation effects; the dynamic
    // bit bank keeps the state entity-local when one item type has many placed
    // instances. [orig: Terrain_SpawnEffectsAtUserPoint @0x5cee20]
    std::vector<uint64_t> broken_glass_point_bits;
    // The death tick (entity+0x1AC, first write wins) [orig:
    // Entity_ProcessDestructibleDeath @ 0x43fc0c / AI_TransitionToDestroyed_Vehicle].
    uint32_t death_tick = 0;
    // Squib callback/motor aliases of entity+0x270/+0x2A0..+0x30C.
    int32_t script_next_ssn = 0;
    struct SquibState {
        bool motor = false;
        int32_t ammo_index = 0; // signed word +0x160 (presentation)
        int32_t damage_ammo_index = 0; // dword +0x26C
        int32_t spread_q16 = 0;
        int32_t remaining = 0;
        int32_t interval = 0;
        uint32_t last_tick = 0;
        int32_t next_ssn = 0;
        FixedVec3 center, origin, step, direction;
    } squib;
    bool item_section_piece = false; // locally allocated class fragment
    bool palm_sections = false; // palm/psec model callback @0x53BF10
    int32_t palm_state = 0; // entity+0x270
    // The per-section damage bytes at entity+0x2BA+section. Retail indexes
    // them unchecked; this bank grows on first write (the caller bounds the
    // section by the def's int8 huskSubPartCount, the authority bound
    // @0x440722) so the thousands of pooled rows that never take section
    // damage carry no bytes, and an unwritten section reads zero.
    // [orig: Entity_UpdateSectionDamage @0x4406A0: the byte add @0x4406C7 /
    //  @0x44072B, the completion scan @0x440768]
    struct SectionDamageBank {
        std::vector<uint8_t> bytes;
        uint8_t &operator[](int section) {
            if (size_t(section) >= bytes.size()) bytes.resize(size_t(section) + 1);
            return bytes[size_t(section)];
        }
        uint8_t read(int section) const {
            return size_t(section) < bytes.size() ? bytes[size_t(section)] : uint8_t(0);
        }
    } item_section_damage;
    int32_t section_pitch_rate = 0; // +0x2C8; settle reuses it as distance
    int32_t section_pitch_accel = 0; // +0x2C4; settle reuses it as limit
    bool section_bounced = false; // +0x155
    int32_t palm_damage[2] = {}; // entity+0x274/+0x278
    int32_t collapse_step = 0; // bld2/cran currentSpeed (+0x29C), steps 0..64
    std::array<int32_t, 6> destroy_phases_q16{};
    double destroy_progress = 0.0;
    uint32_t last_state_sent_ms = 0; // entity+560; authority 0x26 send clock
    bool objective_death_scored = false;
    int32_t destroy_timer = 0; // entity+0x1B0 initial fade delay
    bool destroy_timer_initialized = false;
    // Hidden/dismembered skeletal sections (entity+0x134): a set bit removes
    // the matching ordinal bone from person collision and presentation.
    // Distinct from spawned_piece_mask at +0x138.
    uint32_t section_mask = 0;
    // Host-local marker for the pool-0 corpse clone created by the person
    // damage callback's dismemberment leg. The clone is an ordinary wire row
    // (0x0A compacts / 0x0C join batch / 0x18 self-heal, none carrying the
    // section mask — world-wac-ai-re §19.2); this flag only suppresses the
    // clone's own death scream and keeps host bookkeeping honest. net_id and
    // the names are cleared (D-AI-9 aliasing-safety divergence).
    bool dismemberment_piece = false;
    // Husk sections that left as death pieces (entity+0x138): the husk renders
    // and collides WITHOUT these sections. Bit 0 (the hull) never sets.
    // [orig: Entity_SpawnDeathPieces @ 0x493983]
    uint32_t spawned_piece_mask = 0;
    DeathMotionMode death_motion = DeathMotionMode::None;
    // The +0x1C4 callback is nullsub_28 while AINODEPATH is enabled.
    // Independent +0x1C8 brain events remain active. [orig: @0x43B22A]
    bool motor_suspended = false;
    // FIND_AND_USE's attachment is independent of the seated parentEntity.
    // [orig: Entity_FindAttachBone @0x4B9580; entity+0x184/+0x364]
    EntityHandle attach_parent;
    uint8_t attach_bone = 0;
	uint8_t death_effect_active[3] = {}; // four owned handles per bank
	bool death_effect_underwater = false;

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
	// BMS team_budget byte165 -> entity+356, distinct from spawn team +357.
	// [orig: Entity_SpawnFromBMSRecord @0x40E9F0]
	int8_t vehicle_spawn_team = 0;
	uint8_t vehicle_spawn_priority = 0; // marker+539 [orig: @0x529B40]
	// Resolved pcvehicle_spawnlist mask -> full items.def IDs (+100000).
	std::vector<int32_t> vehicle_spawn_ids;
	// The mask bit (the shared g_ItemGroups slot) each entry above resolved
	// from, parallel to vehicle_spawn_ids: the C2S 0x40 spawn pick names the
	// group by that bit [orig: NapiNPServerMsg_HandleVehicleSpawnRequest
	// @0x51C5C0 `(1 << typeIndex) & def+2772`, then g_ItemGroups[typeIndex]].
	std::vector<uint8_t> vehicle_spawn_groups;

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
    // BMS 6088 marker gen_string[31] -> entity+692 [orig: @0x40E9F0].
    std::string script_effect_name;

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
    // True only when the authored attachment row behind the parent/type pair
    // was identified exactly. A streamed 0x0D child still has an exact absolute
    // wire pose when this is false; parent/root fallback must not overwrite it.
    bool emplacement_pose_metadata_resolved = false;
    // The emplacement's embedded MountSlot (parent+0x2B4). A UseGun occupant borrows
    // this slot: the AI update only queues nextAction=FIRE, then the later global
    // weapon-action pump owns cadence/ammo and attributes the round to slot.owner.
    // [orig: Entity_AttachToUseGunSlot @0x546b80; WeaponAction_ProcessAllEntities
    //  @0x542690; WeaponAction_Fire @0x542b10]
    WeaponSlotState primary_weapon_slot;
    uint8_t primary_weapon_slot_adm = 0xFF;
    EntityHandle primary_weapon_owner;
    // The emplaced gun channel: the gun's yaw/pitch relative to this
    // emplacement's own frame, the two stored words retail keeps on the ewep
    // entity (+0x322 yaw / +0x324 pitch = the high words of the BAM32
    // deltas). The 'ewep' per-tick update refreshes them from the UseGun
    // occupant (IsTurret items slew them at most 0x92CF34/tick and pull the
    // gunner's own yaw back toward the turret), the per-update window clamp
    // pins them at the arc edge and writes the pinned look into the occupant,
    // and the model's EWEAP_GUNYAW/GUNPITCH CTRL pair publishes them as
    // stored. Never cleared on detach: an IsTurret turret stays where its last
    // gunner left it. Producer/consumer: world/mount_controls.h.
    // [orig: Entity_UpdateChildAttachment @0x4409A0 stores @0x440b23/@0x440b45
    //  (yaw) and @0x440b58 (pitch); Entity_UpdateTransformAndTurret @0x440ca0
    //  window write-back @0x44125c (yaw) / @0x4412a4 (pitch)]
    // The ewep class update's barrel accumulator and coast-down byte.
    // Its firing source is the SAME inline MountSlot kick byte (+0x30F).
    // [orig: Entity_UpdateTransformAndTurret @0x4413FE..0x441447]
    bool emplaced_update = false; // items.def move_function ewep
    // The render class row's CTRL callback (def+0x144): the 'ewep' row
    // installs the writer that publishes the stored gun words, the spin word
    // and the inline slot's heat at every render and every userpoint
    // transform of this entity, occupied or not (world/mount_controls.h).
    // [orig: render-class row 'ewep' @0x82CFA0 -> HUD_CacheWeaponSlotInfo
    //  @0x440930, installed by BoneCallback_LookupByTag @0x4E32ED..0x4E3306]
    bool emplaced_ctrl_publisher = false; // items.def render_function ewep
    uint8_t emplaced_spin_ticks = 0; // entity+0x31B
    uint16_t emplaced_spin_phase = 0; // entity+0x320, WEAP_SPIN
    int16_t emplaced_gun_yaw_word = 0;   // entity+0x322
    int16_t emplaced_gun_pitch_word = 0; // entity+0x324
    // The subobject (bone) the child's anchor userpoint rides — retail reads
    // it off the parent model's 48-byte userpoint record (+0x18, indexed by
    // the child's 1-based userpoint index entity+0x319 = emplacement_bone)
    // as the parent-brain publication gate: only a child sitting on the
    // parent ROOT (0) drives the parent's turret channel with its gun words.
    // -1 = not stamped (the publication stays off); the value is
    // ThreediUserPoint::subobject_index of the resolved anchor.
    // [orig: Entity_UpdateTransformAndTurret @0x440f1d..0x440f54]
    int16_t emplacement_anchor_subobject = -1;
    // The resolved weapon userpoint bytes (entity+0x327..0x332): four weapon
    // slots x {b fire origin, m flash anchor, c casing anchor}, each a 1-based
    // index into the model's userpoint table (0 = none). Resolved from the def's
    // twelve weap[lr][bmc]up[2] names when the entity's model attaches; the
    // fire tick reads them through weapon_userpoint_byte below.
    // [orig: Entity_InitBoneReferences @0x441470 (slot 0 <- weapr?up, 1 <-
    //  weapl?up, 2 <- weapr?up2, 3 <- weapl?up2) + the zero-fill
    //  Entity_ResolveBoneUserpoints @0x545940]
    uint8_t weapon_userpoint_bytes[4][3] = {};
    // The single tracked FIRST occupant (entity+368 occupantEntity): claimed at attach by
    // ctrlx/drvrx (empty-or-same) and UseGun (only when empty), never by sitex; cleared only
    // when THE claimant detaches — a remaining second controller does not inherit it. This
    // is the retail engine-running latch: the PlayerControl occupancy effect (and the engine
    // start/stop sounds) key off it, not off any-control-seat occupancy.
    // [orig: Entity_AttachToVehicleSlot @0x4946d0 writes +368 @0x4947d2/@0x4948d8/@0x49495e;
    //  Entity_DetachFromVehicle @0x4355f0 stop leg @0x4356e9..0x435759 + clear @0x43577c;
    //  spawner gate @0x48faad in Entity_UpdateHeloRotorSpin (ex entity_update_damage_accumulator_and_shadow) Entity_UpdateHeloRotorSpin @0x48fa70]
    // On a person the same word is the ride link: the same-team occupant of the
    // carrier it stands on, held by ride_link_hold below.
    EntityHandle primary_occupant;
    // A person's ride-link hold (+0x174): +4 per think that finds the link, up
    // to 0xF0; a think without it spends one tick, and the link clears once the
    // hold is spent. [orig: Entity_UpdateInfantryAI @0x4BD87E..0x4BD905;
    //  Entity_UpdateInfantryPlayerBody @0x4B5EA9..0x4B5F2C]
    int32_t ride_link_hold = 0;
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
    int16_t music_location = 0; // itemDef+434; containing-building WAC location

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
    // Entity+0x180: backup ground target for the queued Flags 0x200 mount.
    // WAC tele and carrier spawn write it; vehicle respawn clears it with
    // matching ground references. [orig: @0x4F2375, @0x50D45A, @0x43ED46;
    // org2 fallback read @0x4B4260]
    EntityHandle mount_toggle_fallback;

    // Carried-object link (entity+0x268 mountedChild): the object this entity
    // is CARRYING (a picked-up flag/carryable), distinct from the seat-mount
    // family above. Read by the GroupHoldingGroup/SingleHoldingGroup trigger
    // conditions against the held object's command group. Every witnessed
    // producer is the CTF/carryable pickup-drop-capture family or the savegame
    // restore (bms-event-runtime-re §3b item 4). Match owns the live flag
    // producer; the conditions and savegame consumer share this same link.
    // [orig: set
    // Entity_AttachToVehicle @0x43c130; cleared Entity_DropCarriedObject
    // @0x439df0, the capture-zone clear @0x4ada07, Entity_Destroy @0x43ea03]
    EntityHandle mounted_child;         // kInvalid = carrying nothing

    // The shooter's last claimed fire target (entity+104 -> +12): stamped per accepted
    // C2S 0x06 [orig: Server_ClientFiredRound @0x50c2ad stores the resolved target ptr],
    // read LIVE at 0x0A tag-2 serialize time — a set handle adds the wire 0x40 flag +
    // target word [orig: NetPacket_SerializeRoundEvent @0x50485a]. (D-NET-152)
    EntityHandle last_fire_target;      // kInvalid = no target claimed

    // Medic drag link: self identifies the backward-walking dragger; a corpse
    // references its dragger. Serial prevents a recycled pool slot inheriting it.
    // [orig: entity+0x350, HeliLift_UpdateSlotState @0x451730]
    EntityHandle dragger;
    uint64_t dragger_spawn_id = 0;
    // The "a medic is reviving me" latch (entity+0x1E0): set by the host's
    // revive transaction on the victim (and on a client by S2C 0x3A), read by
    // the medic interaction to refuse a second reviver and by the deploy
    // statics; cleared by the spawn-state reset.
    // [orig: GameEvent_RevivePlayer @0x517D4F; GameEvent_HandleMedicInteraction
    //  @0x4E67C4; Game_InitNewRound @0x422740]
    bool medic_reviving = false;

    // The mover-entry pose stamp riders consume — retail savedLivePose
    // (+0x80..+0x88) and the body* attitude triple (+0x8C..+0x94), stamped at
    // the START of the entity's own mover tick; the deck-ride reads
    // (current - saved) as the carrier's per-tick delta [orig: the org2 self
    // stamp @0x4b4190; the ride reads @0x4b530b../@0x4ba47f..]. Kept in the
    // ride's units (16.16 position / BAM32 attitude) so rider deltas are
    // exact. Never-stamped entities (statics, pre-first-tick rows) read as
    // zero delta via saved_live_valid.
    int32_t saved_live_pos[3] = {};
    int32_t saved_live_yaw = 0;
    int32_t saved_live_pitch = 0;
    int32_t saved_live_roll = 0;
    bool saved_live_valid = false;

    // --- vehicle motor state (pool-1 PlayerControl vehicles; world/vehicle_motor.h) ---
    // The original keeps this state across the entity struct and the 812-B per-entity
    // AI/physics component (`vehicleData` = *(entity+100)); the slot comments name the
    // original homes. [orig: Entity_UpdateVehiclePhysics @0x48af00]
    struct VehicleMotorState {
        int32_t yaw_bam = 0;          // 32-bit engine-frame heading (entity+0x10). Entity::yaw
                                      // (mission deg) mirrors (90 - bam/deg) each motor tick —
                                      // steering accumulates sub-degree BAM deltas.
        bool yaw_seeded = false;      // yaw_bam initialized from Entity::yaw on first tick
		// Spawn-relative pose (+0x24C), support (+0x264), and team (+0x165).
		// [orig: Game_StartMission @0x526003..0x52606B]
		int32_t spawn_pose[6] = {};
		EntityHandle spawn_support;
		uint8_t spawn_team = 0;
		bool spawn_pose_valid = false;
		bool respawn_waiting_for_overlay = false; // entity+0x2C bit 0, @0x52A110
		int32_t speed = 0; // currentSpeed, 16.16 u/tick [orig: entity+0x29C]
		int32_t speed_accel = 0;      // per-tick speed delta [orig: entity+0x2A0 speedAccel]
        int32_t cmd_speed = 0;        // commanded/target speed [orig: vehicleData+544]
        int32_t cmd_lateral_speed = 0; // commanded lateral speed [orig: vehicleData+540]
        int32_t steer_target_bam = 0; // steering target heading [orig: vehicleData+528]
        int32_t steer_ramp_bam = 0;   // key-steer ramp offset [orig: vehicleData+548]
        int32_t steer_state = 0;      // smoothed wheel deflection [orig: entity->aiState reuse]
        int32_t wheel_rate_bam = 0;   // grounded yaw rate [orig: entity->modelPtr0 reuse]
        int32_t vel_x = 0;            // world velocity, 16.16 u/tick — persists airborne
        int32_t vel_y = 0;            // (ballistic) [orig: entity velocityX/Y +0x98/+0x9C]
        int32_t slide_z = 0;          // vertical velocity, 16.16 [orig: slideDecay +0xA0]
		int32_t contact_direction[3] = {}; // +0x3BC..0x3C4: retained skid direction, Q16
		uint32_t slip_started_tick = 0; // +0x3F8: tire-slip recovery window
		int32_t wheel_slip_phase = 0; // +0x46C: signed wheelspin carry
		bool contact_solved_once = false; // +0x3CE: contact solver has run
		bool flare_latched = false; // brain+0x318 bit 2
		int32_t view_tilt_bam = 0; // +0x45C, helo GUNPITCH fallback and bike lean history
		uint16_t gear_phase = 0; // +0x470, automatic five-unit clearance servo
		bool movement_effects_disabled = false; // +0x44C
		bool damage_smoke_active = false; // emitter +0x1CC
		bool damage_fire_active = false; // emitter +0x400

		bool pivot_sound_latched = false; // brain+0x318 bit 5: stationary tank turn
		int32_t pivot_sound_prev_rate = 0; // brain+0x328: prior yaw rate, not slide_z
		int32_t tread_sound_accum = 0; // brain+0x320: even-tick travel sum (slot 45)
		int32_t tread_sound_prev_speed = 0; // brain+0x324: the previous even tick's speed
		bool skid_sound_latched = false; // brain+0x318 bit 3
		uint32_t rev_sound_ticks = 0; // brain+0x31C
		bool skid_effects_requested = false; // per-tick bone-effect producer
		uint32_t contact_wake_tick = 0; // entity+0x3B8: heavy neighbor wake
		bool collision_sound_latched = false; // brain+0x318 bit 4
		bool engine_sound_latched = false; // brain+0x318 bit 0: claimant start/stop
		bool light_sound_latched = false; // brain+0x318 bit 2: lights-on audio edge
		bool reverse_sound_latched = false; // movement-sound direction bit
											// [orig: vehicleData+0x318 bit 1]
		// brain+0x318 bits 6/7: the tank's hard and medium tumble cues. Bit 7
		// also gates the soft cue; the airborne tail clears both.
		// [orig: Entity_ApplyWheelSuspensionForces @0x4635D2 / @0x463607;
		//  Entity_ProcessWheeledVehiclePhysics `and byte ptr [eax+318h], 3Fh` @0x478AAE]
		bool tumble_hard_latched = false;
		bool tumble_med_latched = false;
		uint32_t sound_anchor_until_tick = 0; // keep residual lanes attached after claimant loss
        bool grounded = true;         // wheel contact / +0x2F2 [orig: BYTE2(entity->aiRef0) reuse];
                                      // vehicles spawn RESTING (contact resolved at init),
                                      // so the default is grounded — the first motor tick
                                      // re-derives it from the terrain clamp. The same byte
                                      // gates the tracked extend loop's pre-gate and is
                                      // cleared by Entity_RespawnVehicle @0x45FFFD.

        // --- Joiner-side vehicle prediction (net-re §5.38e, D-NET-196). The
        // wire record apply stages these and the family client mover chases +
        // predicts between records [orig: the @0x48D480 interp block + register
        // mirror; brain[177]/[179] = net-received speed/steer]. Only meaningful
        // when net_predicted (the joiner staged this vehicle).
        bool net_predicted = false;
        int32_t net_smooth_target[3] = {}; // staged wire target -> per-step vector
        int32_t net_smooth_heading = 0;    // staged heading -> per-step delta
        int16_t net_interp_progress = 0;   // [orig: +0x27C]
        int16_t net_interp_steps = 0;      // [orig: +0x27E]
        int32_t net_recv_speed = 0;        // [orig: brain[177]] 16.16, stale-decays
        int32_t net_recv_steer_bam = 0;    // [orig: brain[179]]
        int32_t net_recv_lat = 0;          // [orig: brain[178]] air lateral cmd, stale-decays
        int32_t net_alt_target = 0;        // [orig: brain[131]] absolute target Z, never decays
        bool net_engine_on = false;        // replicated Flags 0x80 (air engine/collective)
        // The AUTHORITY's climb-above-ground register [orig: brain[137] +0x224],
        // clamped at zero on every write; the engine flag IS `[548] != 0`
        // (@0x491dfd). The client path folds it into net_alt_target instead.
        int32_t net_climb = 0;
        // Authority AI flight: chel_ai_drive staged this tick's commands into
        // cmd_speed/cmd_lateral/steer_target/net_alt_target — the mover keeps
        // them instead of adopting net mirrors, then clears the flag. Retail
        // has no flag: the AI leg and the physics are ONE function.
        // [orig: the CHel AI leg inside Entity_UpdateAircraftPhysics @0x490310]
        bool ai_drive = false;
        int32_t air_pitch_bam = 0;         // live attitude the air mover integrates
        int32_t air_roll_bam = 0;
        int32_t air_pitch_rate = 0;        // [orig: modelPtr1 +0xA8]
        int32_t air_roll_rate = 0;         // [orig: modelPtr2 +0xAC]
        int32_t ground_cache = INT32_MIN;  // [orig: entity+0x2A4] 8th-tick terrain sample
        // [orig: brain[11] +0x2C] Z offset subtracted before EVERY aircraft
        // ground sample (@0x4903AD, @0x4909D0). Its value producer is
        // untraced (record §13 — "port as an opaque def/brain offset"), so
        // the register defaults 0 until the producer is witnessed; the
        // subtraction mechanism itself is structural.
        int32_t air_probe_z_off = 0;
        uint32_t net_seen_revision = 0;    // last consumed row compact_revision
        // --- Boat platform-solve state (vehicle-client-movers-re.md §3; client subset).
        int32_t plat_acc[4] = {};          // per-corner drop accumulators [orig: +0x2C4..+0x2D0]
                                           // — ALSO the ground spring leg's per-pad free-fall
                                           // sinks: the same four dwords (vehicle_suspension.h)
        float plat_bob_phase = 0.0f;       // heave-bob phase, radians [orig: +0x318 float]
        bool plat_at_rest = false;         // bob arm latch [orig: byte +0x364]
        bool plat_porpoise = false;        // bow-dip cycle latch [orig: byte +0x365]
        bool plat_planing = false;         // planing/bow-up bit [orig: +0x472 bit 1]
        bool plat_afloat = false;          // Flags 0x8000 mirror [orig: set @0x482CA5]
        bool plat_solve_valid = false;      // an earlier platform solve authored plat_afloat
		VehicleTrailState trails;
		uint16_t rotor_wash_handle = 0;
		int32_t plat_airborne_ticks = 0; // [orig: +0x3D4]
		// Light (cbik) solve: consecutive rear-wheel contact ticks — the
        // contact byte requires > 1, so a one-tick graze never grounds the
        // bike [orig: entity[1].pad_040[8]; ++ @0x47C154-analog in
        // Entity_ProcessLightVehiclePhysics, reset in the both-wheels-off
        // branch].
        int32_t light_rear_contact_ticks = 0;
        // --- Part-animation/fall accumulators (world/vehicle_part_anim.h).
        // The rotor spin machine's three dwords and the wheel phase; the PANM
        // registers HELO_ROTOR/HELO_TAILROTOR/VEHICLE_WHEELS sample their
        // HIGH words [orig: Entity_UpdatePartSpinAccumulator @0x4928B0 owns
        //  +0x460 speed / +0x464 angle / +0x468 rate; the wheel phase is
        //  +0x2B8; Entity_CacheVehicleHUDStats @0x4929B0 reads +0x466 /
        //  +0x2BA]. The crashed bike's chassis-orientation helper reuses
        //  +0x460 as its fall rate [orig: @0x468BFC/@0x468D67..0x468D7D].
        struct PartSpin {
            int32_t speed = 0; // +0x460
            int32_t angle = 0; // +0x464
            int32_t rate = 0;  // +0x468
        };
        PartSpin part_spin;
        int32_t wheel_phase = 0; // +0x2B8
		int32_t track_phase[2] = {}; // +0x2BC/+0x2C0, tank left/right tracks
		// --- Suspension spring leg (world/vehicle_suspension.cpp +
		// world/ground_conform.h). Per-wheel compression sinks, the four
		// oscillator blocks, the spring energy word, and the park latch bytes
		// [orig: +0x2D4..+0x2E0 compression; +0x304 + 0x18*i oscillators;
		//  +0x300 energy; +0x2EC parked latch / +0x2ED mover disable request
		//  / +0x2EE / +0x2EF; vehicle-client-movers-re.md §7.3].
		int32_t wheel_comp[6] = {};
		struct WheelOsc {
			int32_t amplitude = 0; // +0
            int32_t extension = 0; // +4
            int32_t energy = 0;    // +8
			int32_t impulse = 0; // +0x10, tank landing impulse
			float phase = 0.0f; // +0x14
		};
		WheelOsc wheel_osc[4];
        // DIAGNOSTIC ONLY (aiprobe "pd"): the ground solve's per-pad contact
        // depths from the last solve — probe instrumentation for the 00TRg
        // contact-flap hunt (AI-PARITY-CONCEPT §6.15). Not retail state; no
        // gameplay reader.
        int32_t dbg_pad_depth[4] = {};
		// One-tick wheel forces and the retained chassis rotation.
		// [orig: Entity_ComputeChassisOrientation @0x463940; +0x368 / +0x4E8..0x540]
		struct ChassisForce {
			int32_t direction[3] = {};
			int32_t rate = 0, scratch = 0;
		};
		ChassisForce chassis_forces[4];
		int32_t chassis_matrix[16] = { 4194304, 0, 0, 0, 0, 4194304, 0, 0, 0, 0, 4194304, 0, 0, 0,
			0, 4194304 };
		float chassis_quaternion[4] = {};
		bool chassis_active = false;
		uint32_t chassis_blend_tick = 0;
		int32_t chassis_blend_ticks = 0;
		bool chassis_contact_active = false; // +0x3DC
		int32_t chassis_impulse_direction[3] = {}; // +0x3EC..0x3F4, normalized Q16
		int32_t chassis_impulse_amplitude = 0; // +0x3FC
		int32_t spring_energy = 0; // +0x300 (the impact sink)
		// The crash latch bytes, named by retail offset (vehicle_suspension.h
        // documents each; Entity_RespawnVehicle @0x45FF40 is the one writer of
        // the whole set).
        uint8_t crashed = 0;         // +0x2EC — the CRASHED / TIPPED state
        uint8_t crash_request = 0;   // +0x2ED — the per-tick crash request
        uint8_t landing_2ee = 0;     // +0x2EE — the hard-landing marker
        uint8_t byte_2ef = 0;        // +0x2EF — zeroed at arming
        uint8_t settle_2f0 = 0;      // +0x2F0 — the wreck/settle latch
        uint8_t fresh_2f1 = 0;       // +0x2F1 — 1 after Entity_RespawnVehicle
                                     // (+0x2F2 is `grounded` above)
        uint8_t wreck_2fc = 0;       // +0x2FC — wreck-settled / bike fall latch
        uint8_t wheelie_request = 0; // +0x3DD, cleared at the light mover tail
        uint8_t wheelie_active = 0; // +0x3DE, retained through the launch
        int32_t bike_ground_contact_ticks = 0; // +0x2F4, both-wheel contact run
        int32_t bike_launch_direction[3] = {}; // +0x3E0..0x3E8, Q16
        uint32_t airborne_stamp_2f8 = 0; // +0x2F8 — the client crash window's stamp
        float susp_rate_pick = 0.0f; // the one-shot 1.75/1.25 disable-rate pick
        // The driverless stuck counter [orig: entity+0x148 moveTimer — ++ per
        // AI_CheckVehicleStuckState @0x465290 call, zeroed while occupied
        // (@0x48DFB9 boat / the air AI leg @0x491185)].
        int32_t stuck_ticks = 0;
        // The handbrake stop latch [orig: entity byte +0x3CD @0x48c03a..0x48c074]:
        // 1 while `occupant && Flags & 8 && itemDef->handBrake`, and the
        // command word is forced to zero for as long as it holds.
        uint8_t handbrake_latched = 0;
        // Retained direction used by the brake-mode freelook merge (+0x3C8).
        // Its only original writer is the unreferenced mover @0x48748B; live
        // ground/bike motors read it without replacing it.
        // [orig: Entity_UpdateVehiclePhysics @0x48B855..0x48B864]
        int32_t handbrake_direction = 0;
    };
    VehicleMotorState veh;
};

// The ONE home of the ItemDefAttrib stamp: both raw dwords plus the per-entity
// facts the engine derives from them at spawn (the items.def trait sweep,
// mission/item_traits.cpp) and again when a tool overrides one entity's words
// (EntityCommands::set_entity_item_attrib). Per-item caches keyed by item id
// (world.tables.item_death_traits, vehicle_traits) are the sweep's alone.
// [orig: Entity_InitFromItemDef @0x49e550 — the def+84/+88 copies; the AS zone
//  gates @0x4a2de0 / @0x4fe110 (ChangeTeam / SpawnPoint); the AIData gate
//  @0x433327; LeaveCorpse @0x4b9e54]
inline void stamp_item_attrib(Entity &e, uint32_t attrib, uint32_t attrib2) {
    e.item_attrib = attrib;
    e.item_attrib2 = attrib2;
    e.is_ai_capable = (attrib & kItemAttribAIData) != 0;
    e.is_capture_trigger = (attrib & kItemAttribChangeTeam) != 0;
    e.is_spawn_point = (attrib & kItemAttribSpawnPoint) != 0;
    e.leave_corpse = (attrib & kItemAttribLeaveCorpse) != 0;
}

// Entity_GetWeaponSlotByte: the resolved userpoint byte for one weapon slot and
// field (0 b/fire, 1 m/flash, 2 c/casing); 0 unless the def has weapon slots
// (ItemDefAttrib 0x20) — the person layout (attrib 0x40, entity+0x4D8) is not
// carried: person fire origins ride the organic launch bytes +0x365..+0x367
// (AiProfile::OrganicWeapons::launch).
// [orig: Entity_GetWeaponSlotByte @0x5459c0 (attrib test @0x5459d3, the
//  +0x327 cluster read @0x5459ef..0x5459f9)]
inline uint8_t weapon_userpoint_byte(const Entity &e, int slot, int field) {
    if ((e.item_attrib & kItemAttribEweap) == 0u || slot < 0 || slot > 3 ||
        field < 0 || field > 2)
        return 0;
    return e.weapon_userpoint_bytes[slot][field];
}

// The field a slot's action pair selects: the flash (1) while FIRE hands over
// to RECOIL, the casing (2) through RECOIL, else the fire origin (0).
// [orig: Entity_ComputeUserpointWorldTransform @0x545D17..0x545D3F]
inline int weapon_userpoint_field(const WeaponSlotState &slot) {
    if (slot.current == weapon_action::kFire)
        return slot.next == weapon_action::kRecoil ? 1 : 0;
    return slot.current == weapon_action::kRecoil ? 2 : 0;
}

} // namespace opennova::world
