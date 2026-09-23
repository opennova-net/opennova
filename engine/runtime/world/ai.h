// Entity AI subsystem — byte-exact port of the Jointops.exe infantry/vehicle AI
// brain (the third ticking system over the shared world, after WAC + BMS events).
//
// The data structures, the two class dispatchers and every row of the state
// table are ported. See docs/world/world-wac-ai-re.md for the full RE map.
//
// IDA anchors (Jointops.exe, imagebase 0x400000):
//   EntityAI_ProcessInfantryStateMachine @0x4581b0   (the AIR-class dispatcher: CHel/cpln)
//   EntityAI_ProcessVehicleStateMachine  @0x4583c0   (the ground dispatcher: cveh/cbot/ctrn)
//   AI_BeginUpdate                        @0x457b40   (movement controller row 4, phase limit 496)
//   AIEvent_QueueEntry                    @0x455da0   (1024 x 5-dword ring)
//   AIEvent_ProcessTimedEntries           @0x455df0   (timer -= 0.016/frame)
//   state-handler table                   @0x815238   (24 records x {enter,tick,exit,event})
//   Entity_LookupAIStateName              @0x455cc0   (the authoritative state enum)
//
// Container rebase (a structural translation; no behavior rides on it): the
// original keeps brains in the absolute global array unk_AED380 (812-byte stride),
// the profile at brain[1] and the per-entity controller at brain[2] as absolute
// pointers, and the handler dispatch in absolute function-pointer tables. We rebase
// those to containers (a vector of AiEntity, embedded profile/controller fields, a
// static StateRow table). Struct bodies are modeled as int32 f[N] + named indices so the
// ported handlers index fields exactly as the decompiler does (b.f[4], b.f[5], ...).
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <runtime/world/collision.h>
#include <runtime/world/entity.h>
#include <runtime/world/infantry.h>
#include <runtime/world/system.h>

namespace opennova::terrain {
struct TerrainHeightField;
}

namespace opennova::world {

class World;
struct VehicleDriveCmd;
struct VehicleTraits;

// ----------------------------------------------------------------------------
// AI state ids. [orig: Entity_LookupAIStateName @0x455cc0 string table.] States
// 0..5 are default/uninitialised (handlers are nullsubs); HELO_* are air units,
// GROUND_* are infantry + ground vehicles. ids 13 and 21 are transitional gaps.
// ----------------------------------------------------------------------------
enum AiState : int32_t {
    kAiDefault0 = 0, // 0..5 spawn/uninitialised, nullsub handlers
    kAiHeloLand = 6,
    kAiHeloFollowWp = 7,
    kAiHeloCombat = 8,
    kAiHeloHunt = 9,
    kAiHeloEvade = 10,
    kAiHeloFormation = 11,
    kAiHeloReturnToBase = 12,
    kAiHeloPretty = 14,
    kAiHeloDead = 15,
    kAiGroundFollowWp = 16,
    kAiGroundCombat = 17,
    kAiGroundEvade = 18,
    kAiGroundFormation = 19,
    kAiGroundReturnToBase = 20,
    kAiGroundPretty = 22,
    kAiGroundDead = 23,
    kAiStateCount = 24,
};

// Human-readable state name (returns "?" for the gaps/unknowns), faithful to
// Entity_LookupAIStateName @0x455cc0.
const char *ai_state_name(int32_t state);

// ----------------------------------------------------------------------------
// PLAYPARTANIM phase domain: one full sweep spans 0..0x10000 (16.16 1.0). The
// present ACTIVE code is live only while positive and within one phase. Retail
// stores only the channel direction and rate [orig: Entity_ApplyCommand case
// 0x22 @ 0x43B192]; its sweep integrator Entity_UpdateSuspensionBounce
// @ 0x456710 has no caller, pointer or thunk anywhere in the executable, so a
// live phase dword holds its value (the editor preview integrates through
// part_anim_step below).
// ----------------------------------------------------------------------------
inline constexpr int32_t kPartAnimPhaseOne = 0x10000;
constexpr bool part_anim_phase_active(int32_t code) {
    return code > 0 && code <= kPartAnimPhaseOne;
}

// ----------------------------------------------------------------------------
// AiBrain — entity+100 / unk_AED380, 812 bytes (203 dwords). Modeled as a raw
// dword array so handlers index it exactly like the decomp (`b.f[4]`).
// ----------------------------------------------------------------------------
struct AiBrain {
    int32_t f[203] = {};

    // Named dword indices (confirmed from the decomp; world-wac-ai-re §1/§4).
    enum Idx : int {
        kOwner = 0,        // back-ref to entity (nonzero = slot live)
        kCurState = 4,     // current AI state  [byte +16]
        kPendState = 5,    // pending state (applied when != kCurState) [byte +20]
        kFallback = 6,     // fallback state [byte +24]
        kStep = 7,         // think period / controller phase step (idle 16 / patrol 64) [byte +28]
        kFireTimer = 9,    // fire countdown (decrements by kStep) [byte +36]
        kTick = 10,        // ++ each SM update [byte +40]
        kModelFloor = 11,  // |intact model floor| from the class init [byte +44]
        kHuskFloor = 12,   // |husk floor| from the class init [byte +48]
        // ---- waypoint sub-struct (passed to AIWaypoint_UpdateTarget as brain+52) ----
        kWpType = 13,      // waypoint type: 1 nav-node, 3 literal coord [byte +52]
        kWpChannel = 14,   // nav/anim channel id (= navMeshId)        [byte +56]
        kWpNode = 15,      // current node index within the channel path [byte +60]
        kWpResolved = 16,  // resolved pool-3 node (orig: ptr; we store index) [byte +64]
        kWpCoordX = 17,    // type-3 literal target X [byte +68]
        kWpCoordY = 18,    // type-3 literal target Y [byte +72]
        kWpCoordZ = 19,    // type-3 literal target Z [byte +76]
        kWpCoordSrc = 20,  // type-3 -> copied into kWpNodeVal [byte +80]
        kWpBearing = 21,   // BAM bearing to target (atan2 * 2^32/2pi) [byte +84]
        kWpDistance = 22,  // approx Manhattan distance to target [byte +88]
        kWpNodeVal = 23,   // node payload f[0] (type1) / kWpCoordSrc (type3) [byte +92]
        kWpExtra = 24,     // node payload f[4] (type1) / 0 (type3) [byte +96]
        kAnimFlag = 32,    // cleared on node advance [byte +128]
        kStoredKeyTime = 35, // stored node-val on advance [byte +140]
        kPriorityTarget = 37, // priority target (orig: entity ptr; same packed+1 rebase as
                           // kTargetSlot). acquire's LOS-only bypass compares it per
                           // candidate [orig: brain+148 read @0x467350; restored from the
                           // savegame handle record+476 @0x45dbb3 — no live gameplay
                           // producer witnessed, see world-wac-ai-re §16.2a] [byte +148]
        kTargetSlot = 38,  // current target (orig: entity ptr; container rebase stores
                           // EntityHandle.packed + 1 so 0 keeps the orig null meaning) [byte +152]
        kDamageInfo = 39,  // damage source/info copied from a damage event's extra [byte +156]
        kCombatTimer = 40, // no-target / combat re-acquire timer (>620 re-acquire) [byte +160]
        kFireDelay = 41,   // fire-delay countdown set on engagement [byte +164]
        kRetargetTimer = 42, // += 16 per processed state-17 tick; >248 rescans [byte +168]
        kAccuracy = 43,    // scatter accuracy 0..5 (modulus 6 - acc) [byte +172]
        kDriveSkill = 44,  // commanded drive skill, clamped 0..4 [byte +176]
        kAlert = 46,       // alert level [byte +184]
        kPrevAlert = 47,   // previous alert level (edge) [byte +188]
        kNoTargetIdle = 48,// set 1 when no target + profile not combat [byte +192]
        kSpeedA = 49,      // move speed A [byte +196]
        kSpeedB = 50,      // move speed B (state 16 GROUND_FOLLOWWP) [byte +200]
        // ---- the state-17 tick working set (world-wac-ai-re §17.6) ----
        kTickAccum = 8,    // accumulates kStep; >=16 -> one processed tick [byte +32]
        kCooldownPair = 52,// PACKED u16 pair (weapon cooldowns A/B): += 0x10001*step per
                           // tick — one add advances both words, low-word carry included
                           // [orig: AIEntity_ProcessWeaponFire @0x472e00: brain[52] +=
                           //  0x10001 * brain[7] @0x472E42..0x472E48; bytes +208/+210]
        kAmmoA = 53,       // primary ammo count [byte +212]
        kAmmoB = 54,       // secondary ammo count [byte +216]
        kBoneCountA = 55,  // primary muzzle bone-list count [byte +220; list at +224]
        kBoneCountB = 72,  // secondary muzzle bone-list count [byte +288; list at +292]
        kLastWeapon = 106, // 1/2 = which weapon the continuation branches re-fire [byte +424]
        kBoneRoundRobin = 107, // seed<=0 muzzle-bone rotation counter; 0 -> -1 reseed,
                               // slot = ctr % count, then -- [orig: brain+0x1AC @0x4569F1]
        kUseWaypointZones = 108, // AIUSEWPZ/AICLEARWPZ command latch [byte +432]
        // ---- the turret solve blocks (Entity_ComputeWeaponFireTransform_0 @0x456980) ----
        // Active = the live turret pose the slew legs advance; staging = the fresh
        // solve. Layout (both): {hdist, ?, dist, yaw, pitch, ?} — yaw at +12/pitch
        // at +16 within the block; the CTRL-global writers read the yaw/pitch high
        // words [orig: staging @0x456D7B brain+0x1E4..0x1F8; active copy @0x456DDC
        // brain+0x1CC..0x1E0; AI_GetSuspensionFirePoint reads brain[118]].
        kActiveBlock = 115,  // brain+0x1CC..0x1E0 (yaw = f[118], pitch = f[119])
        kActiveYaw = 118,    // brain+0x1D8 — the slewed live turret yaw
        kActivePitch = 119,  // brain+0x1DC
        kStagingBlock = 121, // brain+0x1E4..0x1F8 (yaw = f[124], pitch = f[125])
        kSweepPhase = 180, // sweep-fire lateral phase, -196608..196608 step 10918 [byte +720]
        kBurstWindow = 181,// burst window: armed to 1 on fire, += step while <= 186 [byte +724]
        // Saved continuation solutions: out-transform minus entity pos/angles,
        // written at each mobile solve; the burst continuation re-fires them
        // without re-solving [orig: @0x4735F9..0x47363A primary +0x2D8, secondary
        // +0x2F0; §17.6].
        kSavedDeltaA = 182, // brain+0x2D8..0x2EC (pos xyz, yaw, pitch, roll deltas)
        kSavedDeltaB = 188, // brain+0x2F0..0x304
        kElevationBias = 197, // WEAPON_PITCHLOCKED commanded elevation, BAM32.
                              // [orig: brain+0x314; AI_HandleCommand case 22 writes
                              //  arg x 11930464 (deg->BAM) @0x4659EF; the solver's
                              //  flags&8 leg adds it to pitch @0x45706A]
        // ---- part-anim channels (vehicle/emplacement parts; PLAYPARTANIM, 2 channels) ----
        // [orig: Entity_ApplyCommand @0x43ab60 case 0x22 writes comp+436 (direction) /
        // comp+444 (rate). The phase pair (comp+452/+456) starts at the allocator's
        // zero fill (Entity_InitVehicleAI memset @0x460246); only the savegame
        // restore writes the channel block again (SaveFile_ApplyEntityRecord
        // @0x4AC030 -> Entity_CopyVehicleDefToAIComp: rate @0x45DDF9, phase
        // @0x45DE11); HUD_CacheEntityDisplayInfo @0x4A3E27 publishes it.]
        kPartAnimDir0 = 109,   // comp+436 channel-1 sweep direction (-1/0/+1)
        kPartAnimDir1 = 110,   // comp+440 channel-2 sweep direction
        kPartAnimRate0 = 111,  // comp+444 channel-1 rate (16.16 phase units / tick)
        kPartAnimRate1 = 112,  // comp+448 channel-2 rate
        kPartAnimPhase0 = 113, // comp+452 signed phase dword (ordinary range 0..0x10000)
        kPartAnimPhase1 = 114, // comp+456 channel-2 phase
        kTargetRef = 127,  // primary target ref [byte +508]
        kOutSpeed = 128,   // mover output speed [byte +512]
        kWorkPosX = 129,   // working target transform X [byte +516]
        kWorkPosY = 130,   // working target transform Y [byte +520]
        kWorkPosZ = 131,   // working target transform Z [byte +524]
        kWorkHeading = 132,// working target heading [byte +528]
        kWorkPitch = 133,  // working target pitch [byte +532]
        kWorkRoll = 134,   // working target roll [byte +536]
        kGuard = 144,      // guards kNoTargetIdle [byte +576] — the gunner-attachment COUNT:
                           // a vehicle carrying attached children never idles
        // ---- the gunner-attachment block (Entity_SetupGunnerAttachments @0x468100) ----
        // +576 = the attached-child count (kGuard above), then 16 pairs at +580+8*i:
        // pair[0] = the assigned 'agun' userpoint (retail: the model row pointer;
        // here index+1 into VehicleTraits::agun_points, 0 = none), pair[1] = the
        // child (retail: the entity pointer; here EntityHandle.packed + 1, the
        // kTargetSlot rebase). Read every tick by Entity_UpdateAttachedChildren
        // @0x45D550 and by the vehicle DYING enter's child-kill loop
        // @0x467B6E..0x467BBB. [orig: the walk stores @0x468168, count @0x468193,
        //  the bone stores @0x468389]
        kAttachCount = 144,
        kAttachSlots = 145, // +580; 16 x {bone, child} = f[145..176]
        kAttachSlotMax = 16,
        kBoneFlag = 196,   // bone-tracking flag (byte +784)
    };

