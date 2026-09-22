// Mission -> world promotion. See mission/promote.h + docs/world/world-wac-ai-re.md.
#include <runtime/mission/promote.h>

#include <runtime/world/ai.h>
#include <runtime/world/vehicle_part_anim.h>
#include <runtime/world/world.h>

#include <algorithm>
#include <limits>
#include <array>
#include <cstdlib>
#include <utility>
#include <vector>

namespace opennova::mission {

using namespace opennova::world;

// engine/runtime/world mirrors these bms::AttribFlags bits beside its mission_attrib_flags
// field (world stays mission-parser-free); this TU sees both headers, so it pins
// the mirror values to the canonical enum.
static_assert(MissionTables::kMissionAttribSinglePlayerRespawn ==
              static_cast<uint32_t>(bms::AttribFlags::SinglePlayerRespawn));
static_assert(MissionTables::kMissionAttribEnableNVG ==
              static_cast<uint32_t>(bms::AttribFlags::EnableNVG));

std::string ai_profile_name_for(
        const bms::Entity &e, bool placed_item,
        const std::function<PromoteOptions::AiProfileDefaults(int32_t)> &defaults) {
    const auto lower = [](char c) {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
    };
    // name2 is the raw 8-byte BMS field — an 8-char name carries no terminator;
    // the shell resolver this mirrors trimmed and lowercased it.
    std::string name;
    for (size_t i = 0; i < sizeof(e.name2) && e.name2[i] != '\0'; ++i)
        name.push_back(lower(e.name2[i]));
    const auto is_ws = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    size_t begin = 0;
    while (begin < name.size() && is_ws(name[begin])) ++begin;
    size_t end = name.size();
    while (end > begin && is_ws(name[end - 1])) --end;
    name = name.substr(begin, end - begin);
    // The authored ai_textfile wins in every retail arm [orig: the +0x9C
    // non-empty tests @0x461f2b / @0x4684a4 / @0x4687a3].
    if (!name.empty() || !placed_item || !defaults) return name;
    const PromoteOptions::AiProfileDefaults d = defaults(e.type_id);
    if (!d.known) return name;
    // The vehicle family consults the def's own default_aip (+0x8B8) before
    // the helo1 default [orig: Entity_InitVehicleAIFromDef @0x4687b5..0x4687d1];
    // the helicopter family goes straight to helo1 [orig: @0x461f50 / @0x4684c9].
    if (!d.helicopter_init && !d.default_aip.empty()) {
        std::string def_name;
        for (char c : d.default_aip) def_name.push_back(lower(c));
        return def_name;
    }
    return "helo1";
}

// Shared DEF/profile initialization for placed and dynamically spawned AI.
// [orig: Entity_InitVehicleAI @0x460200; Entity_InitHelicopterAI @0x461F00]
void initialize_ai_profile(AiEntity &ae, const aip::Profile &data, AiSystem &ai, EntityKind kind) {
    AiBrain &b = ae.brain;
    if (data.combat_speed >= 0)
        b.f[AiBrain::kSpeedA] = static_cast<int32_t>(
                (static_cast<int64_t>(data.combat_speed) << 16) / 225);
    if (data.patrol_speed >= 0)
        b.f[AiBrain::kSpeedB] = static_cast<int32_t>(
                (static_cast<int64_t>(data.patrol_speed) << 16) / 225);
    if (data.has_ground_combat_speed)
        b.f[AiBrain::kSpeedA] = data.ground_combat_speed;
    if (data.has_ground_patrol_speed)
        b.f[AiBrain::kSpeedB] = data.ground_patrol_speed;
    b.f[AiBrain::kDriveSkill] = data.drive_skill;
    b.f[AiBrain::kPrevAlert] = data.alert;
    // The §16.2 class walk data: the four class-priority words and the
    // derived +40..+52 walk order. The parse is already type-gated like
    // retail's, so the words carry exactly what AIProfile_ParseProperty
    // wrote; the sort loads its keys only for HELO/GROUND profiles
    // [orig: AIProfile_ParseProperty @0x45de70 +80..+92;
    // AIProfile_LoadOrFind @0x45fd80 qsort (CompareFunction @0x455d90,
    // ascending, insertion-stable at 4 entries) stored REVERSED
    // @0x45fed9-0x45ff04].
    ae.profile.type = data.type;
    ae.profile.subtype = data.subtype;
    // The parsed profile is the runtime profile: retain its flight and
    // targeting fields on both families. [orig: @0x45DE70; @0x460200]
    ae.profile.flags96 = static_cast<uint8_t>(data.evade_flags);
    ae.profile.field104 = std::max(0, data.react_ticks);
    ae.profile.view_fov_bam = data.view_fov_bam;
    ae.profile.radar_fov_bam = data.radar_fov_bam;
    ae.profile.view_dist = data.view_dist;
    ae.profile.fov_primary = uint8_t(uint32_t(data.radar_fov_bam) >> 24);
    ae.profile.fov_secondary = uint8_t(uint32_t(data.view_fov_bam) >> 24);
    ae.profile.range_primary = static_cast<int16_t>(data.radar_dist >> 16);
    ae.profile.range_secondary = static_cast<int16_t>(data.view_dist >> 16);
    ae.profile.approach_cap = data.radar_dist;
    ae.profile.min_chase = data.min_chase;
    ae.profile.max_chase = data.max_chase;
    if (data.type == 1) {
        if (data.helo_combat_speed >= 0)
            b.f[AiBrain::kSpeedA] = data.helo_combat_speed;
        if (data.helo_patrol_speed >= 0)
            b.f[AiBrain::kSpeedB] = data.helo_patrol_speed;
        ae.profile.patrol_altitude = data.helo_patrol_altitude;
        ae.profile.patrol_climb = std::max(0, data.helo_patrol_climb);
        ae.profile.field216 = data.helo_combat_altitude;
        ae.profile.field220 = std::max(0, data.helo_combat_climb);
        ae.profile.min_agl = data.min_agl;
        ae.profile.min_speed = std::max(0, data.min_speed);
        ae.profile.flight_flags = data.hunt_flags;
        b.f[AiBrain::kUseWaypointZones] = data.use_waypoint_z;
        if (kind == EntityKind::Item)
            b.f[51] = (uint16_t(ai.prng_step_a()) % 20) << 16;
    }
    // The resolved speed words the class init's respawn re-run reloads
    // [orig: Entity_InitVehicleAIFromDef @0x4688C1..0x4688D3;
    //  Entity_InitHelicopterAIFromDef @0x468597..0x4685A9].
    ae.profile.class_speed_a = b.f[AiBrain::kSpeedA];
    ae.profile.class_speed_b = b.f[AiBrain::kSpeedB];
    ae.profile.class_priority[0] = data.priority_air;
    ae.profile.class_priority[1] = data.priority_ground;
    ae.profile.class_priority[2] = data.priority_organics;
    ae.profile.class_priority[3] = data.priority_decorations;
    {
        const bool keyed = data.type == 1 || data.type == 2;
        std::array<std::pair<int32_t, int8_t>, 4> ents{};
        for (int8_t i = 0; i < 4; ++i)
            ents[i] = {keyed ? ae.profile.class_priority[i] : 0, i};
        std::stable_sort(ents.begin(), ents.end(),
                         [](const auto &a, const auto &b) { return a.first < b.first; });
        for (int i = 0; i < 4; ++i)
            ae.profile.slot_class[i] = ents[3 - i].second;
    }
    // The GROUND weapon def blocks (profile+120/+152) + their brain
    // seeds. The ammo COUNT words brain[53]/[54] are the class init's
    // copy of the +120/+152 capacities, gated on each block's resolved
    // ammo byte [orig: Entity_InitVehicleAIFromDef @0x468882..0x4688B7].
    // The authored "*_weap" names resolve against the loaded ammo table
    // at the item-traits sweep, which applies that gate
    // (mission::resolve_ai_weapons); until then the capacities stand in
    // [orig: AIProfile_ParseProperty @0x45de70 GROUND block;
    // AIEntity_ProcessWeaponFire field map §17.6].
    if (data.type == 1 || data.type == 2) {
        const auto seed_block = [](world::AiProfile::WeaponFire &dst,
                                        const aip::WeaponBlock &src) {
            dst.ammo_cap = src.ammo;
            dst.cone_bam = src.cone_bam;
            dst.flags = src.flags;
            dst.facing_bam = src.facing_bam;
            dst.pitch_bam = src.pitch_bam;
            dst.ammo_name = src.weapon;
        };
        seed_block(ae.profile.fire_a, data.primary);
        seed_block(ae.profile.fire_b, data.secondary);
        b.f[AiBrain::kElevationBias] = data.primary.pitch_bam;
        const aip::WeaponBlock *turret = (data.primary.flags & 1) != 0 ? &data.primary
                : (data.secondary.flags & 1) != 0 ? &data.secondary
                                                     : nullptr;
        if (turret != nullptr)
            b.f[AiBrain::kActiveYaw] = b.f[AiBrain::kStagingBlock + 3] = turret->facing_bam;
        ae.profile.fire_interval_a = data.primary.rate_ticks;
        ae.profile.fire_interval_b = data.secondary.rate_ticks;
        b.f[AiBrain::kAmmoA] = data.primary.ammo;
        b.f[AiBrain::kAmmoB] = data.secondary.ammo;
        // COMBAT_FLAGS is the witnessed flags100 source (ATEAM/
        // ATEAM_LOCK/RC_FIRE ride the SM weapon dispatch).
        ae.profile.flags100 |= static_cast<uint8_t>(data.combat_flags);
        if (data.aim_skill >= 0) {
            // PROBABLE, not anchored: aim_skill (+28, clamped 0..4) is
            // the only 0..4-shaped profile field feeding the (6 -
            // brain[43]) scatter modulus; the spawn copy site itself
            // is the same unwitnessed block copy as the ammo counts
            // (§17.7 item 1, D-AI-2).
            ae.profile.accuracy = data.aim_skill;
            b.f[AiBrain::kAccuracy] = data.aim_skill;
        }
    }
}

// [orig: Entity_InitVehicleAI @0x460200 — the profile pointer only arrives in
//  eax @0x460288, AFTER `mov ebx,[esi+18h]` @0x46024b read the zeroed slot, so
//  fallback/cur/pend are 0 and never profile+0x18. The constants follow the
//  four profile copies (+0x1C/+0x20/+0x24/+0x38 -> brain +0xAC/+0xB0/+0xBC/
//  +0x1B0) and the smooth-target pose copy (entity+0x234..+0x248, +0x27C = 0).]
void initialize_vehicle_brain(AiEntity &ae, World &world, int32_t heading) {
    AiBrain &b = ae.brain;
    b.f[AiBrain::kFallback] = 0;         // @0x46028b
    b.f[AiBrain::kCurState] = 0;         // @0x46028e
    b.f[AiBrain::kPendState] = 0;        // @0x460291
    b.f[AiBrain::kSweepPhase] = -196608; // -3.0 u sweep end  @0x4602b8
    b.f[AiBrain::kBurstWindow] = 0;      // @0x4602c2
    b.f[177] = 0;                        // @0x46032d
    b.f[178] = 0;                        // @0x460333
    b.f[AiBrain::kOutSpeed] = 0;         // @0x460339
    b.f[AiBrain::kTargetRef] = 0;        // @0x46033f
    b.f[137] = 1638400;                  // 25.0 u climb seed  @0x460345
    b.f[179] = heading;                  // entity Yaw  @0x46034f..0x460352
    b.f[199] = 0;                        // @0x460358
    // PRNG_Next16_C @0x6131b0 returns a 16-bit value; the signed `% 0x80000`
    // idiom (`and eax,8007FFFFh; jns; dec; or 0FFF80000h; inc`) is the identity
    // on it, kept for the structural draw [orig: @0x46035e..0x460371].
    b.f[200] = static_cast<int32_t>(world.next_prng16_c()) % 0x80000;
    b.f[201] = 0;                        // @0x460377
}

namespace {

// degrees -> 32-bit binary angle (the entity-heading unit, entity+16). [orig: AI_HandleCommand
// command 0x16 @0x4659fa multiplies degrees by 11930464.]
constexpr int64_t kBamPerDegree = 11930464;

// Provisional kind -> g_pool_list index. The exact original mapping matters only for the
// deferred acquire_target pool scan (P2), not for movement; documented in notes §10.
int pool_for_kind(EntityKind k) {
    switch (k) {
        case EntityKind::Organic: return 0;
        case EntityKind::Item: return 1;
        case EntityKind::Building: return 2;
        case EntityKind::Marker: return 3;
    }
    return 0;
}

const ItemSeatSpec *seat_spec_for_type(const std::vector<ItemSeatSpec> &specs, int32_t type_id) {
    for (const ItemSeatSpec &spec : specs) {
        if (spec.type_id == type_id) return &spec;
    }
    return nullptr;
}


Entity make_seed(const bms::Entity &e, EntityKind kind, uint16_t ssn, uint32_t origin) {
    Entity s;
    s.net_id = ssn;
    s.bms_id = e.id; // carry the file entity id so the embedder can map this entity back to its placed node
    s.kind = kind;
    s.item_id = e.type_id;
    s.position.x = e.get_x();
    s.position.y = e.get_y();
    s.position.z = e.get_z();
	s.spawn_position = s.position;
	s.yaw = e.yaw;
	s.pitch = e.pitch;
    s.roll = e.roll;
    s.team = e.team;
	s.vehicle_spawn_team = static_cast<int8_t>(e.team_budget);
	s.group_id = e.group_id;
	s.waypoint_id = e.waypoint_id;
    s.wp_number = e.wp_number;
    // [orig: Entity_SpawnFromBMSRecord @0x40E9F0, entity+672]
    s.script_next_ssn = (e.type_id == 6005 || e.type_id == 6006) ?
            e.ttool_index : e.next_ssn;
    if (e.type_id == kParticleEffectMarkerTypeId) {
        size_t length = 0;
        while (length < 31 && e.gen_string[length] != '\0') ++length;
        s.script_effect_name.assign(e.gen_string, length);
    }
    s.alert_state = e.alert_state;
    s.ai_flags = e.bmsi_attributes;
    s.spawn_origin = origin;
    // The retail entity Flags dword (entity+36), BMS-attribute part — the 0x10 static record
    // streams it raw (D-NET-147/150). [orig: Entity_SpawnFromBMSRecord @0x40e9f0: attrib
    // 0x200000 -> 0x4000000 (Indestructible), 0x800000 -> 0x400 (Reflective),
    // 0x1000000 -> 0x1000000 (NoShadow)]. The item-def part (Building 0x20000 is kind-known
    // here; hp==0 -> 0x4000000 + subType 0xFF needs the item db) completes in the embedder's
    // item-traits sweep [orig: Entity_InitFromModel @0x40e105 / @0x40dc8e].
    using bms::BmsiAttributeFlags;
    const uint32_t attrib = e.bmsi_attributes;
    if (attrib & static_cast<uint32_t>(BmsiAttributeFlags::Indestructible)) s.engine_flags |= kEntityFlagIndestructible;
    if (attrib & static_cast<uint32_t>(BmsiAttributeFlags::Reflective)) s.engine_flags |= kEntityFlagReflective;
    if (attrib & static_cast<uint32_t>(BmsiAttributeFlags::NoShadow)) s.engine_flags |= kEntityFlagNoShadow;
    // An "engine running" vehicle record spawns with its rotor at full speed,
    // Flags 0x80 up and the ground speed register at 1.0 — a helicopter placed
    // in flight has its rotor already turning [orig: Entity_SpawnFromBMSRecord
    // @0x40ee70..0x40ee94 on attrib bit 0x20000: Flags |= 0x80, +0x468 =
    // 0x2D82D, +0x460 = 0x0CCCCCC0, +0x29C = 0x10000].
    if (kind == EntityKind::Item &&
        (attrib & static_cast<uint32_t>(BmsiAttributeFlags::EngineRunning)) != 0) {
        s.flags |= 0x80u;
        rotor_spawn_full(s.veh.part_spin);
        s.veh.speed = 0x10000;
    }
    if (attrib & static_cast<uint32_t>(BmsiAttributeFlags::Guarding)) {
        s.engine_flags |= kEntityFlagMounted;
        s.flags |= kEntityFlagMounted;
    }
    if (kind == EntityKind::Building) s.engine_flags |= kEntityFlagBuilding;
    s.ammo_count = e.map_symbol; // BMS byte 81 -> entity+290 [orig: @0x40e9f0]
    s.ref_num = e.ref_num;       // BMS byte 153 -> entity+533 [orig: @0x40e9f0]
    // BMS byte 155 (.mis "lfp_group") -> entity+538 — the AS zone number (net-re §5.61).
    // [orig: Entity_SpawnFromBMSRecord @0x40e9f0]
    s.zone_number = e.lfp_group;
    // A numbered zone spawns SECURED: entity+540 (the control level) is seeded
    // to 1.0 for every record whose byte 155 is nonzero, so the owner's frontier
    // zone is spawnable from the first tick and no first-presence Secure edge
    // fires [orig: `movzx edx,[edi+0A5h]; mov [esi+164h],dl; jz; mov dword ptr
    // [esi+21Ch],10000h` @0x40ec0a..0x40ec19].
    if (e.lfp_group != 0) s.zone_control = 0x10000;
    // BMS word 14 (wp_distance low u16) -> entity+350 — the capture-zone/proximity radius
    // the 0x0D record's 0x2000/0x8000-gated u16 streams (golden ASH_I5A bunkers: 70).
    // [orig: Entity_SpawnFromBMSRecord @0x40e9f0; net-re §5.11]
    s.zone_radius = static_cast<uint16_t>(e.wp_distance & 0xFFFF);
    // Three marker definitions replace entity+0 (the ordinary model bound)
    // with the BMS waypoint-distance radius. Type 6006 is the KOTH gameplay
    // source consumed by the 1 Hz proximity pass; 6005/2044 use the same field
    // for waypoint/location presentation. Preserve the authored override here
    // so later collision-model resolution cannot substitute the marker graphic's
    // physical bound. [orig: Entity_SpawnFromBMSRecord
    // @0x40F05A..0x40F173/@0x40F213..0x40F227]
    if (kind == EntityKind::Marker &&
        (e.type_id == 6005 || e.type_id == 6006 || e.type_id == 2044)) {
        s.bound_radius = e.wp_distance != 0
            ? static_cast<float>(e.wp_distance)
            : 0.5f;
    }
    return s;
}

// Initialize a freshly-attached AI brain for a spawned entity. Grounded in Entity_InitVehicleAI
// @0x460200 (geometry copy from entity+4.., the zero state words and the constant block via
// initialize_vehicle_brain) plus the def callbacks that wrap it (profile speeds, the waypoint
// words, the move step). Every brain starts in state 0: the vehicle family's first mover tick
// promotes it to GROUND_PRETTY (22) / HELO_PRETTY (14) [orig: Entity_UpdateVehiclePhysics
// @0x48afac..0x48afb2; Entity_UpdateAircraftPhysics @0x490377..0x49037d], and only the
// AISETSTATE command, an alert edge or damage moves it on from there. Organics carry no
// vehicle brain in retail (the AiSlot drives the infantry motor), so the port's organic
// AiEntity simply keeps the freshly-attached zero words.
void init_brain(AiEntity &ae, const bms::Entity &e, const PromoteOptions &opts, World &world,
		AiSystem &ai, EntityKind kind) {
	AiBrain &b = ae.brain;

	// Geometry (entity+4/+8/+12 = x/y/z, all 16.16; entity+16 heading = BAM). The engine heading is
    // (90 - yaw) degrees, NOT yaw [orig: Entity_SpawnFromBMSRecord @0x40e9f0 entity+4 =
    // ((90 - yaw)<<16/360)<<16]. The waypoint mover writes the same engine frame (atan2(dY,dX) bearing
    // into kWorkHeading), so storing the seed in the engine frame keeps a unit's facing consistent
    // whether parked or moving; the present pass converts engine-heading -> mission yaw for the basis.
    ae.pos[0] = e.x;
    ae.pos[1] = e.y;
    ae.pos[2] = e.z;
    ae.heading = static_cast<int32_t>(static_cast<int64_t>(90 - e.yaw) * kBamPerDegree);
    ae.team = e.team;

    // [orig Entity_InitVehicleAI: brain[4]=brain[5]=brain[6]=0 @0x46028b..0x460291] initial
    // state 0 (re-stamped with the constant block below for the vehicle family).
    b.f[AiBrain::kCurState] = 0;
    b.f[AiBrain::kPendState] = 0;
    b.f[AiBrain::kFallback] = 0;
    // The def callbacks' move step [orig: Entity_InitVehicleAIFromDef `[brain+1Ch] = 10h`
    // @0x468915; Entity_InitHelicopterAIFromDef @0x468645]; nonzero so the mover advances.
    b.f[AiBrain::kStep] = 16;

    // Profile (movement-relevant subset) from the mission AI fields.
    ae.profile.flags96 = 0;   // not combat-capable / can-fire here (the weapon phase sets these)
    ae.profile.flags100 = 0;  // no use-fallback / ignore-stealth
    // Engage ranges are WORLD UNITS on both sides of the compare, so the BMS value
    // goes in UNSCALED. The BMS fields are plain units, not 16.16 -- the SLOT seeds
    // further down shift the SAME two fields UP to 16.16
    // [orig: slot+60 max / +64 min engagement (<<16); see s.f[15]/s.f[16] below], and
    // AiProfile::range_primary/secondary are documented "+78/+70 max engage range
    // (world units, signed i16)". The consumer agrees: ai_score_target gates
    // `distance > range` against dist3d_units, which is the 16.16 distance shifted
    // DOWN to units [orig: AI_FindBestTargetB @0x4671e8].
    //
    // This previously shifted DOWN (>> 16), uncited. Every 00TRg soldier authors
    // 10..500 here, so it produced 0 for all of them -- and a 0 range makes
    // ai_score_target reject EVERY candidate at any nonzero distance (0 is not
    // "unlimited"), so those AI could never acquire a target, never alert, and never
    // leave their idle pose.
    //
    // UNWITNESSED, declared: whether the BMS record is the right SOURCE for these two
    // profile slots at all. +78/+70 live in the AI PROFILE struct, whose retail source
    // is the .aip / item def rather than the mission record; seeding them from the BMS
    // entity is a pre-existing adaptation kept as-is, with only the SCALE corrected.
    ae.profile.range_primary =
            static_cast<int16_t>(std::clamp<int32_t>(e.max_engagement_distance,
                                    std::numeric_limits<int16_t>::min(),
                                    std::numeric_limits<int16_t>::max()));
    ae.profile.range_secondary =
            static_cast<int16_t>(std::clamp<int32_t>(e.min_engagement_distance,
                                    std::numeric_limits<int16_t>::min(),
                                    std::numeric_limits<int16_t>::max()));

	// Per-node mover speed: brain[49]=kSpeedA (states != 16), brain[50]=kSpeedB (state 16).
	// The .aip profile seeds them when the embedder supplied the entity's profile
	// speeds; the parse scale is x65536/225 exactly like the PATROLSPEED command
	// [orig: Entity_InitVehicleAIFromDef brain[49] = profile+0xC4 (combat_speed),
	//  brain[50] = profile+0xC0 (patrol_speed) @0x4688C7/@0x4688D3; the .aip parse
	//  x1000 x 1/225000 x 65536 @0x45E6E8..0x45E6FD]. Unresolved profiles keep the
	//  default_speed for embedders that do not supply profiles.
	b.f[AiBrain::kSpeedA] = opts.default_speed;
    b.f[AiBrain::kSpeedB] = opts.default_speed;
    // The profile name retail's AI init would load: the ai_textfile, else (a
    // placed vehicle item) the def's default_aip or "helo1" — the same
    // resolution the boot resolver used to load the rows, so a nameless
    // Blackhawk finds its helo1 row here [orig: Entity_InitHelicopterAIFromDef
    // @0x4683C0 / Entity_InitVehicleAIFromDef @0x4686C0 name arms].
	const std::string want =
            ai_profile_name_for(e, kind == EntityKind::Item, opts.ai_profile_defaults);
    if (!want.empty()) {
        for (const PromoteOptions::AiProfileRow &ps : opts.ai_profiles) {
            if (ps.profile != want) continue;
			initialize_ai_profile(ae, ps.data, ai, kind);
			break;
		}
    }
	// The speed words the class init's respawn re-run reloads (the resolved
	// profile's, else the embedder default) [orig: Entity_InitVehicleAIFromDef
	// @0x4688C1..0x4688D3].
	ae.profile.class_speed_a = b.f[AiBrain::kSpeedA];
	ae.profile.class_speed_b = b.f[AiBrain::kSpeedB];
	// The allocator's own seeds (state 0, the constant block, the PRNG C draw) land for
	// every vehicle-family brain — retail always reaches @0x460200 for an AI-data item,
	// profile row found or not (AIProfile_LoadOrFind never returns null there). Organics
	// never run it: their AiSlot is the retail AI record [orig: Entity_InitAllFromModels
	// pool-1 loop @0x40E5B8..0x40E5D8 -> the def callbacks' @0x460200 call @0x4687ff /
	// @0x4684bf / @0x4684cf]. (The former flags100 |= 2 no-acquire stand-in is gone: the
	// class walk gates on the profile's priority words, and the shipped drivable-transport
	// profiles author them zero — the same no-scan outcome, now via the witnessed path.)
	if (kind == EntityKind::Item) initialize_vehicle_brain(ae, world, ae.heading);

    // Waypoint route words (channel = the entity's waypoint_id). The state stays 0: the
    // route is consumed only once the brain reaches GROUND_FOLLOWWP / HELO_FOLLOWWP
    // through the mover's PRETTY promotion and the PlayerControl hand-back (22 -> 16
    // @0x48bc16..0x48bc1c, 14 -> 7 @0x49158a..0x491590) or an AISETSTATE command
    // [orig: AI_HandleCommand case 7 @0x46581c -> AIState_SetByEntityType @0x457570].
    // Id 0 is the reserved no-route id REGARDLESS of the table's slot-0 contents
    // — retail's positional table simply never authors list 0 and every consumer
    // 0-gates [orig: AIWaypoint_UpdateTarget @0x457380 navMeshId==0 -> -1]; the
    // slot half's witnessed seeding is gated the same way (init_ai_slot below).
    const NavChannel *ch =
            e.waypoint_id != 0 ? ai.nav.channel(e.waypoint_id) : nullptr;
    if (ch && ch->count > 0) {
        b.f[AiBrain::kWpType] = 1; // nav-node waypoint
        b.f[AiBrain::kWpChannel] = e.waypoint_id;
        b.f[AiBrain::kWpNode] = std::min<int32_t>(e.wp_number, ch->count - 1);
    }
}

// Seed the infantry motor for an organic (entity class org1).
void init_infantry(AiEntity &ae) {
    ae.inf.active = true;
    // Body and independent look start at the authored facing.
    ae.inf.body_heading = ae.inf.target_heading = ae.inf.aim_heading = ae.heading;
    ae.inf.aim_pitch = ae.pitch;
}

// Seed the AiSlot from the BMS record. Retail allocates and fills the slot for
// EVERY record whose def carries the AI-class attrib (0x100000), vehicles
// included — the vehicle respawn budget (slot[18]) and route restart
// (slot[35/37/38]) read these words. Field map grounded in
// Entity_SpawnFromBMSRecord @0x40e9f0 (the AiSlot block) — see
// docs/world/world-wac-ai-re.md §3.2.
// [orig: the def gate `test [eax+54h],100000h` @0x40ED4E ahead of
//  Entity_AllocateAISlot @0x40ED5C; the fills @0x40ED61..0x40F054]
void init_ai_slot(AiEntity &ae, const bms::Entity &e) {
    AiSlot &s = ae.slot;
    // The authored AI attributes become AiSlot[1] control bits at spawn. Berserk
    // is retail's intentional attack-anyone exception to normal team filtering.
    // [orig: Entity_SpawnFromBMSRecord Blind @0x40ed92..0x40ed9b,
    //  Berserk @0x40eddd..0x40edea, Coward @0x40ee1a..0x40ee26]
    const uint32_t attrib = e.bmsi_attributes;
    if (attrib & static_cast<uint32_t>(bms::BmsiAttributeFlags::Blind)) s.f[1] |= 0x1;
    if (attrib & static_cast<uint32_t>(bms::BmsiAttributeFlags::Berserk)) {
        s.f[1] |= 0x200;
        ae.see_all = true;
    }
    if (attrib & static_cast<uint32_t>(bms::BmsiAttributeFlags::Coward)) s.f[1] |= 0x8;
    // [orig: slot+48/+52 = (field<<16)/100 — perception2/perfectionist2 are the AI move-speed
    // percentages (engine truth: the editor-era names are misleading)]
    s.f[12] = (e.perception2 << 16) / 100;
    s.f[13] = (e.perfectionist2 << 16) / 100;
    // [orig: slot+56 = (obliqueness<<8)/360]
    s.f[14] = (static_cast<int32_t>(e.obliqueness) << 8) / 360;
    // [orig: slot+60 max / +64 min engagement (<<16, both fall back to max_attack_distance),
    //  +68 = max_attack_distance<<16]
    s.f[15] = (e.max_engagement_distance != 0 ? e.max_engagement_distance
                                              : e.max_attack_distance) << 16;
    s.f[16] = (e.min_engagement_distance != 0 ? e.min_engagement_distance
                                              : e.max_attack_distance) << 16;
    s.f[17] = e.max_attack_distance << 16;
    // [orig: slot+40/+44 = 100 - w_accuracy2/w_accuracy1, clamped >= 0; 0 -> 100]
    s.f[10] = (e.w_accuracy2 != 0) ? std::max(0, 100 - e.w_accuracy2) : 100;
    s.f[11] = (e.w_accuracy1 != 0) ? std::max(0, 100 - e.w_accuracy1) : 100;
    // [orig: timers x62 ticks/second — slot+72 movetimer, +76 crouchtimer, +80 shoot_timer,
    //  +88 advancetimer]
    s.f[18] = 62 * static_cast<int32_t>(e.spawns);
    s.f[19] = 62 * (static_cast<int32_t>(e.crouch_timer) |
                    (static_cast<int32_t>(e.unk15a) << 8));
    s.f[20] = 62 * static_cast<int32_t>(e.shoot_timer);
    s.f[22] = 62 * e.advancetimer;
    // [orig: slot byte+136 = alert_state]
    s.bytes()[AiSlot::kAlertByte] = e.alert_state;
    // [orig: slot+140 = 1 (has-route), +148 = waypoint_id, +152 = wp_number (START node)]
    if (e.waypoint_id != 0) {
        s.f[35] = 1;
        s.f[37] = e.waypoint_id;
        s.f[38] = e.wp_number;
    }
}

} // namespace

void initialize_item_seats(Entity &entity, const std::vector<ItemSeatSpec> &specs) {
    const ItemSeatSpec *spec = seat_spec_for_type(specs, entity.item_id);
    if (spec == nullptr) return;
    entity.emplaced_config_valid = spec->mount_config_valid;
    entity.emplaced_config = spec->mount_config_valid ? spec->mount_config : 0;
    entity.armory_points = spec->armory_points;
    entity.primary_weapon = spec->primary_weapon;
    // The item-definition traits (has_item_def, item_attrib) come only from
    // the items.def sweep; a seat spec never stands in for a definition row.
    if (spec->seats.empty()) return;
    entity.seats = spec->seats;
    for (Seat &seat : entity.seats) {
        seat.occupant = EntityHandle{};
    }
}

ItemAttachmentSpawns spawn_item_attachments(World &world, const std::vector<EntityHandle> &carriers,
        const std::vector<ItemSeatSpec> &specs) {
    ItemAttachmentSpawns result;
    // Every stored items.def addeweap* slot creates a pool-1 child. The public
    // metadata already normalized the authored full item id to the raw type id;
    // G/C flags and angle fallback presence remain separate. Children use a
    // non-BMS origin sentinel so the listen-server WirePresentPass cannot defer
    // them to an unrelated placed node with the same (kind,index).
    struct AttachmentWork {
        EntityHandle carrier;
        std::vector<int32_t> lineage;
    };
    std::vector<AttachmentWork> attachment_work;
    attachment_work.reserve(carriers.size());
    for (EntityHandle h : carriers) {
        const Entity *carrier = world.registry.get(h);
        if (carrier != nullptr)
            attachment_work.push_back(AttachmentWork{h, {carrier->item_id}});
    }
    for (size_t work_index = 0; work_index < attachment_work.size(); ++work_index) {
        const AttachmentWork work = attachment_work[work_index];
        Entity *carrier = world.registry.get(work.carrier);
        if (carrier == nullptr) continue;
        const ItemSeatSpec *carrier_spec =
                seat_spec_for_type(specs, carrier->item_id);
        if (carrier_spec == nullptr) continue;
        if (work.lineage.size() >= 8) continue;
        for (const ItemEmplacementAttachmentSpec &attachment :
             carrier_spec->emplacement_attachments) {
            if (attachment.child_type_id == 0 ||
                std::find(work.lineage.begin(), work.lineage.end(),
                          attachment.child_type_id) != work.lineage.end())
                continue;
            Entity child_seed;
            child_seed.kind = EntityKind::Item;
            child_seed.item_id = attachment.child_type_id;
            child_seed.position = carrier->position;
            child_seed.yaw = carrier->yaw;
            child_seed.pitch = carrier->pitch;
            child_seed.roll = carrier->roll;
            child_seed.team = carrier->team;
            child_seed.spawn_origin = 0xFFFFFFFFu;
            const EntityHandle child_handle =
                    world.registry.spawn(pool_for_kind(EntityKind::Item), child_seed);
            if (!child_handle.valid()) {
                ++result.dropped;
                break;
            }
            result.handles.push_back(child_handle);
            Entity *child = world.registry.get(child_handle);
            carrier = world.registry.get(work.carrier);
            if (child == nullptr || carrier == nullptr) continue;
            child->emplacement_parent = work.carrier;
            child->emplacement_parent_spawn_id =
                    carrier->registry_spawn_id;
            // NoNetworkCallback addeweap children also carry their host in the
            // ordinary groundEntity field; retail's shared MountSlot resolver
            // follows +0x28, not the attachment metadata pointer.
            child->ground_target = work.carrier;
            child->emplacement_local = attachment.anchor.seat_local;
            child->emplacement_yaw_offset = attachment.anchor.yaw_offset;
            child->emplacement_bone =
                    attachment.anchor_found ? attachment.anchor.bone_index : 0;
            child->emplacement_anchor_subobject =
                    attachment.anchor_found ? attachment.anchor_subobject : int16_t{-1};
            child->emplacement_kind = static_cast<uint8_t>(attachment.kind);
            child->emplacement_slot = attachment.stored_slot;
            child->emplacement_attachment_flags = attachment.attachment_flags;
            child->emplacement_angle_count = attachment.angle_count;
            child->emplacement_down_limit_bam = attachment.down_limit_bam;
            child->emplacement_up_limit_bam = attachment.up_limit_bam;
            child->emplacement_right_limit_bam = attachment.right_limit_bam;
            child->emplacement_left_limit_bam = attachment.left_limit_bam;
            // Promotion is the authority-side source of the exact addeweap
            // row, including its stored slot even when sibling types repeat.
            child->emplacement_pose_metadata_resolved = true;
            initialize_item_seats(*child, specs);
            Seat anchor = attachment.anchor;
            anchor.type = SeatType::Gunner;
            anchor.bone_index = child->emplacement_bone;
            anchor.attachment_frame = true;
            world.vehicles.pose_mounted_occupant(*child, *carrier, anchor);

            std::vector<int32_t> lineage = work.lineage;
            lineage.push_back(attachment.child_type_id);
            attachment_work.push_back(
                    AttachmentWork{child_handle, std::move(lineage)});
        }
    }
    return result;
}

namespace {

// The record is temporary: retail remaps a 5305 teammate before the item lookup.
// Its class selector has one runtime writer, the settings copy of the constant
// default 1. [orig: Config_SetDefaults @0x54d400; apply_session_settings_to_globals
// @0x551a2a; Entity_SpawnFromBMSRecord @0x40ea5c]
constexpr int32_t kBmsTeammateClass = 1;

bool admit_record(const bms::Entity &record, const World &world,
        const PromoteOptions &opts) {
    const bool session = world.rules.mp_session;
    if (record.type_id == 5305 && (session || world.rules.teammates_disabled)) return false;
    // Both thresholds are signed bytes. The lower-bound flag rejects every
    // offline load; the upper-bound comparison applies only in a session.
    // [orig: Entity_SpawnFromBMSRecord @0x40ea76..0x40eb1c]
    const uint32_t flags = record.bmsi_attributes;
    if ((flags & 0x10u) && (!session || opts.player_limit < static_cast<int8_t>(record.no_less_than))) return false;
    if ((flags & 0x20u) && session && opts.player_limit > static_cast<int8_t>(record.no_more_than)) return false;
    if ((flags & 0x40u) && session) return false;
    if ((flags & 0x80u) && !session) return false;
    const int32_t type_id = record.type_id == 5305 ? 4999 + kBmsTeammateClass : record.type_id;
    const uint32_t attrib = opts.item_attributes ? opts.item_attributes(type_id) : 0u;
    if ((attrib & 0x10000u) != 0) {
        return session && opts.team_count == 4 &&
                (opts.game_type == 0x10000u || opts.game_type == 0x10001u || opts.game_type == 0x10008u);
    }
    return opts.team_count == 4 || (record.team != 3 && record.team != 4);
}

} // namespace

PromoteResult promote_mission(const bms::File &m, World &world,
                              const PromoteOptions &opts) {
    AiSystem &ai = world.ai;
    PromoteResult r;

    // Pools (pools 0..3 = actors searched by net id, pool 4 = static props),
    // at the witnessed per-pool retail capacities by default
    // [orig: EntityPool_Allocate @0x442168].
    for (int pool = 0; pool < world::kEntityPoolCount; ++pool)
        world.registry.configure_pool(pool, opts.pool_capacities[pool]);

    // Nav nodes from markers. Node index == marker order, which is exactly what
    // WaypointRecord::waypoint_numbers indexes. Markers ALSO spawn into pool 3 below
    // (the original spawns them as full entities; the nav table reads them in place,
    // our container rebase keeps a separate node array).
    ai.nav.nodes.clear();
    ai.nav.nodes.reserve(m.markers.size());
    // The map grid-label origin: the FIRST type-2043 marker ("Map
    // Centerpoint, helps align commander map grid" — items.def id 102043).
    // [orig: HUD_InitOverlaySystem @0x5a4999 pool-3 scan for
    //  entity+80 == 2043 -> dword_2723EB4]
    world.tables.map_grid_origin_present = false;
    for (const bms::Entity &mk : m.markers) {
        if (mk.type_id == 2043 && !world.tables.map_grid_origin_present && admit_record(mk, world, opts)) {
            world.tables.map_grid_origin_present = true;
            world.tables.map_grid_origin_x = mk.x;
            world.tables.map_grid_origin_y = mk.y;
        }
    }
    for (const bms::Entity &mk : m.markers) {
        NavEntry n;
        if (!admit_record(mk, world, opts)) {
            ai.nav.nodes.push_back(n); // preserve the zeroed pool-3 slot's index
            continue;
        }
        // Arrival radius from the marker's wp_distance, default 0.5u. [orig:
        // Entity_SpawnFromBMSRecord @0x40e9f0 item 6005: entity dword[0] =
        // wp_distance ? wp_distance<<16 : 0x8000]
        n.f[0] = (mk.wp_distance != 0) ? (mk.wp_distance << 16) : opts.arrival_radius;
        n.f[1] = mk.x;                // entity+4
        n.f[2] = mk.y;                // entity+8
        n.f[3] = mk.z;                // entity+12
        // Facing = the marker's spawn heading (engine frame, like every entity).
        // [orig: marker+16; the infantry think faces it during a hold]
        n.f[4] = static_cast<int32_t>(static_cast<int64_t>(90 - mk.yaw) * kBamPerDegree);
        // Hold time: 62 ticks per movetimer second. [orig: entity[82] = 62 * u16@+62
        // (bms 'spawns' = .mis movetimer); 0 = no hold]
        n.wait_ticks = 62 * static_cast<int32_t>(mk.spawns);
        ai.nav.nodes.push_back(n);
    }
    r.nav_nodes = static_cast<int>(ai.nav.nodes.size());

    // Nav channels from waypoint records. The .bms waypoint block is POSITIONAL —
    // 128 fixed slots where slot index == the authored list id — and the engine's
    // channel table is indexed by waypoint_id directly, so channel i == slot i.
    // Channel 0 is the "no route" sentinel only because no mission authors list 0
    // (AIWaypoint_UpdateTarget @0x457380 returns -1 when navMeshId==0). Inserting a
    // synthetic slot here shifted every channel by one: an order for list N read
    // slot N-1 — routed vehicles took the neighboring list, and a list whose N-1
    // slot was empty (00TRg's group-3 redirect to list 3, list 2 unauthored)
    // dropped the order entirely.
    // [orig: XML_ParseGroupAction @0x4cc450 writes the 34-dword record per channel.]
    ai.nav.channels.clear();
    ai.nav.channels.reserve(m.waypoint_records.size());
    for (const bms::WaypointRecord &wr : m.waypoint_records) {
        NavChannel ch;
        // The channel's dword 0 is the RAW route-flags word: bit0 = one-shot
        // (the only bit the movers mask), bit1/bit2 = the blue/red team-route
        // marks the waypoint-track pick below reads. [orig: Buffer[34*ch] —
        // the mover masks bit0 @0x457c3d-adjacent; the 0x0F waypoint writer
        // tests bit1 on the same dword @0x502e53]
        ch.loopflag = static_cast<int32_t>(static_cast<uint32_t>(wr.flags));
        int count = std::min<int>(static_cast<int>(wr.marker_count), 32);
        count = std::min<int>(count, static_cast<int>(wr.waypoint_numbers.size()));
        ch.count = count;
        for (int k = 0; k < count; ++k)
            ch.entries[k] = static_cast<int32_t>(wr.waypoint_numbers[k]);
        ai.nav.channels.push_back(ch);
    }
    r.nav_channels = static_cast<int>(m.waypoint_records.size());

    // The player waypoint track: the FIRST blue-route channel (flags bit 1 —
    // the same pick the 0x0F world-state writer serializes for a team-1
    // recipient) over the pool-3 markers, with the marker waypoint fields.
    // [orig: NetPacket_WriteWorldStateLoad0x0F @0x502e41 (channel scan) +
    //  Entity_SpawnFromBMSRecord @0x40f0aa (the marker fields); ≤128 entries]
    world.script.waypoints.clear();
    for (const bms::WaypointRecord &wr : m.waypoint_records) {
        if ((static_cast<uint32_t>(wr.flags) &
             static_cast<uint32_t>(bms::WaypointFlags::BlueTeam)) == 0)
            continue;
        const int count = std::min<int>({static_cast<int>(wr.marker_count),
                                         static_cast<int>(wr.waypoint_numbers.size()), 128});
        for (int k = 0; k < count; ++k) {
            const int idx = static_cast<int>(wr.waypoint_numbers[k]);
            if (idx < 0 || idx >= static_cast<int>(m.markers.size())) continue;
            const bms::Entity &mk = m.markers[static_cast<size_t>(idx)];
            WaypointEntry e;
            e.node = idx;
            if (!admit_record(mk, world, opts)) {
                e.radius = 0;
                world.script.waypoints.entries.push_back(e);
                continue;
            }
            e.x = mk.x;
            e.y = mk.y;
            e.z = mk.z;
            // [orig: entity+0 = wp_distance<<16, default 0x8000 @0x40f09b]
            e.radius = (mk.wp_distance != 0) ? (mk.wp_distance << 16) : opts.arrival_radius;
            // [orig: entity+672 = the record's ttool_index @0x40f0ad — the
            //  WPNames STRWPNAME%03i id]
            e.name_id = mk.ttool_index;
            // [orig: entity+528 = word rec 'wp_adv_trigger' @0x40f0b7; the
            //  fired-event index that completes this waypoint]
            e.linked_event = mk.wp_adv_trigger;
            // [orig: entity+535 = attributes bit 22 @0x40f123-0x40f129]
            e.chain_back = (mk.bmsi_attributes & (1u << 22)) != 0;
            world.script.waypoints.entries.push_back(e);
        }
        break; // first flagged channel only [orig: the @0x502e53 scan stops on the first hit]
    }

    // The objectives panel's per-slot text-id tables, from the mission header.
    // The engine indexes them 1-based off a byte pointer one BELOW the first
    // header byte (byte_A7628B[1] = win_conditions[0]); mirror that shift so
    // slot arithmetic stays the witnessed form. [orig: byte_A7628B/byte_A76293
    //  = header +0xBC win_conditions / +0xC4 lose_conditions; readers
    //  EventAction_Dispatch @0x454546/@0x454600, HUD_DrawWinConditions @0x5ba9e0]
    world.script.subgoals = SubgoalState{};
    for (int slot = 1; slot <= 8; ++slot) {
        world.script.subgoals.win_text_ids[slot] = m.header.win_conditions[slot - 1];
        world.script.subgoals.lose_text_ids[slot] = m.header.lose_conditions[slot - 1];
    }

    // Area-trigger zones -> the registry's area table, registered in array order so the area id ==
    // the area-trigger array index (the value SingleIsWithinArea/GroupIsWithinArea param2 references;
    // bms.h param-semantics). Z is unbounded (+-16384.0) unless the trigger constrains it. Without
    // this the within-area triggers + AREA_AI family resolve against an empty table (always false).
    // [The designer-zone-id (1..99) <-> array-index correspondence + AREA_AI param1's exact zone
    // reference are grill-gated (P5); registering the table is the prerequisite.]
    world.registry.clear_script_tables();
    for (const bms::AreaTrigger &at : m.area_triggers) {
        Aabb b;
        b.min.x = at.get_x_min(); b.max.x = at.get_x_max();
        b.min.y = at.get_y_min(); b.max.y = at.get_y_max();
        if (at.constrains_z()) { b.min.z = at.get_z_min(); b.max.z = at.get_z_max(); }
        else { b.min.z = bms::AreaTrigger::kUnboundedZMin; b.max.z = bms::AreaTrigger::kUnboundedZMax; }
        // The Active flag rides along for the player-AWOL probe, which walks only
        // active zones [orig: Entity_IsLocalPlayerOutOfBounds @0x439d40]. The
        // authored id (record dword @0) rides along for the load-time zone-ref
        // resolve [orig: @0x453000/@0x453100 match record[0]].
        Aabb raw = b;
        raw.min.z = at.get_z_min(); raw.max.z = at.get_z_max();
        world.registry.register_area(std::string(), b, at.is_active(), at.id, raw);
    }

    // [orig: Mission_LoadBMSFile @0x40FCC3] Normalize each bounding-box axis.
    // Type 5 supplies WAC location IDs; these are not area-trigger records.
    world.reverb = {};
    world.reverb.mission = world.reverb.selected = m.header.reverb;
    for (const bms::BoundingBox &box : m.bounding_boxes) {
        if (box.type == 4) {
            ReverbRegion r;
            const int32_t lo[3] = {box.min_x, box.min_y, box.min_z};
            const int32_t hi[3] = {box.max_x, box.max_y, box.max_z};
            for (int i = 0; i < 3; ++i) { r.min[i] = std::min(lo[i], hi[i]); r.max[i] = std::max(lo[i], hi[i]); }
            r.value = box.ref_id;
            world.reverb.regions.push_back(r);
        }
        if (box.type != 5) continue;
        Aabb bounds;
        bounds.min = {std::min(box.min_x, box.max_x) / 65536.0f,
                      std::min(box.min_y, box.max_y) / 65536.0f,
                      std::min(box.min_z, box.max_z) / 65536.0f};
        bounds.max = {std::max(box.min_x, box.max_x) / 65536.0f,
                      std::max(box.min_y, box.max_y) / 65536.0f,
                      std::max(box.min_z, box.max_z) / 65536.0f};
        world.registry.register_location(bounds, box.ref_id);
    }

	// Spawn actors + AI brains (organics are AI-driven; vehicles get brains in the vehicle
	// phase). The net id every trigger/action references is AUTHORED in the record —
	// [orig: Entity_SpawnFromBMSRecord @0x40e9f0 copies record dword @+8 to entity+124;
	// EntityPool_FindByNetId @0x4f0a20 matches its low 16 bits over pools 0..3] — so the
	// seed copies e.id verbatim (no load-time assignment). Spawn order mirrors the file
	// order in Mission_LoadBMSFile @0x40f4e0: items -> buildings -> markers -> organics.
	// Command 123/124/125 boarders spawn ON FOOT and walk in through the infantry
	// think's board leg (infantry_board.cpp), exactly like retail — spawn stores
	// only the order (slot+148/+152 via init_ai_slot).
	// [orig: Entity_SpawnFromBMSRecord @0x40e9f0 stores the order; the walk/attach
	//  is Entity_UpdateInfantryAI @0x4b9910]
	// A pool-1 item gets an AI brain when its type authors a CONTROL seat (ctrlx/drvrx
	// userpoints = a drivable vehicle) — the stand-in for the def AIData attrib gate
	// when no model/profile metadata is supplied by the embedder. A pure-gunner
	// emplacement remains brainless itself; its attached organic owns and pumps the
	// parent's embedded weapon slot. [orig: every AIData item gets the 812-byte component
	// at spawn; Entity_SpawnFromBMSRecord @0x40e9f0; UseGun swap @0x546c42]
	auto item_is_drivable = [&](int32_t type_id) {
        for (const ItemSeatSpec &spec : opts.item_seat_specs) {
            if (spec.type_id != type_id) continue;
            for (const Seat &s : spec.seats) {
                if (is_vehicle_control_seat(s.type)) return true;
            }
        }
        return false;
    };
    std::vector<EntityHandle> promoted_item_handles;
    uint32_t spawn_phase_counter = 0; // dword_A77638, reset per load [orig: @0x40f5c0 area]
    auto promote_vec = [&](const std::vector<bms::Entity> &vec, EntityKind kind, bool ai_capable_default) {
        // Pool 0's used count is the ACCEPTED record count while every
        // accepted record still lands at its record index, so with k
        // rejected organics the accepted records at the last k indices sit
        // above `used`: never ticked, never resolved by net id, and
        // overwritten once the player allocator has filled the in-window
        // holes and extends the pool. Pools 1..3 set `used` to the full
        // record count and keep their holes addressable.
        // [orig: Mission_LoadBMSFile @0x40fb0d..0x40fb34 — Pool_GetEntry(0,
        //  record index), `add edi,ebp` on a spawn success, Pool_SetUsed(0,
        //  edi) vs the record-count Pool_SetUsed @0x40f9db/@0x40fa4a/@0x40faba;
        //  the `used` walks: Entity_UpdateAllEntities @0x4c243b,
        //  EntityPool_FindByNetId @0x4f0a2d, Pool_AllocEntry @0x442230]
        std::vector<char> admitted(vec.size());
        uint32_t used_window = static_cast<uint32_t>(vec.size());
        if (kind == EntityKind::Organic) used_window = 0;
        for (size_t i = 0; i < vec.size(); ++i) {
            admitted[i] = admit_record(vec[i], world, opts) ? 1 : 0;
            if (kind == EntityKind::Organic && admitted[i]) ++used_window;
        }
        uint32_t idx = 0;
        for (const bms::Entity &record : vec) {
            uint32_t origin = spawn_origin_pack(static_cast<uint32_t>(kind), idx);
            const bool admit = admitted[idx] != 0;
            ++idx;
            if (!admit) { ++r.dropped; continue; }
            if (idx > used_window) { ++r.dropped; continue; } // record index >= used
            bms::Entity e = record;
            if (e.type_id == 5305) e.type_id = 4999 + kBmsTeammateClass;
            Entity seed = make_seed(e, kind, static_cast<uint16_t>(e.id), origin);
            if (record.type_id == 5305) seed.player_class = kBmsTeammateClass;
            // The authored display name: name_index 0 = none; the resolver maps
            // the index through the mission RTXT [PeopleNames] STRNAME%03i entry
            // and the copy truncates at the retail 15 chars [orig:
            // Entity_SpawnFromBMSRecord @0x40ecbf..0x40ed0a — the rec+4 gate,
            // sprintf("STRNAME%03i") -> "PeopleNames" lookup -> strncpy(+0xF4, 15)].
            if (e.name_index != 0 && opts.people_name_resolver) {
                std::string authored = opts.people_name_resolver(e.name_index);
                if (authored.size() > 15) authored.resize(15);
                seed.display_name = std::move(authored);
            }
            // Pool slot = the record's index within its pool section, not the
            // first free slot: every pool loop hands Entity_SpawnFromBMSRecord
            // `Pool_GetEntry(pool, i)` for record i, so a record that fails to
            // spawn leaves a HOLE and a later record never slides down into
            // it. A live occupant is overwritten exactly as the fixed pool
            // entry would be. [orig: Mission_LoadBMSFile @0x40F4E0 — pool 1
            // @0x40f9bb..0x40f9c6, pool 2 @0x40fa28..0x40fa34, pool 3
            // @0x40fa98..0x40faa4, pool 0 @0x40fb0d..0x40fb19; the per-pool
            // Pool_SetUsed @0x40f9db/@0x40fa4a/@0x40faba/@0x40fb34]
            const EntityHandle want = EntityHandle::make(
                    pool_for_kind(kind), static_cast<int>(idx - 1));
            if (world.registry.get(want) != nullptr) world.registry.despawn(want);
            EntityHandle h = world.registry.spawn_at(want, seed);
            if (!h.valid()) { ++r.dropped; continue; }
            ++r.spawned;
            if (kind == EntityKind::Item) promoted_item_handles.push_back(h);
            if (Entity *spawned = world.registry.get(h)) {
                initialize_item_seats(*spawned, opts.item_seat_specs);
                // The spawn-phase stagger: every spawned record takes the load
                // counter modulo 60 into entity+0x2AC and the counter steps by
                // 11, so the first blink/indoors refresh of the statics is spread
                // over the first 60 ticks in 11-tick steps instead of all firing
                // in the first cohort pass [orig: `mov ecx,dword_A77638 ...
                // mov [esi+2ACh],ecx; add dword_A77638,0Bh` @0x40ec8c..0x40ecb8;
                // the counter resets per load]. Vehicles re-stamp their own
                // 0..15 phase below.
                spawned->class_think_ticks = static_cast<int32_t>(spawn_phase_counter % 60u);
                spawn_phase_counter += 11u;
            }
            const bool ai_capable =
                    ai_capable_default ||
                    (kind == EntityKind::Item && item_is_drivable(e.type_id));
            if (ai_capable) {
                int ai_idx = ai.attach(h);
                AiEntity &ae = *ai.at(ai_idx);
                // The record's slot seed precedes the class init's brain.
                // [orig: Entity_SpawnFromBMSRecord @0x40ED4E..0x40F054, then
                //  Entity_InitAllFromModels -> the def callbacks]
                init_ai_slot(ae, e);
                init_brain(ae, e, opts, world, ai, kind);
                if (kind == EntityKind::Organic) {
                    // Soldiers run the infantry motor, not the vehicle SM.
                    // [orig: g_EntityClassPhysicsTable "org1" -> Entity_UpdateInfantryAI]
                    init_infantry(ae);
                }
                ae.net_id = seed.net_id;
                ae.relmat_id = seed.net_id; // provisional relation-matrix id (net layer = later)
                ae.health = 100;
                if (kind == EntityKind::Item) {
                    world.registry.get(h)->spawn_phase = world.vehicle_ai_spawn_phase;
                    world.vehicle_ai_spawn_phase = (world.vehicle_ai_spawn_phase + 1) & 15;
                }
                ++r.brains;
            }
        }
    };
    promote_vec(m.items, EntityKind::Item, /*ai_capable=*/false);

    const ItemAttachmentSpawns attachments =
            spawn_item_attachments(world, promoted_item_handles, opts.item_seat_specs);
    r.spawned += int(attachments.handles.size());
    r.dropped += attachments.dropped;

    promote_vec(m.buildings, EntityKind::Building, /*ai_capable=*/false);
    promote_vec(m.markers, EntityKind::Marker, /*ai_capable=*/false);
    promote_vec(m.organics, EntityKind::Organic, /*ai_capable=*/true);

    return r;
}

void stash_mission_loadout_rules(
        const bms::File &mission,
        std::vector<std::pair<std::string, int32_t>> &r_availability_rows,
        std::vector<world::WeaponKitEntry> &r_kit_rows) {
    r_availability_rows.clear();
    r_kit_rows.clear();
    for (const bms::ItemAvailabilityEntry &row : mission.item_availability) {
        if (row.name.empty()) continue;
        // The status byte is SIGNED in retail: -1 is the "mission allowed"
        // sentinel the availability builder maps to 3 [orig:
        // build_item_restriction_table @0x54ddb0 reads `(char)name[strlen+1]`,
        // `== -1 -> 3`]; an unsigned widen (255) would never hit that arm.
        r_availability_rows.emplace_back(row.name,
                                         static_cast<int32_t>(static_cast<int8_t>(row.status)));
    }
    for (const bms::WeaponLoadoutRecord &row : mission.loadout.entries) {
        if (row.name.empty()) continue;
        world::WeaponKitEntry entry;
        entry.name = row.name;
        entry.ammo_primary = static_cast<int32_t>(
                std::strtol(row.ammo_primary.c_str(), nullptr, 10));
        entry.ammo_secondary = static_cast<int32_t>(
                std::strtol(row.ammo_secondary.c_str(), nullptr, 10));
        entry.flags = static_cast<int32_t>(
                std::strtol(row.flags.c_str(), nullptr, 10));
        r_kit_rows.push_back(std::move(entry));
    }
}

} // namespace opennova::mission