    // Byte-addressed fields packed inside dword 196 [orig: the brain is
    // byte-addressed; +784 = the solver's bone slot | 0x80 (fire commit ORs
    // 0x40, give-up clears), +785 = the stationary weapons-free gate].
    enum ByteIdx : int {
        kBoneFlagByte = 784, // [orig: brain+0x310 @0x456A23 slot|0x80; @0x47306F |=0x40]
        kGuardFireByte = 785, // [orig: brain+0x311 — read @0x472EDE; written by the
                              //  WAC AI command (AI_HandleCommand case 21 @0x4659D2)
                              //  and the savegame restore @0x45DDBA; spawn default 0]
    };
    uint8_t *bytes() { return reinterpret_cast<uint8_t *>(f); }
    const uint8_t *bytes() const { return reinterpret_cast<const uint8_t *>(f); }

    int32_t cur_state() const { return f[kCurState]; }
    void set_pend(int32_t s) { f[kPendState] = s; }
};
static_assert(sizeof(AiBrain) == 812, "AiBrain must match unk_AED380 812-byte stride");

// AiSlot — entity+104 / unk_A34B90, 172 bytes (43 dwords). [orig: Entity_AllocateAISlot
// @0x40d2c0.] The reset helpers clear a movement flag byte at slot+136.
struct AiSlot {
    int32_t f[43] = {};
    uint8_t *bytes() { return reinterpret_cast<uint8_t *>(f); }
    const uint8_t *bytes() const { return reinterpret_cast<const uint8_t *>(f); }
    // Named dword indices (the behavior/accuracy controls and the
    // perception/attack ranges the infantry motor reads) plus the alert byte.
    enum Idx : int {
        kBehaviorFlags = 1, // BLIND 1 / COWARD 8 / BERSERK 0x200 / CLIMBER 0x400 [byte +4]
        kAimErrorPrimary = 10,   // 100 - authored accuracy 2 [byte +40]
        kAimErrorSecondary = 11, // 100 - authored accuracy 1 [byte +44]
        kAttackRange = 15, // max attack range [byte +60]
        kEngageMin = 16,   // minimum engagement range [byte +64]
        kSightRange = 17,  // perception/sight range [byte +68]
    };
    // Behavior-word bits the motor reads back. CLIMBER is ChangeAI sub 17's
    // write [orig: case 0x11 @0x43af36] and the ladder gate's third fresh-entry
    // qualifier [orig: aiRuntime+4 & 0x400 @0x4b326a] — its one retail reader.
    static constexpr uint32_t kClimber = 0x400;
    // The per-entity ALERT byte at controller+0x88 (0 green / 1 yellow / 2
    // red) — the state the SingleAtRed/YellowAlert triggers read and the
    // ChangeAI command family writes; the AI reset/patrol enters clear it
    // because they reset alert. Renamed 2026-08-13 from the kMoveFlagByte
    // misnomer with the writer census (bms-event-runtime-re §3b item 1).
    // [orig: read Entity_IsSsnAtAlertLevel @0x43e780; command writes
    //  Entity_ApplyCommand @0x43ac2d/=2, @0x43ac8d/=1, @0x43acfd/=0]
    enum { kAlertByte = 136 };
};
static_assert(sizeof(AiSlot) == 172, "AiSlot must match unk_A34B90 172-byte stride");

// AiProfile — brain[1], the read-only AI definition. Modeled minimally: only the
// flag bytes the state machine reads. [orig: AIProfile_LoadOrFind @0x45fd80.]
struct AiProfile {
    uint8_t flags96 = 0;   // +96: bit1 (&2) combat-capable, bit4 (&0x10) can-fire
    // +100 mode dword (modeled as the low byte): bit0 (&1) move-while-fighting, bit1 (&2)
    // use-fallback-state, bit2 (&4) hold-heading, bit3 (&8) ignore-refcount-saturation,
    // bit5 (&0x20) sweep fire, bit6 (&0x40) burst fire, bit7 (&0x80) stationary fire.
    // [orig: AIEntity_ProcessWeaponFire @0x472e00 mode dispatch; world-wac-ai-re §17.6]
    uint8_t flags100 = 0;
    int32_t field216 = 0;  // +216: added into brain working field [131]
	int32_t patrol_altitude = 0; // profile+204
	int32_t patrol_climb = 0; // profile+208
	int32_t min_agl = 0; // profile+232
	int32_t min_speed = 0; // profile+236
	int32_t min_chase = 0; // profile+184
	int32_t max_chase = 0; // profile+188
	uint32_t flight_flags = 0; // profile+192
	int32_t field220 = 0; // +220: copied into brain working field [138]
	// ---- combat / targeting (P2) ----
	int32_t view_fov_bam = 0; // profile+64, full precision for fire validation
	int32_t radar_fov_bam = 0; // profile+72
	int32_t view_dist = 0; // profile+68, 16.16
	int32_t field104 = 0; // +104: base fire delay (engagement); 0 -> no jitter (branch A)
	uint8_t fov_primary = 0;    // +75:  primary FOV arc byte (OR'd with 1 before use)
    uint8_t fov_secondary = 0;  // +67:  secondary FOV / turret arc byte (OR'd with 1)
    int16_t range_primary = 0;  // +78:  primary-FOV max engage range (world units, signed i16)
    int16_t range_secondary = 0;// +70:  secondary-FOV max engage range (world units, signed i16)
    // ---- the SM state-17 tick (vehicles/emplacements; world-wac-ai-re §17.6) ----
    int32_t fire_interval_a = 0;  // +124: primary fire interval (cooldown word +208 gate)
    int32_t fire_interval_b = 0;  // +156: secondary fire interval (word +210 gate)
    // The two .aip weapon def blocks the fire-transform solver reads
    // (profile+120 primary / +152 secondary; engine/formats/aip WeaponBlock).
    // ammo_index is the world.tables.ammo row resolved from the authored weapon NAME
    // at the item-traits sweep (-1 = unresolved -> the leg cannot fire), the
    // sibling of the organic seed (Entity_InitOrganicAI @0x4BFCC0). [orig: AIProfile_ParseProperty
    // "primary_weap" -> AmmoDef_LookupByName -> the byte store profile+148 @0x45EF80;
    // "secondary_weap" -> profile+180 @0x45F27E]
    struct WeaponFire {
        int32_t ammo_cap = 0;     // block+0: brain[53]/[54] spawn seed
        int32_t cone_bam = 0;     // block+8: solve cone half-angle (BAM32)
        uint32_t flags = 0;       // block+16: aip::kWeapon* mask
        int32_t facing_bam = 0;   // block+20: yaw bias
        int32_t pitch_bam = 0;    // block+24: pitch bias
        int32_t ammo_index = -1;  // resolved world.tables.ammo row for block+28's name
        std::string ammo_name;    // authored "*_weap" value, pre-resolution
        // The block+28 byte itself: AmmoDef_LookupByName returns 0 for a miss
        // and for the null first row, and the parser keeps only its low byte
        // [orig: AmmoDef_LookupByName @0x409870; AIProfile_ParseProperty @0x45EF80].
        uint8_t ammo_byte() const {
            return ammo_index > 0 ? static_cast<uint8_t>(ammo_index) : uint8_t(0);
        }
    };
    WeaponFire fire_a;
    WeaponFire fire_b;
    int32_t accuracy = 0;         // brain[43] seed: scatter modulus = 6 - accuracy (0..5)
                                  // [orig: the allocator copies profile+0x1C (aim_skill)
                                  //  into brain[43] @0x460294..0x460297]
    int32_t approach_cap = 0;     // +76: chase range cap (16.16)
    // ---- the infantry combat pass (org1 riflemen; world-wac-ai-re §17.4/§33.35) ----
    // The definition callback writes entity+0x358..0x35B as byte ammo IDs in
    // closeattack/easyrocket/advancedrocket/marker3 order (zero = none), each
    // resolved per field from its def name [orig: Entity_InitOrganicAI
    // @0x4BFCC0 -> AmmoDef_LookupByName @0x409870, calls @0x4BFE27..0x4BFE81].
    // The three one-based launch points are +0x365/+0x366/+0x367; both
    // rocket ammo slots use the middle point. Runtime storage is per body.
    struct OrganicWeapons {
        std::array<uint8_t, 4> ammo{};
        std::array<uint8_t, 3> launch{};
    } organic;
    int32_t clip_size = 0;        // items.def clipsize (magazine reseed)
    // Indices into world.sound_profiles (the def's sound_profile pair, resolved
    // at the host's item-traits sweep; -1 = unresolved -> the table's
    // "default" fallback at emit). Player character identity selects the
    // female variant; NPC defs keep their authored primary profile.
    // [orig: def+2148/+2152; CharacterEntity[12] select @0x52831c]
    int16_t sound_profile = -1;
    int16_t sound_profile_female = -1;
    // ---- the §16.2 class-driven pool walk (D-AI-1) ----
    // The four class-priority words and the derived walk order. An unresolved
    // profile keeps all-zero priorities — retail's memset-0 record — so its
    // brain acquires nothing; that IS the witnessed behavior for entities whose
    // .aip is missing. [orig: AIProfile_ParseProperty @0x45de70 writes
    // priority_air/ground/organics/decorations -> +80..+92; AIProfile_LoadOrFind
    // @0x45fd80 memsets the 248-byte record then sorts the four class ids by
    // their priority words (qsort CompareFunction @0x455d90 ascending, stored
    // REVERSED into +40..+52 @0x45fed9-0x45ff04 -> the walk runs
    // priority-descending; only type 1/2 profiles load the sort keys).]
    int32_t class_priority[4] = {}; // +80/+84/+88/+92: air/ground/organics/decorations
    int8_t slot_class[4] = {3, 2, 1, 0}; // +40..+52: class ids, priority-descending
	int32_t subtype = 0; // +20: STD 0 / BOAT 1 / PLANE 2 / TRAIN 3
	int32_t type = 0; // +16: HELO 1 / GROUND 2 / ORGANIC 3 (0 = unresolved)
	// The class init's brain[49]/[50] sources as the spawn resolved them (vehicle
	// init +0xC4/+0xC0, helicopter init +0xD4/+0xC8); the respawn re-run reloads
	// them over any command-set speed. [orig: Entity_InitVehicleAIFromDef
	// @0x4688C1..0x4688D3; Entity_InitHelicopterAIFromDef @0x468597..0x4685A9]
	int32_t class_speed_a = 0;
	int32_t class_speed_b = 0;
};


// ----------------------------------------------------------------------------
// Nav / anim node table. [orig: globals at 0xA71DD0 (Buffer) / 0xA71DD4
// (dword_A71DD4) / 0xA71DD8 (entryIndex) — three aliases into ONE array of
// 34-dword (136-byte) records.] Each record is a path/channel; entries[k] is a
// pool-3 (waypoint-marker) entry index resolved via Pool_GetEntryUnchecked(3,..).
// Tracked deviation: rebased from the three overlapping absolute globals to plain
// containers; values resolved are byte-identical.
// ----------------------------------------------------------------------------

// Pool-3 waypoint/nav marker entry. The mover/solver read f[1]/f[2]/f[3] as world
// X/Y/Z and f[0]/f[4] as node payload. (Real pool-3 stride may exceed 20 bytes; we
// only model the 5 dwords the AI touches.)
struct NavEntry {
    // {arrival_radius, x, y, z, facing}. A nav node in the original IS the pool-3 marker
    // entity; these mirror the fields the movers read: f[0] = arrival radius [orig: marker
    // dword[0], seeded from BMS wp_distance<<16, default 0x8000 @0x40e9f0], f[1..3] =
    // position, f[4] = facing [orig: marker+16 = spawn yaw]. The vehicle mover reads f[0]
    // as its advance threshold ("animTime") -- same field, same meaning.
    int32_t f[5] = {};
    // Marker hold time in ticks [orig: marker+328 = 62 * BMS u16@+62]; the infantry think
    // converts to think-ticks as (wait+8)>>4 on arrival. Extra field of our container
    // rebase (the original reads it straight off the marker entity).
    int32_t wait_ticks = 0;
};

// One channel record (34 dwords). loopflag bit0 set = terminate at path end (else
// wrap to node 0). count = the record's RAW node count: the loader never bounds it
// (CP19's list 6 counts 39). entries = the record's 32 raw slot words, the pool-3
// indices of its first 32 nodes and whatever the file carries past the count.
struct NavChannel {
    int32_t loopflag = 0;     // [orig: Buffer[34*ch]]        bit0 = one-shot
    int32_t count = 0;        // [orig: dword_A71DD4[34*ch]]  node count
    int32_t entries[32] = {}; // [orig: entryIndex[34*ch + k]] pool-3 node indices
};
static_assert(sizeof(NavChannel) == 136, "NavChannel must match the 34-dword stride");

// The channel table + pool-3 node store. Populated by mission->world promotion (P6).
class NavNodeTable {
public:
    std::vector<NavChannel> channels;
    std::vector<NavEntry> nodes; // pool 3

    const NavChannel *channel(int ch) const {
        if (ch < 0 || ch >= static_cast<int>(channels.size())) return nullptr;
        return &channels[ch];
    }
    // The marker a node index names, or null when no marker sits there (the
    // inspection views). The movers read slot() instead.
    const NavEntry *entry(int idx) const {
        if (idx < 0 || idx >= static_cast<int>(nodes.size())) return nullptr;
        return &nodes[idx];
    }
    // Node `node` of channel `ch`: the word at flat[34*ch + 2 + node] of the record
    // block, read the way the movers read it, with no bound on the node. A node at
    // or past 32 (a count over 32, or a start node past the count) reads on into the
    // following records' words; a word outside the block reads 0.
    // [orig: AIWaypoint_UpdateTarget `mov edx, Buffer+8[ecx*4]` @0x457476 with
    //  ecx = node + 34*ch; Entity_FindNearestTriggerByType @0x407F0B..0x407F6B]
    int32_t entry_index(int ch, int node) const {
        const int64_t flat = static_cast<int64_t>(ch) * 34 + 2 + node;
        if (flat < 0 || flat >= static_cast<int64_t>(channels.size()) * 34) return 0;
        const NavChannel &rec = channels[static_cast<size_t>(flat / 34)];
        const int word = static_cast<int>(flat % 34);
        return word == 0 ? rec.loopflag : word == 1 ? rec.count : rec.entries[word - 2];
    }
    // The pool-3 slot a node index resolves to, unchecked: a slot no marker filled
    // is a zeroed pool-3 entry (position 0, radius 0), never "no node".
    // [orig: Pool_GetEntryUnchecked @0x441FC0 over pool 3, which Pool_Clear
    //  @0x442060 zeroes at every mission reset]
    const NavEntry &slot(int idx) const {
        static const NavEntry kZeroSlot{};
        if (idx < 0 || idx >= static_cast<int>(nodes.size())) return kZeroSlot;
        return nodes[static_cast<size_t>(idx)];
    }
};

// One AI-controlled entity's complete brain state. (The original splits brain /
// slot / profile across three arrays; we colocate, keyed by the world handle.)
// The geometry fields mirror the original entity record offsets the AI reads/writes
// (faithful units: 32-bit fixed-point position, 32-bit binary-angle heading).
struct AiEntity {
    EntityHandle handle;
    AiBrain brain;
    AiSlot slot;
    AiProfile profile;
    bool has_physics = true;   // entity+368 present
    int32_t pos[3] = {};       // entity+4/+8/+12 (position X/Y/Z, 32-bit fixed)
    int32_t heading = 0;       // entity+16 (32-bit binary angle); copied to brain[132]
    int32_t pitch = 0;         // entity+20
    int32_t roll = 0;          // entity+24
    // The slope-conform body pitch (entity+0x90): the slope pass chases it toward the
    // fore-aft ground slope for conforming (prone-family / flagged / dead) bodies and
    // decays it to level otherwise; the §14 overlay body-pitch term reads it.
    // [orig: org1 @0x4ba320 / org2 @0x4b6fc1-0x4b6fda]
    int32_t body_pitch = 0;
    // The entity DEFINITION's attrib dword (def = entity+0x20, attrib at def+0x54).
    // Bit 0x200 forces slope-conform in the slope pass. JO infantry defs leave it
    // clear; host wiring is deferred until a consumer needs the other bits.
    // [orig: selector @0x4ba10f / @0x4b6d99]
    uint32_t def_attrib = 0;

    // --- network receive-apply: a remote peer's pose, read-applied on the host ---
    // The host stages a joiner's reported 0x0C pose into these engine-frame slots
    // [orig: NetPacket_SerializePlayerState case 4 @0x4c2042-0x4c20a9]. Retail's
    // entity+0x24 bit 0 only selects the conditional hard snap at @0x4C207E; it
    // is not an all-remote classifier. `net_is_remote_peer` is OpenNova's explicit
    // snapshot-ownership fact: it skips locomotion but retains the authority org2
    // animation/collision tail. net_smooth_target/heading/pitch are staged
    // for the CLIENT-side interpolation smoothing (motor fall-through @0x4b9a8c, is_authority==0)
    // — a deferred, client-only concern; the authority host never interpolates. NEVER set for
    // the host's own player (ADR-0012 amendment / §5.38).
    bool    net_is_remote_peer = false;
    int32_t net_smooth_target[3] = {};   // entity+0x234/+0x238/+0x23C (16.16 world)
    int32_t net_smooth_heading = 0;      // entity+0x240 (BAM32; also mirrored to heading/+0x10)
    int32_t net_smooth_pitch = 0;        // entity+0x244 (BAM32; also mirrored to pitch/+0x14)
    int16_t net_interp_progress = 0;     // entity+0x27C (reset to 0 on each read-apply)
    // Deferred client-interp bookkeeping (consumed by the @0x4b9a8c fall-through only):
    int32_t net_saved_live_pose[3] = {}; // entity+0x80/+0x84/+0x88 (interp delta basis)
    int16_t net_interp_steps = 0;        // entity+0x27E (2..16; buckets {3,4,5,8,16})

    int32_t vel_x = 0;         // entity+152 (velocityX)
    int32_t vel_z = 0;         // entity+160 (slideDecay, the vertical velocity); the retail
                               // +152/+156/+160 triple is InfantryState::vel[3]
    int16_t health = 100;      // entity+286 (<=0 -> death path)
    int32_t net_id = 0;        // entity+124 (RelationMatrix_SetBitA key / DcbId)
    uint16_t relmat_id = 0;    // entity+284 (RelationMatrix_SetBitB key)
    uint8_t team = 0;          // entity+354 (team id; 0 = neutral)
    // brain[2] is this entity's movement controller. Its +16 phase survives
    // world ticks and advances only through the selected movement callback.
    // [orig: AI_BeginUpdate @0x457B40; world-wac-ai-re.md (D-AI-14)]
    int32_t patrol_f0 = 0;     // scheduler +0
    int32_t patrol_delta = 0;  // scheduler +4 (patrol heading delta)
	int32_t aircraft_controller = 0; // controller+0: movement callback ID
	int32_t aircraft_result = 0; // controller+12
	int32_t aircraft_phase = 0; // controller+16
	int32_t aircraft_side = 0; // controller+20
	int32_t patrol_goal = 0; // scheduler +8 (patrol goal active -> still en route)

	// Infantry motor state (org1-class soldiers). When inf.active the entity is driven
    // by AiSystem::tick_infantry [orig: Entity_UpdateInfantryAI @0x4b9910] instead of the
    // vehicle state machine; promote routes BMS organics here. See world/infantry.h.
    InfantryState inf;

    // Per-entity collision-resolver state (prev-position gating + the idle skip
    // throttle). [orig: entity savedLivePose + pad_370[3]; collision.h]
    CollisionWorld::ResolveState collide_state;
};

// A resolved AI target — the fields the engagement bookkeeping reads off the target entity.
// [orig: best_target in AI_HandleEvent_HelicopterCombatD @0x467730.]
struct AiTarget {
    int32_t relmat_id = 0;       // target+284 (pad6_pre[24], i16) — relation-matrix key
    int32_t net_id = 0;          // target+124 (DcbId)
    bool has_brain = false;      // target+0x64 (the SM brain) nonzero -> jitter branch A vs B
    EntityHandle handle;         // container rebase: the world handle (orig: the entity ptr)
};

// A perception candidate for acquire_target. [orig: AI_FindBestTargetB @0x466f60. The live
// feed (acquire_target) runs the witnessed class-driven pool walk; the scoring core
// (acquire_target_from) runs the per-candidate perception gates + FOV/range/stealth scoring
// + LOS + priority bypass over an explicit list — the tests' injection seam.]
struct AiCandidate {
    EntityHandle handle;
    int32_t pos[3] = {};        // candidate+4/+8/+12 (X/Y/Z, 16.16 fixed)
    uint8_t team = 0;           // candidate+354
    int32_t flags = 0;          // candidate[9] (+36): &2 ignore, &0x4000 priority, &0x8000000 skip
    int16_t health = 100;       // candidate+286 (*143)
    int32_t visibility = 0;     // candidate+530 (*265): stealth counter (0 = fully visible)
    int32_t range_primary = 0;  // candidate+422 (*211): max primary-FOV engage range
    int32_t range_secondary = 0;// candidate+420 (*210): max secondary-FOV engage range
    int32_t relmat_id = 0;      // candidate+284
    int32_t net_id = 0;         // candidate+124 (DcbId)
    bool has_brain = false;     // candidate+0x64: carries an SM brain (persons carry none)
    bool is_priority = false;   // == brain+148 priority target -> LOS-only bypass
    // Preset LOS verdict for injected lists (default: clear). The live feed passes a
    // lazy evaluator instead — LOS runs only for a would-be best / the priority
    // bypass, never per candidate [orig: Entity_CheckMutualLineOfSight @0x539be0
    // called only at 0x467363/0x46738b].
    bool los_blocked = false;
};

class AiSystem; // fwd

// AIEvent ring entry: 5 dwords. [orig: dword_AE0778 ring, AIEvent_QueueEntry.]
//   f[0]=type, f[1]=(channel:lo16 | entity_index:hi16), f[2]=timer(float bits),
//   f[3]=extra, f[4]=unused(caller fills 4, QueueEntry copies 5).
struct AiEventEntry {
    int32_t f[5] = {};
    int32_t type() const { return f[0]; }
    int32_t channel() const { return f[1] & 0xFFFF; }
    int32_t entity_index() const { return (f[1] >> 16) & 0xFFFF; }
    float timer() const { float v; std::memcpy(&v, &f[2], 4); return v; }
    void set_timer(float v) { std::memcpy(&f[2], &v, 4); }
};

// Fixed circular AIEvent queue. [orig: dword_AE0778 (base) + dword_AE5778 (count),
// max 1024; AIEvent_QueueEntry @0x455da0 / AIEvent_ProcessTimedEntries @0x455df0.]
class AiEventQueue {
public:
    static constexpr int kMax = 1024;
    static constexpr float kFrameDt = 0.016000001f; // [orig: timer -= 0.016 per frame]

    // [orig: AIEvent_QueueEntry] copies 5 dwords if count<1024.
    void queue(const AiEventEntry &e);

    // [orig: AIEvent_ProcessTimedEntries] decrement timers; on expiry dispatch the
    // state's event handler + apply any pending transition; compact swap-with-last.
    void process_timed(AiSystem &sys, World &world);

    int count() const { return count_; }
    const AiEventEntry &at(int i) const { return buf_[i]; }
    void clear() { count_ = 0; }

private:
    std::array<AiEventEntry, kMax> buf_{};
    int count_ = 0;
};

// Per-handler call context. The original passes the raw entity pointer; we pass the
// resolved AiEntity + world + (for event handlers) the firing AIEvent entry.
struct AiThinkCtx {
    AiSystem *sys = nullptr;
    AiEntity *self = nullptr;
    World *world = nullptr;
    const AiEventEntry *event = nullptr; // non-null only in event-handler dispatch
};

using AiHandler = void (*)(AiThinkCtx &);

// One state's 16-byte record: {enter, tick, exit, event}. [orig: off_815238/3C/40/44.]
struct StateRow {
    AiHandler enter;
    AiHandler tick;
    AiHandler exit;
    AiHandler event;
};

// [orig: AIWaypoint_UpdateTarget @0x457380] Resolve the brain's current waypoint
// target, writing bearing (kWpBearing, BAM), distance (kWpDistance), and node
// payload into the waypoint sub-struct. `pos` is the entity's X/Y/Z (entity+4/+8/+12).
// Returns 0 on success / type-2 / type-3; returns -1 when a type-1 nav target can't
// be resolved (channel 0, missing count) or the type is unknown.
int ai_waypoint_update_target(AiBrain &b, const int32_t pos[3], const NavNodeTable &nav);

// The spawn-anchor proximity test [orig: Entity_IsBoneInProximity @0x434F90 —
// the "bone" is the entity's own +0x24C spawn pose; a spawn PARENT (+0x264)
// lifts it to world first and a dead parent fails the test]: planar deltas
// full, the Z delta HALVED, 3D length <= 8 u (0x80000). Consumers: the minAI
// crew clamp and Entity_CanEnterVehicle's at-spawn arm. Body in ai_waypoints.cpp.
bool vehicle_at_spawn_anchor(const World &world, const Entity &veh);

// The rider count [orig: Entity_CountMountedEntities @0x435970 — live pool-0
// entities with an ItemDef whose ground link (+0x28) is the vehicle, or whose
// ground link's own ground link is (a body on a carried gun)]. Body in
// ai_waypoints.cpp.
int count_mounted_entities(const World &world, const Entity &veh);

// A PLAYER driver whose head is under the water plane hands the boat to the
// AI leg [orig: Entity_UpdateWatercraftPhysics @0x48DFD3..0x48DFDF —
// `occupant->Position.z + CameraOffset.z <= Env_WaterHeightFixed`]. Body in
// ai_waypoints.cpp.
bool watercraft_driver_submerged(const World &world, const Entity &occ);

// [orig: Entity_ApplyCommand @0x43ab60] Apply the command arms which write the
// 812-byte brain directly: AIUSEWPZ/AICLEARWPZ (subs 0x20/0x21) and
// PLAYPARTANIM (sub 0x22). Controller-slot/entity-flag mutations and commands
// which queue AIEvent 6..11/21/22 live at the EntityCommands seam so they retain
// the original two-stage dispatcher. PLAYPARTANIM slots are
// p2=channel(1/2), p3=play_type(-1/0/+1), p4=time(16.16 seconds).
void ai_apply_command(AiBrain &comp, int sub_type, int32_t p2, int32_t p3, int32_t p4);

// The PLAYPARTANIM rate from ANIMTIME seconds: (0.016f / seconds) * 65536
// phase-units/tick with the x87-ftol integer-indefinite on inf/NaN/overflow
// (zero seconds -> INT_MIN, which the min-1 guard deliberately does NOT
// promote) [orig: Entity_ApplyCommand @0x43B1A9..0x43B1F9]. Shared by
// ai_apply_command case 0x22 and the editor-preview integrator binding.
int32_t part_anim_rate_from_seconds(double seconds);

// One 16 ms part-anim sweep step: wrapping signed ADD for direction +1,
// wrapping SUB for every other nonzero direction; only a STRICT upper (>
// 0x10000) or negative overshoot clamps — landing exactly on an endpoint
// stays active. Returns true when the sweep finished (the caller clears its
// direction) [orig: Entity_UpdateSuspensionBounce @0x456740..0x4567A9, an
// unreferenced routine]. Only the editor-preview binding steps with it; the
// world runtime never integrates the brain phases.
bool part_anim_step(int32_t &phase, int32_t dir, int32_t rate);

// [orig: Entity_CalcAverageGroundHeight @0x457230] The two height offsets the
// caller's brain carries: brain[11] (+0x2C, the class init's CMDL floor) when
// alive, brain[12] (+0x30) when dead, both skipped for a brainless caller
// [orig: `add eax,[ebx+30h]` @0x45734D, `add eax,[ebx+2Ch]` @0x457367, the brain
// test @0x457333]. The stand clearance the movers add on top (brain[131] =
// ground + 0x50000) is NOT here; each mover adds its own [orig:
// AI_ProcessMovementStep @0x466db0].
struct GroundClearance {
    int32_t alive_offset = 0;  // [orig: brain+0x2C] added to ground when alive
    int32_t dead_offset = 0;   // [orig: brain+0x30] added when dead
    bool use_dead = false;     // [orig: (Flags & 2 || health <= 0) && entity+0x34,
                               //  @0x457337..0x45734B]
    bool has_physics = false;  // [orig: entity+0x170 != 0 @0x45731E] enables the
                               //  worldY water clamp
};

// [orig: Entity_CalcAverageGroundHeight @0x457230] 5-tap weighted ground height,
// 16.16 fixed. `pos` is the entity X/Y/Z (16.16); `sample_radius` is 16.16 (the AI
// movers pass 0x50000 = 5.0). Samples the bilinear terrain column at center + N/S/E/W:
//   result = (N + S + E + W + 2*(C + 2*max)) / 10, clamped >= C, then the worldY water
//   clamp (when has_physics) and the def alive/dead offset.
// Tracked deviation: the original per-tap sampler is the hi-res down-raycast
// raycast_entity_collision -> Terrain_RaycastHeightmapHiRes_0 @0x60e710; we use the
// renderer-accurate bilinear column height (the near-vertical raycast's result),
// deferring the sub-cell along-ray refinement. Returns INT32_MIN when the field is
// invalid (faithful "no terrain" fallback — leave the entity's Z untouched).
int32_t calc_average_ground_height(const terrain::TerrainHeightField &field,
                                   const int32_t pos[3], int32_t sample_radius,
                                   const GroundClearance &clearance);

// The brain height offset every AI ground sample ends with: a vehicle brain
// adds its husk floor brain[12] when the entity is dead (Flags & 2 or health
// <= 0) and carries a first husk model (entity+0x34), else its intact floor
// brain[11]; a caller without a vehicle brain (an organic, or no AI) adds
// nothing. [orig: Entity_CalcAverageGroundHeight @0x457333..0x457367 and its
// inline copies AI_InitDeathState @0x4576E8..0x457705, AI_InitGroundHeight
// @0x45780D..0x45782C, AI_CheckLethalDamage @0x457950..0x45796D,
// AI_TransitionToDeath_Vehicle @0x46691B..0x466939 and @0x4669CE..0x4669EC,
// Entity_ProcessVehicleDestruction @0x466AEE..0x466B0D]
int32_t brain_ground_offset(const World &world, const Entity &entity);

// A recorded RelationMatrix_SetBitA/B side effect (net-replication bookkeeping the
// mover emits per node advance). The live TriggerRelations matrices are updated alongside
// this diagnostic trace by the route-arrival seam.
struct RelMatCall {
    int which = 0;       // 0 = SetBitA (net_id key), 1 = SetBitB (relmat_id key)
    int32_t key = 0;     // entity+124 (A) or entity+284 (B)
    int32_t channel = 0;
    int32_t node = 0;
};

// The 8 relation-matrix bit-set ops emitted on target engagement, in call order.
// [orig: AI_HandleEvent_HelicopterCombatD @0x4677b3..0x4678b2.] The matrices live in the
// net/relation layer; we record the calls (op + the two keys) rather than apply them.
enum RelOp {
    kRelEventSpecial = 0, // EventMatrix_SetSpecialBit   @0x452a40 (self relmat, target relmat)
    kRelSharedMem = 1,    // SharedMem_Init              @0x452ad0 (self net,    target relmat)
    kRelProximity = 2,    // EntityMatrix_SetProximityBit@0x452b60 (self relmat, target net)
    kRelEnemy = 3,        // TeamMatrix_SetEnemy         @0x452bf0 (self net,    target net)
    kRelAllied = 4,       // TeamMatrix_SetAllied        @0x452aa0 (self relmat, target relmat)
    kRel452B30 = 5,       // sub_452B30                  @0x452b30 (self net,    target relmat)
    kRelDamaged = 6,      // EntityMatrix_SetDamagedBit  @0x452bc0 (self relmat, target net)
    kRelSpotted = 7,      // TeamMatrix_SetSpottedBy     @0x452c70 (self net,    target net)
};
struct RelOpCall { int op; int32_t a; int32_t b; };

// [orig: AI_FindBestTargetB @0x466f60 scoring core] Combined FOV/range/stealth/priority score
// for a candidate (16.16 fixed point, 64-bit intermediates, +0x8000 rounding). Returns -1 when
// the candidate is outside both FOV/range gates (the secondary gate @0x467256..0x467280 skips
// to the next candidate, loc_467097), distinct from a
// legitimate in-gate score of 0 (a fully-stealthed target), which the caller needs to honor the
// priority-bypass ordering. `angle_diff` is the folded BAM heading delta in [0,128]; `distance`
// is ftol2(dist3d) >> 16 (world units). primary/secondary_fov are the profile arc bytes already
// OR'd with 1; the *_max args are the engage-range caps.
int32_t ai_score_target(int angle_diff, int distance, int primary_fov, int secondary_fov,
                        int primary_max, int secondary_max, int cand_primary_max,
                        int cand_secondary_max, int visibility, int cand_flags);

// The AI subsystem: every brain and the organic bodies on the shared world.
// World::update_all_entities drives it row by row, inside the one entity pass
// (the pool-1 visits think the brains, the pool-0 walk runs the bodies).
class AiSystem {
public:
    AiSystem();

    // Re-seed every brain to the captured spawn baseline + clear the transient queues.
    // [Drives World::restore: load_systems() calls it on Play->Stop, so the AI
    // rewinds alongside the registry/vars/env the World snapshot restores. No-op until
    // capture_spawn_baseline() has run.]
    void on_load(World &world);

    // The damage chain's inline reaction stamps, drained from this tick's
    // processed hits: an NPC body gets the alert byte, its group alert and
    // wasHit / damageTimer / lastAttacker; a state-machine brain gets a
    // queued damage AIEvent. The entity pass runs it right after the
    // projectile and explosion legs, so the pool-0 walk of the same tick sees
    // the stamps. [orig: the damage callbacks inside Weapon_UpdateAllProjectiles
    //  and Projectile_ProcessExplosionQueue (Entity_UpdateAllEntities @0x4C223A /
    //  @0x4C223F): Entity_HandleDamageTrigger @0x4073c8..0x4073ea,
    //  Entity_OnDamageReceived @0x4af859..0x4af878,
    //  Projectile_ProcessDamageOnTarget @0x4e7fb0]
    void apply_round_hits(World &world);
    // A brain row's class event callback on its think visit: the vehicle-class
    // machine for a cveh/cbot/ctrn row, the air-class machine for a CHel/cpln
    // row. Any other class row's fn1 is not a brain machine, so a brain whose
    // row names neither (or that has no traits row) does not think.
    // [orig: Entity_UpdatePool1Slot @0x4B8E3C (`call [esi+1C8h]`);
    //  g_EntityClassEventCallbackTable @0x813000 rows @0x8132a0 CHel /
    //  @0x813378 cveh / @0x813390 cbot / @0x8133a8 cpln / @0x8133c0 ctrn,
    //  resolved by EntityDef_InitAllCallbacks @0x4a5aae]
    void think_brain(AiEntity &e, World &world);
    // One pool-0 row's +0x1C4 body update: the org1 motor (or the joiner's
    // presentation leg for a wire-owned peer).
    // [orig: Entity_UpdateAllEntities @0x4C2460..0x4C2474 (`call eax`);
    //  g_EntityClassPhysicsTable row "org1" -> Entity_UpdateInfantryAI @0x4b9910]
    void update_organic(AiEntity &e, World &world, uint32_t logic_tick);

    // Capture the current AI state as the restore baseline. The embedder calls this once at
    // play start (after promote + the pre-mission pass), when it snapshots the World.
    void capture_spawn_baseline();

    // Attach a brain to a world entity; returns its AI index (faithful to the
    // unk_AED380 array index used by AIEvent entity_index).
    int attach(EntityHandle h);
    // Attach the synthetic half-corpse produced by dismemberment. The body
    // keeps the source pose/AnimMap state and corpse motor, while all active
    // combat/network inputs are cleared. impulse_q16 is round velocity >> 8,
    // horizontal only — the original kicks the clone's X/Y velocity pair and
    // leaves Z alone [orig: @0x4076b7/@0x4076c9].
    int attach_dismemberment_piece(EntityHandle h, const AiEntity &source,
                                   const int32_t impulse_q16[3]);
    // Free the brain bound to h: the slot is zeroed in place (index-stable, so
    // queued AIEvent entity indexes never re-point) and its owner word cleared,
    // which is exactly what Entity_Destroy does to the AI component
    // (memset(brain, 0, 812) + entity+100 = 0 @0x43e995..0x43e99d; the AiSlot
    // memset @0x43e9ae); the next attach reuses the lowest free slot the way
    // Entity_InitVehicleAI's owner scan does (@0x460204..0x460222). No brain
    // for h is a no-op. Every registry despawn that can carry a brain calls
    // this (the retail path is Server_RemoveEntityAndNotify @0x50A270 ->
    // Entity_Destroy @0x43e810).
    void release(EntityHandle h);
    AiEntity *at(int ai_index);
    const AiEntity *at(int ai_index) const;
    AiEntity *for_handle(EntityHandle h);
    const AiEntity *for_handle(EntityHandle h) const;
    // The pool index behind for_handle's resolve (-1 = no brain for h) — the
    // directory join key (world/inspect.h).
    int index_for_handle(EntityHandle h) const;
    int count() const { return static_cast<int>(entities_.size()); }

    AiEventQueue events;
    NavNodeTable nav;         // channel/node table the waypoint mover walks
    bool is_authority = true; // [orig: g_napi_np_ctx.is_authority]
    bool is_in_session = false;

    // The mission's terrain field (MissionKernel::wire_terrain). Null (the default) is a
    // terrain-free rig: the ground samplers report no coverage.
    const terrain::TerrainHeightField *terrain = nullptr;
    GroundClearance ground_clearance{};
    // World-object collision (embedder-wired like `terrain`; null = terrain-only motor).
    // When set, the tick rebuilds the proximity tables [orig: Entity_UpdateAllEntities
    // @0x4c2100 -> Entity_BuildAllProximityLists @0x4c20f0] and the infantry vertical
    // resolve routes through CollisionWorld::resolve_entity (D-INF-3 burn-down).
    CollisionWorld *collision = nullptr;

    // ---- Infantry motor (org1 soldiers; docs/world/world-wac-ai-re.md §3) ----
    // Root-motion provider; injected like `terrain`. Null = no clips: every state is
    // unavailable, the selector idles, and infantry entities stand still (no model fallback —
    // motion comes from clips, as in the original).
    IRootMotionSource *root_motion = nullptr;
    // The variant-ring heads both channels of every body serve from, one table
    // per loaded .adm (infantry.h AnimVariantRings). Rewound with the brains.
    AnimVariantRings anim_rings;
    // (The fall-damage tolerance is the WAC named value World::wac_values.fallmps
    //  [orig: dword_C6EAE4]; the landing leg in infantry.cpp reads it there.)
    int find_target_calls = 0;// coverage: target-acquisition invocations
    std::vector<RelMatCall> relmat_calls; // diagnostic trace of the applied mover side effects

    // ---- P2: GROUND combat + targeting ----
    std::vector<RelOpCall> rel_ops;        // recorded engagement relation-matrix ops (trace;
                                           // the APPLY now writes world.script.relations — D-AI-3)
    std::vector<int32_t> target_set_calls; // recorded Entity_SetAITarget net-ids (@0x45d760)
    uint32_t prng_a = 0;    // [orig: dword_31BFBB8] engagement fire-delay jitter stream
	int32_t aircraft_turn_sequence = -1; // [orig: dword_815190]
	uint16_t fire_shot_seq = 0; // per-shot sequence word [orig: word_B7C670]

	// dword_31BFBB8 owns this independent rotate LCG. PRNG_Next16's shared
    // dword_31BFBB0 owner lives on World so non-AI consumers cannot fork it.
    int32_t prng_step_a();  // [orig: inline LCG on dword_31BFBB8]

    int index_of(const AiEntity &e) const; // AI index (= AIEvent entity_index)

    // [orig: the shared death-velocity event @0x467730/0x457d70/0x467400] queue a crash(3) or
    // still(4) AIEvent by the hull's horizontal speed (sqrt over the entity record's
    // +0x98/+0x9C velocity pair, >=1057 -> 3 else 4; channel 0). `world` resolves that
    // record; a brain without one reads its own mirrors.
    void queue_death_event(const World *world, AiEntity &e);

    // [orig: AI_FindBestTargetB @0x466f60] the candidate FEED (D-AI-1): the class-driven
    // pool walk — four profile weapon-slot classes (+40+4i), each gated by its class-
    // priority word (+80+4*class), each selecting its pools + sub-filter legs (class 0 =
    // pool 1 helo-brained then pool 0 players; 1 = pool 1 non-helo; 2 = pool 0 non-player;
    // 3 = pool 2) — into a scratch list, then the scoring core with the lazy LOS probe.
    // Entry gates: teamless-without-see-all and the round-end latch [orig:
    // g_spawn_success_gate @0x24C1928] return null. `variant_a` runs
    // AI_FindBestTarget @0x465A50 instead, instruction-identical but for
    // the two arc bytes read signed (movsx @0x465A8C/0x465A9B against B's
    // movzx @0x466FBC/0x466FCB): the aircraft sites always, and
    // AIEntity_TryAcquireTarget for a type-1 (HELO) profile.
    bool acquire_target(World &world, AiEntity &e, AiTarget &out, bool variant_a = false);

    // Lazy LOS seam for the scoring core: null -> each candidate's preset los_blocked
    // (the injected-list tests); the live feed supplies the evaluator so the ray runs
    // only for a would-be best / the priority bypass, exactly where the original calls
    // Entity_CheckMutualLineOfSight [orig: @0x467363/@0x46738b — never per candidate].
    using LosBlockedFn = bool (*)(void *ctx, const AiCandidate &c);

    // The byte-exact scoring core over an explicit candidate list (the P2 port; tests pin
    // it directly). [orig: AI_FindBestTargetB @0x466f60 scoring walk]
    bool acquire_target_from(AiEntity &e, const std::vector<AiCandidate> &candidates,
                             AiTarget &out, LosBlockedFn los_fn = nullptr,
                             void *los_ctx = nullptr, bool variant_a = false);

    // [orig: the engagement block @0x4677b3..0x4678b2] APPLY the sees+targeted quads to
    // world.relations (D-AI-3 closed) + record the trace, Entity_SetAITarget, reset the
    // combat timer, set the fire-delay (exact PRNG jitter; the brained-target branch is
    // guarded by base-delay, the other is unconditional), pending = 17.
	void engage_target(World &world, AiEntity &e, const AiTarget &t, bool aircraft = false);

	// [orig: Entity_SetAITarget @0x45d760] brain[kTargetSlot] + AiSlot[3] = handle;
	// maintain the OLD/NEW targets' Entity::ai_target_refcount (dec clamp >=0 / inc).
	void ai_set_target(World &world, AiEntity &e, EntityHandle target);
	void clear_entity_references(World &world, EntityHandle removed);

	// The 8 relation-matrix writes of acquisition/fire: the sees quad + the targeted quad,
	// in the witnessed order/keys (group = Entity::group_id +0x11C, single = net_id +0x7C).
	// [orig: @0x4677b3..0x4678b2 / @0x4b0a6f..0x4b0ae2; matrix identity via the setter
	// bases g_SeesMatrix*/g_TargetedMatrix* — world-wac-ai-re §16.4/§17.2]
	void apply_engage_relations(World &world, const Entity &self, const Entity &target);

    // The aim/LOS origin [orig: Entity_ComputeWeaponFireOrigin @0x43b4b0]. The
    // person leg (def type 3) is pos + (entity+0x6C >> 1|2) + jitter with the
    // +0x6C writer unwalked (D-AI-6 residual), so persons keep the muzzle seam
    // below; a modeled non-person transforms its def "TARGET" userpoint
    // (def+1350) by the placement matrix (@0x43b5d4..0x43b5f6), else the model
    // collision-bbox center entity+0x1FC (@0x43b619); without a model the raw
    // position (@0x43b54f). Both ends of the mutual-LOS test and the aim
    // TARGET point ride this [orig: Entity_CheckMutualLineOfSight @0x539be0].
    void weapon_aim_origin(World &world, const Entity &e, int32_t out[3]) const;
    void weapon_aim_origin(World &world, const AiEntity &e, int32_t out[3]) const;
    // The fire origin: the static forms copy the raw entity origin, retail's
    // own no-model / no-point copy [orig: Entity_GetAttachmentWorldPosition
    // @0x4b2767..0x4b278e]. The aim EYE use is exact: retail's combat-pass aim
    // anchor IS the posed launch bone [orig: Entity_GetAttachmentWorldPosition
    // @0x4b2670 on bone +0x366 — world-wac-ai-re §21.1/§21.4].
    static void weapon_fire_origin(const AiEntity &e, int32_t out[3]);
    static void weapon_fire_origin(const Entity &e, int32_t out[3]);
    // Asset-aware live-pose form used by the simulation. It asks the world's
    // native muzzle-pose provider at the actual LOS/aim/fire call site (the
    // posed launch userpoint of the current tick), else copies the raw origin.
    void weapon_fire_origin(World &world, const AiEntity &e,
                            int32_t out[3]) const;
    void weapon_fire_origin(World &world, const Entity &e, int32_t out[3]) const;
    // One of the NPC's three launch points and the live entity orientation.
    void organic_fire_pose(World &, const AiEntity &, int launch_slot, int32_t out[6]) const;
    // The weapon fire POSITION and its quality [orig: Entity_GetWeaponFirePosition
    // @0x43B630]: 1 = a posed point (a UseGun body's EWeap gun point, a person's
    // position plus CameraOffset, or a modeled entity's def+1351 point), 2 = the
    // position raised 0.75 u (a model without that point), 3 = the raw position
    // (no item def or no model). Both retail readers, the target scan and the
    // single-target validator, take weapon_aim_origin for anything but 1
    // [orig: Entity_FindTargets @0x53A658..0x53A679; sub_53AFC0 @0x53AFF8..0x53B013].
    int weapon_fire_position(World &world, const AiEntity &e, int32_t out[3]) const;

    // LOS between two EXACT 16.16 endpoints, true = clear — callers supply the
    // fire origins (weapon_fire_origin) or their own witnessed endpoints: the
    // terrain leg (ported heightmap raycast) + the sector leg (pool-2/pool-1
    // collision-model clip via CollisionWorld::raycast_clear — the D-AI-7 leg).
    // `from`/`to` are the sighting pair, excluded from the sector walk with
    // anything standing on them. No terrain wired = clear (the headless-test
    // default); no collision world wired = terrain leg only.
    // [orig: Entity_CheckMutualLineOfSight @0x539be0 feeds two
    // Entity_ComputeWeaponFireOrigin results into Physics_RaycastTerrainAndSectors
    // @0x539910, ray radius 0, 1 = clear]
    bool line_of_sight_clear(World &world, const int32_t a[3], const int32_t b[3],
                             EntityHandle from, EntityHandle to) const;
    // Snapshot-fan variant: identical LOS semantics, with collision target
    // matrices reused inside a server-declared stable query epoch.
    bool line_of_sight_clear_cached(World &world, const int32_t a[3], const int32_t b[3],
                                    EntityHandle from, EntityHandle to) const;

    // [orig: Entity_AlertNearbyAllies @0x4654b0] pool-1 (rebase: + pool-0 organics with
    // brains) same-team, alive, non-building entities within `radius_units` (16.16):
    // own AiSlot+136 = 2 and each ally's brain alert = 2.
    void alert_nearby_allies(World &world, AiEntity &e, int32_t radius);

    // AI fire -> the authoritative round path (the Entity_FireWeaponAndSendPacket @0x42bd80
    // authority leg): ring append (the S2C 0x0A tag-2 fan-out) + RoundSim spawn — the same
    // pair the C2S 0x06 player path enters. Host-only (callers are authority-gated).
    // [orig: WeaponSlot_FireAndSpawnEffects @0x53f440 -> Server_ClientFiredRound @0x50baa0
    // ring append + RoundData_SpawnRound @0x4ec0d0; net-re §5.60]
    bool fire_ai_round(World &world, AiEntity &e, const int32_t origin[3], int32_t yaw_bam,
                       int32_t pitch_bam, int32_t ammo_index);

    // The SM/turret fire-transform solve [orig: Entity_ComputeWeaponFireTransform_0
    // @0x456980 — the D-AI-2 core]. Solves the muzzle origin + aim toward `target`
    // through one .aip weapon block: origin = the block's selected muzzle bone
    // (the brain+224/+292 lists filled at spawn), else entity pos + 2.0u Z (the
    // empty-list leg), aim =
    // the relative yaw/pitch of the target position in the biased shooter frame,
    // the caller's aim offset (retail passes it through the `distance` global:
    // sweep phase under ATEAM, -3.0u under ATEAM_LOCK), the cone gate, then the
    // WEAPON_TURRET staging + WEAPON_SLOW/FAST slew. `target` may be null — only
    // the WEAPON_PITCHLOCKED* legs (flags & 0x18) can solve then [orig: the
    // @0x4569B2 head gate]. Returns true when the pose is solved (and, for
    // turrets, aligned) — the caller then fires; false = hold (slewing, cone
    // miss, LOS block, or no valid solve). `out` = {pos xyz 16.16, yaw, pitch,
    // roll BAM}.
    // Relative aim metrics and angular envelope shared by threat scans and
    // weapon validation. [orig: compute_relative_position_metrics @0x545710]
    static uint32_t weapon_relative_metrics(const int32_t pose[6], const int32_t aim[3],
                                             int32_t metrics[6]);
	bool weapon_target_metrics(World &world, AiEntity &e, const Entity &target,
			const int32_t pose[6], int32_t aim_offset, bool skip_los, int32_t metrics[6]);
	// `seeded` = the caller already copied the retail pre-seed (entity
	// Position/Yaw/Pitch/Roll, or the ground primary continuation's levelled
	// pitch) into `out`; otherwise the solve seeds it from the entity.
	bool solve_weapon_fire_transform(World &world, AiEntity &e, const Entity *target,
			const AiProfile::WeaponFire &wb, int32_t aim_offset, bool skip_los, int32_t out[6],
			bool seeded = false);

	// [orig: AI_HandleCommand @0x465770] The queued ChangeAI command handler: types 6..12,
    // 21 and 22 carry an arm; 13..20 and every other type (the damage/death/destroy events
    // 1/3/4 among them) take the default return 0, so this returns false and the caller's
    // event switch proceeds.
    bool ai_handle_command(World &world, AiEntity &e, const AiEventEntry &ev);

    // [orig: AI_BeginUpdate @0x457b40] movement-controller row 4. Copies working
    // fields, then advances this entity's phase or requests combat/fallback past 496.
    bool begin_update(AiEntity &e);

    // The two class event callbacks (fn1 of g_EntityClassEventCallbackTable
    // @0x813000, resolved per items.def ai_function by EntityDef_InitAllCallbacks
    // @0x4a5aae -> Entity_LookupRenderCallbacks @0x407dc0): the CHel row and the
    // cpln thunk @0x462120 run the air-class machine, the cveh row and the
    // cbot/ctrn thunks @0x462130/@0x462140 the vehicle-class one. Both share one
    // body; they differ only in the alert leg's state pair, the client tick and
    // transition gates, and the kill/damage notification's channel word.
    // [orig: EntityAI_ProcessInfantryStateMachine @0x4581b0] event: 0=update,
    // 1=kill/damage notification, 4=death.
    // (IDB name; it is the AIR-class brain machine: alert -> pend 10 unless cur 14,
    // client ticks cur 13/15, client commits pend 7 or 13..15, notification channel 9.)
    void process_infantry_state_machine(AiEntity &e, World &world, int event);
    // [orig: EntityAI_ProcessVehicleStateMachine @0x4583c0] the ground/boat/train
    // machine: alert -> pend 18 unless cur 22 (@0x458442..0x458448), client ticks
    // cur 21/23 (@0x458545..0x45854d), client commits pend 16 or 21..23
    // (@0x458579..0x458586), notification channel word 0 (@0x45851a, bx zeroed
    // @0x4583cd).
    void process_vehicle_state_machine(AiEntity &e, World &world, int event);

    // The shared pending-state transition (exit current, enter pending, commit).
    void apply_transition(AiEntity &e, World &world);

    // [orig: AI_UpdateWaypointMovement @0x457bd0] advance the brain along its path via
    // the nav table; writes the working target transform (kWorkPos*/kWorkHeading) and
    // out-speed (kOutSpeed). Applies the per-advance visited marks that BMS
    // SingleAtWaypoint/GroupAtWaypoint consume, and retains a diagnostic trace.
    int update_waypoint_movement(AiEntity &e, World &world);
	int update_aircraft_waypoint_movement(AiEntity &e, World &world);
	int32_t aircraft_ground_height(World &world, AiEntity &e, int32_t radius);
	void enter_aircraft_combat(AiEntity &e, World &world);
	void enter_aircraft_evade(AiEntity &e, World &world);
	void aircraft_evade_tick(AiEntity &e, World &world);
	void aircraft_combat_tick(AiEntity &e, World &world);
	int aircraft_movement(AiEntity &e, World &world);
	bool aircraft_target_in_sight(AiEntity &e, World &world);

    // Seat-follow phase for a LIVE mounted occupant. Infantry callers keep running
    // death, perception, combat, and animation around it and suppress only ordinary
    // locomotion; non-infantry callers may use the result as a full SM shortcut.
    // Dead occupants return false so the infantry death edge detaches first.
    // [orig: Entity_UpdateInfantryAI parent/health gate @0x4b9960..0x4b9983,
    //  mounted pose @0x4bec23..0x4bed3f, death detach @0x4b9c57..0x4b9c60.]
    bool pose_if_mounted(AiEntity &e, World &world);

    // Repeat only the carrier-owned seat-frame pose after a later carrier mover.
    // This deliberately does not mirror input, chase gunner aim, select animation,
    // detach relationships, or mutate any other once-per-body-tick state. Local
    // player and gunner LOOK mirrors are preserved while the body/collision frame
    // is recomposed from the carrier's final same-frame transform.
    bool refresh_mounted_pose(AiEntity &e, World &world);

    // The brain half of every waypoint REDIRECT writer (EntityCommands'
    // group/single/WAC orders): the mode/list/node words copied from the slot the
    // writer just stored. [orig: Entity_SetWaypointByTeam @0x43CE02..0x43CE30 /
    // @0x43CEDD..0x43CEFE; Entity_SetWaypointForTeam @0x43DDA8..0x43DDCB /
    // @0x43DE3C..0x43DE6A; WacCmd_SsnToWp @0x4F1D6A..0x4F1D99]
    void copy_route_order_to_brain(AiEntity &e);
    // The waypoint refresh (its -1 not tested) plus the per-leg turn budget
    // (|Yaw - bearing| / denom) << 5 that three of the writer legs run after the
    // copy. [orig: Entity_SetWaypointByTeam @0x43CE36..0x43CE4F;
    // Entity_SetWaypointForTeam @0x43DE70..0x43DE89; WacCmd_SsnToWp @0x4F1D9F..0x4F1DB9]
    void seed_route_turn_budget(AiEntity &e);
    // Quantized 3D distance; the last equal-distance node wins. Reserved
    // boarding commands retain slot[38]. [orig: @0x407EA0]
    int32_t nearest_route_node(const AiEntity &e, uint32_t list) const;

    // The vehicle-physics input staging for a PlayerControl vehicle without a live PLAYER
    // controller, shared by the cveh and ctan movers whose legs are instruction-equivalent
    // [orig: Entity_UpdateVehiclePhysics @0x48af00 — the parked stamp @0x48c002-0x48c02d
    // and the AI-driver leg @0x48bc12-0x48c034 (2026-07-16 witness);
    // Entity_UpdateTankVehiclePhysics @0x488AB0 — AI leg @0x4897DB..0x489C00, parked
    // @0x489BCE..0x489BF9]:
    //  - controller == nullptr (or Flags & 0x10000002): hold heading + zero speed and
    //    stamp the brain into state 22 (the parked/player-mode state). Zero health
    //    alone does not park;
    //  - an AI controller: hand state 22 back to 16, cmd speed = min(brain outSpeed,
    //    player_speed), re-resolve the waypoint target when the per-leg turn budget
    //    (brain[32]) is spent, clamp the bearing delta to the budget, damp speed 0.75x
    //    per ~30/60 deg of residual turn when turn_rate2<<6 < budget, steer = heading +
    //    delta + delta/8, and fill `out` (ai_drive = true).
    // The pool-1 collision-avoid brake, the minAI crew health clamp and the
    // wait-for-boarders stop are inline; the parked branch runs the stuck
    // escalation; the handbrake byte-973 latch and the crashed stop sit in
    // tick_vehicle_motor past the input block.
    void vehicle_ai_drive(World &world, Entity &veh, const Entity *controller,
                          const VehicleTraits &traits, VehicleDriveCmd &out);

    // The WATERCRAFT sibling of vehicle_ai_drive — the cbot AI-driver/parked
    // staging witnessed 2026-08-06 [orig: Entity_UpdateWatercraftPhysics
    // @0x48D480, AI leg @0x48E247..0x48E756, parked leg @0x48E7EE..0x48E81E]:
    //  - controller == nullptr (or wrecked/dead vehicle): parked stamp (state 22);
    //  - an AI controller: 22->16 hand-back, cmd = min(brain outSpeed, water_speed),
    //    boat turn budget (|err|/denom) << 4 on the spent-budget refresh, +-budget
    //    delta clamp, 0.75x speed damps at 15/30/45 deg of residual turn, steer =
    //    heading + delta (no delta/8 term), the slip counter-steer + its 4-tier
    //    speed damps, and the shared pool-1 avoid brake.
    // The minAI crew health clamp (@0x48E27F..0x48E2C7), the wait-for-boarders
    // stop (@0x48E75B..0x48E7EC) and the parked leg's stuck check
    // (AI_CheckVehicleStuckState @0x465290) are inline; the aiComp[135] <-
    // brain[127] mirror stays an unmodeled slot (D-NET-161).
    void watercraft_ai_drive(World &world, Entity &veh, const Entity *controller,
                             const VehicleTraits &traits, VehicleDriveCmd &out);

    // The CHel AI flight drive: stage this tick's flight commands (fwd/lat
    // cyclic, steer heading, target altitude, engine flag) into the aircraft
    // motor's registers and raise VehicleMotorState::ai_drive. Retail computes
    // this INSIDE the aircraft physics; the split to the decision layer
    // mirrors vehicle_ai_drive. Body in ai_waypoints.cpp.
    // [orig: the AI leg of Entity_UpdateAircraftPhysics @0x490310 — parked
    //  state-14 block, the crewed 14->7 transition, waypoint target + turn
    //  budget, cyclic (132*sin/cos)>>22 with the near-ground 1/8 damp,
    //  collective from the altitude error, the AGL floor avgGround + bound/4]
    // True while a live, unmounted body is still walking over to board this
    // vehicle and a seat remains free — the hold behind the witnessed
    // wait-for-boarders gate. Body and witness in ai_waypoints.cpp.
    bool vehicle_waits_for_boarders(World &world, const Entity &veh);
    void chel_ai_drive(World &world, Entity &veh, const Entity *controller,
                       const VehicleTraits &traits);

    // The driverless stuck escalation every family's parked leg runs
    // [orig: AI_CheckVehicleStuckState @0x465290 — call sites: the ground parked
    //  stamp @0x48c01e, the boat parked leg @0x48e808, the air parked block
    //  @0x491c5e]. The +0x148 counter climbs once per call while the entity's
    //  think cooldown is not 1; on the authority, every 16th count past 32 a
    //  live pool-0 body within (both bound radii + 12 u) resets it, and past
    //  3410 counts a hull more than 12 u from its spawn anchor is nudged upward
    //  (slideDecay += 1024 per check) until 3720, then killed (Health = 0).
    //  Body in ai_waypoints.cpp.
    void check_vehicle_stuck(World &world, Entity &veh);

    // The minAI crew clamp at the head of every AI-driver leg [orig: ground
    //  @0x48bc4e..0x48bc94, boat @0x48E27F..0x48E2C7, air @0x4915b2..0x4915e2]:
    //  `minAI > 1`, the hull is no longer at its spawn anchor
    //  (Entity_IsBoneInProximity @0x434F90) and fewer than minAI bodies ride it
    //  (Entity_CountMountedEntities @0x435970) -> Health = min(Health,
    //  criticalHp). Undercrewed AI hulls bleed to critical once they move off.
    void apply_min_ai_crew_clamp(World &world, Entity &veh, const VehicleTraits &traits);

    // ---- Infantry motor [orig: Entity_UpdateInfantryAI @0x4b9910] ----
    // Per-tick update for inf.active entities (replaces the vehicle SM path for them).
    // Order: anim root advance -> death edge -> ground resample (every 8) -> think +
    // state selection (every 16, authority) -> body-heading turn -> slope slide (every 8)
    // -> rotate root delta by heading -> integrate + gravity/ground (every 2).
    void tick_infantry(AiEntity &e, World &world, uint32_t logic_tick);
    // The infantry combat pass (org1 riflemen; world-wac-ai-re §17.1-17.3/17.5, D-AI-4):
    // 32-tick staged perception and 16-tick reactions, movement goals and aim.
    // Returns the proposed animation (0 = no override) for the common selector.
    // Authority + alive only. [orig: think gate @0x4BA970]
    int infantry_combat_think(AiEntity &e, World &world, uint32_t key);
    // The anim-event sound pass (§17.4 sounds): the six SSAudio foley bits
    // (0x20..0x400 -> slots 24-29) then the two footstep bits (0x1/0x2 -> the
    // surface-picked slots 17-23, position dipped to foot level by the frame's
    // capsule bottom). NPC body on ODD ticks, player body on EVEN — the two
    // updaters run opposite halves of the 62 Hz tick.
    // [orig: Entity_UpdateInfantryAI @0x4bf169-0x4bf2b0 (gate @0x4bf144);
    //  Entity_UpdateInfantryPlayerBody @0x4b76f1-0x4b78a8 (gate @0x4b76e6)]
    void infantry_anim_sound_pass(AiEntity &e, World &world, uint32_t logic_tick,
                                  int32_t capsule_bottom);
    // Resolve the entity's sound profile slot to its authored set name and queue
    // the world SoundSlotEvent (empty slot = the id-0 no-op, nothing queued).
    // [orig: Entity_GetProfileSlotSound @0x528300 ->
    //  Entity_PlaySound3D_FullVolume @0x528e20]
    void emit_slot_sound(World &world, const AiEntity &e, int slot, const int32_t pos[3]);
    // The infantry fire pass (§17.4): consume the .bad anim-event trigger bits
    // (inf.last_events, odd ticks) + the walking-fire latch -> fire_ai_round; magazine
    // decrement + the reload trigger. Runs AFTER the anim advance refreshed last_events.
    void infantry_fire_pass(AiEntity &e, World &world, uint32_t logic_tick);
    // Dedicated UseGun request after the ordinary anim-event block: resolve the parent
    // emplacement weapon, then require target/range/alignment before authoritative fire.
    // The occupant's personal profile ammo is never used.
    // [orig: Entity_AttachToUseGunSlot @0x546b80; request @0x4bf4bb..0x4bf59e.]
    void infantry_mounted_fire_pass(AiEntity &e, World &world, uint32_t logic_tick,
                                    uint32_t key);
    // One pool-0 row's visit in the frame's weapon-action walk
    // (World::pump_weapon_actions): a UseGun gunner (an NPC or a remote player)
    // pumps the parent MountSlot it borrowed. The infantry request above only
    // writes next=FIRE; this visit advances the authored weapon FSM and emits
    // the round with the gunner as owner. A gunner that has died drops its
    // link instead.
    // [orig: WeaponAction_ProcessAllEntities pool-0 walk @0x5426A6..0x5426C9
    //  (the gunner's +0x118 is the parent's MountSlot) ->
    //  WeaponAction_ProcessFrame @0x540E60]
    void pump_gunner_slot(World &world, Entity &owner, uint32_t logic_tick);
    // The port's mount->gunner link (Entity::primary_weapon_owner) has no
    // retail twin: retail's borrow lives on the gunner's own +0x118 and ends
    // with it. A link whose gunner is gone, dead, reseated or on another mount
    // is dropped when the walk reaches the mount's pool-1 row.
    void release_stale_gunner_link(World &world, Entity &mount);
    // Reconcile the split AiEntity/registry stores, wire animation, and part channels
    // at either the mounted return or the ordinary end of the infantry tick.
    void finish_infantry_tick(AiEntity &e, World &world);
    // AUTHORITY body-anim selection for a net-snapped REMOTE player. The movement motor must
    // not re-simulate a wire-snapped peer (tick_infantry skips it), but the retail authority
    // still runs the player-body ANIM selection for every player, consuming the REPLICATED
    // MoveOrder byte (bits 0-2 dir, bit 3 moving) + stance bits (C2S 0x1D -> MoveOrder bits
    // 8-9) every 4th tick, and the selected state/ratio feed that player's 0x0A record bytes
    // 14/15. Also mirrors the wire-anim fields onto the world Entity. [orig:
    // Entity_UpdateInfantryPlayerBody @0x4b40e0 — local-or-authority gate @0x4b70a3-0x4b70b2,
    // 4th-tick gate @0x4b70ce, selection @0x4b7183-0x4b729d, commit @0x4b7356-96]
    void remote_player_body_anim(AiEntity &e, World &world, uint32_t logic_tick);
    // Mirror the selected body-anim state + channel phase (and, for the local player, the
    // packed MoveOrder low byte) onto the world Entity the 0x0A snapshot reads.
    void mirror_wire_anim(AiEntity &e, World &world);
    // The 16-tick navigation think: waypoint channel walk (arrival, relmat marks, marker
    // wait + facing, one-shot end), commands 123..127. Writes inf.move_* + target_heading.
    // Returns false when the body is airborne (Flags 0x2000 without the 0x80 climb
    // order): retail then skips the WHOLE rest of the think, combat, selection,
    // attachment and the post-commit tail included [orig: Entity_UpdateInfantryAI
    // @0x4BAA57..0x4BAA66 -> loc_4BE7FD].
    bool infantry_think(AiEntity &e, World &world);
    // Its route leg (channels 1..122): the has-route/cooldown gate and the node walk.
    void infantry_route_think(AiEntity &e, World &world, int32_t &entry_heading);
    // The reserved-command legs of the think (slot+148 = 123..127): the
    // Goto-SSN-and-board family with per-command seat filters, the goto-group
    // hold, and follow-local-player. Bodies in infantry_board.cpp.
    // `entry_heading` is the think-entry target heading the S stage rewrites
    // together with the target heading.
    // [orig: Entity_UpdateInfantryAI @0x4b9910 command dispatch;
    //  Entity_FindBestSeatSlot @0x4351f0; Entity_RequestVehicleAttach @0x4364a0]
    void infantry_command_think(AiEntity &e, World &world, int32_t &entry_heading);
    void infantry_board_think(AiEntity &e, World &world, int32_t command,
                              int32_t &entry_heading);
    // Map the movement order to an anim state (walk/run/jog/turn/stop/wounded + availability
    // fallbacks) and commit it under the lock/emote rules.
    void infantry_select(AiEntity &e, World &world, int selected_state = 0);
    // The witnessed org2 PLAYER-BODY selection, shared by the local player and the
    // authority-side remote-player path (the original runs ONE function for both):
    // moving base 1/11/19 + direction offset; idle 48 / 45 (46 idle_mortar for
    // ForceCrouch weapons) / 43-then-44; the forward-walk run promotion (run_2/run_3
    // by 2 + run_anim, scope-suppressed); prone lean rolls 41/42; airborne jump_loop;
    // commit via the flag-table arbitration; then the SWIM override (36-40 from the
    // float latch in entity_flags, bypassing the arbitration). Callers gate it to
    // every 4th tick.
    // [orig: Entity_UpdateInfantryPlayerBody @0x4b7183-0x4b7396; swim @0x4b73c0-0x4b7452
    //  (the 8-case jumptable @0x4b7411: 0 -> 37, 1/2 -> 38, 3..5 -> 40, 6/7 -> 39,
    //  default 36 @0x4b7448); 4th-tick gate @0x4b70ce]
    void player_body_select(AiEntity &e, World &world, uint32_t entity_flags, uint32_t logic_tick);
    // The lean-angle producer, every body tick: decay lean -= (lean+8)>>4, then the
    // on-foot ramp -0x3000000 (left) / +0x3000000 (right) per held lean bit, gated
    // alive + not prone + not latched on a ladder (entity_flags carries the retail
    // Flags view for the 0x100020 gate; the parachute half rides D-INF-20).
    // [orig: decay @0x4b5c97; ramp @0x4b7dbf/@0x4b7dd6 under the (Flags & 0x100020)
    // gate @0x4b7dad; the seated (+0x168==1) +-0x1400000 variant unported]
    void infantry_lean_tick(AiEntity &e, uint32_t entity_flags);
    // The D-COL-5 climb legs (infantry_ladder.cpp; record world-wac-ai-re.md §30).
    // org2, EVERY tick: the on-ladder state override (32/33/34 by the dir fan +
    // look-pitch sign) and the side/back dismounts.
    // [orig: Entity_UpdateInfantryPlayerBody @0x4b7484-0x4b76d8]
    void infantry_ladder_override(AiEntity &e, Entity *tick_entity);
    // org1, post-resolve: the congestion hold, the facing press (gated on the
    // resolver's pass-2 contact latch), and the climb_up/climb_top anchor-band
    // select. Dormant in game until the AI climb-order writer lands.
    // [orig: Entity_UpdateInfantryAI @0x4bf907-0x4bfad8]
    void infantry_ladder_org1_block(AiEntity &e, World &world, Entity *tick_entity);
    // The org1 float/splash block — body and witness in infantry.cpp.
    // The org2 (player body) water block — the buoyant-rise float form with the
    // local-only surface bob, the look-pitch dive/rise term, the velocity drags,
    // the surface clamp and the dive bit 0x200000. Body in infantry.cpp.
    // [orig: Entity_UpdateInfantryPlayerBody @0x4b8020-0x4b8373]
    void player_water_block(AiEntity &e, World &world, Entity *tick_entity,
                            int32_t capsule_bottom, bool is_authority, uint32_t logic_tick);
    void infantry_water_block(AiEntity &e, World &world, Entity *tick_entity,
                              int32_t capsule_bottom, uint32_t logic_tick);
    // The torso-roll producer (entity+0x2DC), every body tick: prone idle 48 decays it
    // toward level (torso -= (torso+8)>>4); the combat rolls 41/42 RAMP it
    // -/+0x4000000 (5.625 deg) per tick — the FP barrel-roll view; otherwise it
    // chases the entity's slope roll (+0x18) a sixteenth-step per tick with the LAG
    // clamped to roll +-0x0E38E380 (20 deg), which also snaps the wrapped post-roll
    // value back once the clip ends. Consumers: the FP camera roll = torsoRoll +
    // lean/4 and the section-14 head/spine roll overlay terms.
    // [orig: Entity_UpdateInfantryPlayerBody @0x4b5cff-0x4b5d6d + @0x4b700c-0x4b7025]
    void infantry_torso_roll_tick(AiEntity &e);
    // The upper-body weapon channel (the entity's SECONDARY AnimMap channel): per-tick
    // desired-state selection + the locked/emote commit rule + the clip-end deferred
    // promotion + the playhead advance. Local-player slice: reload 65 via the 80-tick
    // window; hold poses 50-61 / binoculars 64 / reload2 ride the AdmDef kind dwords
    // (unparsed) so the rifle default (mirror the primary) applies. [orig:
    // Entity_UpdateInfantryPlayerBody @0x4b5cab..0x4b5ea9 + AnimMap_UpdateDualChannels
    // @0x40b8c0; witness world-wac-ai-re.md §14.8.4/.5]
    // The per-tick half: the arms-dip/pitch-kick, the reload window, the deferred
    // promotion and the playhead advance. Runs the selection half below only on the
    // witnessed 16-tick slow-pass phase.
    void infantry_weapon_channel(AiEntity &e, World &world, uint32_t logic_tick);
    // The selection + commit half [orig: @0x4b5dad..0x4b5ea3]. Split out because the
    // original gates it to `(current_tick & 0xF) == 0` while the advance around it runs
    // every tick from AnimMap_UpdateDualChannels @0x40b8c0.
    void infantry_weapon_channel_select(AiEntity &e);
    // The SHARED dual-channel advance — the part of the secondary channel that runs
    // for EVERY organic entity, player or AI: the clip-end deferred promotion and the
    // playhead/blend step. Both body updaters pass their out-array to it
    // [orig: AnimMap_UpdateDualChannels @0x40b8c0, called from the org2 body
    //  @0x4b40e0 AND the org1 body @0x4b9910; witness world-wac-ai-re.md §14.8.1].
    // Org1 mirrors primary current/pending into the secondary at the motor
    // head [orig: @0x4B9A14..0x4B9A48]; this advance keeps the two channels'
    // phases, blends and variant rings independent.
    void infantry_weapon_channel_advance(AiEntity &e);
    // Availability resolution against root_motion->has_clip with the cited fallback chains.
    int infantry_resolve_state(int adm_id, int state) const;
    // The slope pass: 4 ground probes around the entity feeding the slide impulse and
    // the body_pitch/roll slope-conform chase — but ONLY for conforming bodies:
    // def attrib 0x200, an anim state with flag bit 2 (the prone family — crawl 19-26,
    // rolls 41/42, prone idle 48), or a grounded corpse. Every other body DECAYS
    // body_pitch/roll back to level 1/16-step — a live standing/crouched soldier
    // neither slope-leans nor slope-slides, which is what keeps the standing FP camera
    // level on hillsides (torso_roll chases roll; fp_roll = torsoRoll + lean/4).
    // Two witnessed legs: the org1 NPC leg every 8th tick (shifted small-angle slopes,
    // eighth-step chase, 2048 slide) and the org2 player leg every tick (decay) with
    // probes/chase every 2nd tick (atan2 slopes, quarter-step chase, 512 slide,
    // 60-deg live / 48-deg dead slide threshold, roll write skipped during 41/42).
    // [orig: Entity_UpdateInfantryAI @0x4ba10f-0x4ba34c;
    //  Entity_UpdateInfantryPlayerBody @0x4b6d95-0x4b6ff4]
    void infantry_slope_pass(AiEntity &e, World &world, uint32_t logic_tick, uint32_t key);

    const StateRow &row(int32_t state) const;

private:
    // Apply the paired SetBitB(group)/SetBitA(SSN) waypoint-arrival writes to the
    // shared trigger relations, retaining relmat_calls as a diagnostic trace.
    // [orig: AI_UpdateWaypointMovement @0x457c6d..0x457c88]
    void mark_waypoint_visited(AiEntity &e, World &world, int32_t list, int32_t node);
    void clear_handle_index();
    void rebuild_handle_index();
    // The one LOS body behind line_of_sight_clear / line_of_sight_clear_cached.
    bool line_of_sight_clear_impl(World &world, const int32_t a[3], const int32_t b[3],
                                  EntityHandle from, EntityHandle to, bool cached) const;
    // The gun point of a body on a UseGun seat (+0x168 == 3) of an EWeap
    // parent (def attrib 0x20), the leg both fire-point readers open with:
    // the parent's weapon point through its own inline slot, else the
    // parent's raw pose. False = no such seat; `out` is then untouched.
    // [orig: Entity_GetAttachmentWorldPosition @0x4B2682..0x4B26B6;
    //  Entity_GetWeaponFirePosition @0x43B64D..0x43B67D; both call
    //  Entity_ComputeUserpointWorldTransform @0x545C60 with a NULL slot]
    bool usegun_gun_point(World &world, const Entity &gunner, int32_t out[6]) const;

    // Seat a piece's brain: its handle's stale brain released, then the
    // lowest free slot or a new one.
    int place_dismemberment_piece(EntityHandle h, AiEntity &&piece);

    std::vector<AiEntity> entities_;       // pool-relative; index == AIEvent entity_index
    // A body's own update holds references into entities_, and its org1
    // death edge's hit callback can clone a dismemberment piece mid-update
    // (retail's clone lands in fixed pool and AI-block arrays [orig:
    // Entity_CloneFromTemplateByType @0x4398A0]). Such a piece's brain,
    // copied at clone time, is seated once that body's update returns.
    bool defer_piece_brains_ = false;
    std::vector<std::pair<EntityHandle, AiEntity>> deferred_piece_brains_;
    std::vector<AiEntity> spawn_baseline_; // on_load restore target (editor Play->Stop)
    AnimVariantRings spawn_baseline_rings_; // the ring heads at the same capture
    std::vector<int> handle_to_ai_index_;
    std::vector<AiCandidate> scan_candidates_;       // acquire_target feed scratch (reused)
    bool baseline_captured_ = false;
};

} // namespace opennova::world
