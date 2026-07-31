// BMS mission file parser (NovaLogic's Binary Mission format).
// Used for mission files (.bms) in Delta Force and related games.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

namespace opennova::bms {

// ============================================================================
// Constants
// ============================================================================

constexpr uint32_t kMagic = 0x534D42;  // 'B','M','S' (first 3 header bytes; magic[3] is the format version)
// [orig: version gate `byte_A761D3 < 19` @0x40f5aa Mission_LoadBMSFile / @0x40e30a BMS_LoadAndValidateHeader (Jointops.exe)]
constexpr uint8_t kMinVersion = 19;    // 0x13; shipped JO missions are version 19 (magic reads "BMS\x13")
// [orig: per-pool "Too many ..." clamps @0x40f5b5+ Mission_LoadBMSFile / @0x40e326+ BMS_LoadAndValidateHeader.
//  Over-limit shows a warning dialog then continues loading; it does NOT reject the file.]
constexpr uint32_t kMaxItems = 0x4B0;      // 1200
constexpr uint32_t kMaxBuildings = 0x4B0;  // 1200 (engine calls these "decorations")
constexpr uint32_t kMaxMarkers = 0x300;    // 768
constexpr uint32_t kMaxOrganics = 0x100;   // 256
constexpr size_t kHeaderSize = 616;
constexpr size_t kEntitySize = 0xAC;  // 172 bytes
constexpr size_t kWaypointRecordSize = 136;
constexpr size_t kGroupRecordSize = 32;
constexpr size_t kLayerRecordSize = 20;
constexpr size_t kAreaTriggerSize = 32;
constexpr size_t kEventSize = 24;
constexpr size_t kTriggerSize = 32;
constexpr size_t kActionSize = 32;
constexpr size_t kBoundingBoxSize = 36;

constexpr int kWaypointRecordCount = 128;
constexpr int kGroupRecordCount = 64;
constexpr int kLayerRecordCount = 32;

// ============================================================================
// Enumerations
// ============================================================================

enum class ItemType : uint8_t {
    Marker = 0,
    Item = 1,
    Building = 2,
    Organic = 3,
};

enum class MissionType : uint8_t {
    NormalMission = 1,
    CombatVehicleMission = 2,
    TenthMountain = 3,
};

enum class WeatherType : uint32_t {
    NiceDay = 0,
    Rainy = 1,
    Snow = 2,
};

enum class ClimateType : uint32_t {
    Desert = 0,
    Jungle = 1,
    Snow = 2,
};

// Mission attribute flags. A single 32-bit field (header offset 0x88) holding render/option flags plus
// the game mode. The 11 game-mode bits are single-select; their union mask is 0xFF830000.
// [orig: dfx2med.exe (DFX2 mission editor) sub_402770 — decode @0x4050c7 tests the bits in priority
// order to pick the armory combobox item; encode @0x4031cd does `and [x+0x1D4],0x7CFFFF` (== clear the
// game-mode bits) then OR's exactly one. The option bits 0x1/0x2/0x4/0x8/0x20/0x40/0x100000/0x400000
// read/write the SAME dword (some via byte accesses at +0x1D6), confirming one field, not two.]
enum class AttribFlags : uint32_t {
    None = 0,
    WaterOverrideEnable = 0x1,
    FogDistanceOverrideEnable = 0x2,
    FogColorOverrideEnable = 0x4,
    WeatherOverrideEnable = 0x8,
    ForceIndoors = 0x10, // forces the indoors blink bit every frame — a game-side witness, not a dfx2med
                         // option checkbox [orig: Bms_AttribFlags & 0x10 -> accum |= 2,
                         // Render_ProcessMainSceneFrame @0x5ca1c8-0x5ca1cd; docs/render/render-occlusion-re.md §4]
    RotateMap180 = 0x20,
    SinglePlayerRespawn = 0x40,
    AdvanceAndSecure = 0x10000, // [orig: dfx2med string literal "ATTACK_AND_SECURE"; classic-DF/JO name is Advance & Secure]
    ConquerAndControl = 0x20000,
    EnableNVG = 0x100000,
    StartWithNVGOn = 0x400000,
    AttackAndDefend = 0x800000,
    Coop = 0x1000000,
    Deathmatch = 0x2000000,
    KingOfTheHill = 0x4000000,
    FlagBall = 0x8000000,
    CaptureTheFlag = 0x10000000,
    TeamDeathmatch = 0x20000000,
    TeamKingOfTheHill = 0x40000000,
    SearchAndDestroy = 0x80000000,
};

inline AttribFlags operator|(AttribFlags a, AttribFlags b) {
    return static_cast<AttribFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
inline AttribFlags operator&(AttribFlags a, AttribFlags b) {
    return static_cast<AttribFlags>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}
inline bool has_flag(AttribFlags flags, AttribFlags test) {
    return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(test)) != 0;
}

// Entity AI attribute flags
enum class BmsiAttributeFlags : uint32_t {
    None = 0,
    Blind = 1 << 0,
    Guarding = 1 << 1,
    RemoveIfLessThan = 1 << 4,
    RemoveIfMoreThan = 1 << 5,
    Multiplayer = 1 << 6,
    Berserk = 1 << 11,
    FlyingOrganic = 1 << 14,
    Coward = 1 << 16,
    Attribute17 = 1 << 17, // present in shipped missions; editor label bit map not yet pinned
    AdvancedAmmo = 1 << 18,
    Indestructible = 1 << 21,
    NavigationWaypoint = 1 << 22,
    Reflective = 1 << 23,
    NoShadow = 1 << 24,
};

inline BmsiAttributeFlags operator|(BmsiAttributeFlags a, BmsiAttributeFlags b) {
    return static_cast<BmsiAttributeFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
inline BmsiAttributeFlags operator&(BmsiAttributeFlags a, BmsiAttributeFlags b) {
    return static_cast<BmsiAttributeFlags>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

// Waypoint flags
enum class WaypointFlags : uint32_t {
    None = 0,
    DoesNotLoop = 1 << 0,
    BlueTeam = 1 << 1,
    RedTeam = 1 << 2,
};

inline WaypointFlags operator|(WaypointFlags a, WaypointFlags b) {
    return static_cast<WaypointFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

// Event flags (event record flags dword, offset +0 on disk). The DFX2 mission editor exposes exactly
// these three as author checkboxes; shipped JO missions also use internal bits 0x10/0x20, which are
// preserved by editor-facing edits but not exposed as checkboxes. Bit 0x08 remains unsupported by the
// shipped corpus and parser.
// [orig: Med_EventDialogPopulate @0x411690 -> CheckDlgButton(4203/4212/4213, flags bit0/1/2);
//        Med_EventDialogCommit @0x4118d0 -> IsDlgButtonChecked sets bits 0/1/2 only, leaves the rest]
enum class EventFlags : uint32_t {
    None = 0,
    ResetAfter = 1 << 0,   // 0x01  RESET_AFTER        (repeat; else fire-once)   dlg checkbox 4203
    PreMission = 1 << 1,   // 0x02  PRE_MISSION_EVENT                             dlg checkbox 4212
    PostMission = 1 << 2,  // 0x04  POST_MISSION_EVENT                            dlg checkbox 4213
};

constexpr uint32_t kEventAuthorFlagMask =
    static_cast<uint32_t>(EventFlags::ResetAfter) |
    static_cast<uint32_t>(EventFlags::PreMission) |
    static_cast<uint32_t>(EventFlags::PostMission);
constexpr uint32_t kEventInternalFlagMask = 0x10u | 0x20u;
constexpr uint32_t kEventKnownFlagMask = kEventAuthorFlagMask | kEventInternalFlagMask;

inline EventFlags operator|(EventFlags a, EventFlags b) {
    return static_cast<EventFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

// Trigger main types
enum class TriggerMainType : int32_t {
    Group = 1,
    Single = 2,
    Event = 3,
    MissionVariable = 4,
    SecondTimeThrough = 5,
    Teammate = 6,
    Player = 7,
};

// Group trigger subtypes
enum class GroupTriggerType : int32_t {
    Null = 0,
    GroupSeesGroup = 1,
    GroupHasTargetedGroup = 2,
    GroupAtRedAlert = 3,
    GroupDestroyed = 4,
    GroupAlive = 5,
    GroupHasLostMoreUnits = 6,
    GroupAtWaypoint = 7,
    GroupIntact = 9,
    GroupIsWithinArea = 10,
    GroupHoldingGroup = 11,
    GroupHasMoreUnits = 12,
    GroupHasShotGroup = 13,
    GroupAtYellowAlert = 14,
    GroupHasTargetedSingle = 15,
    GroupSeesSingle = 16,
    GroupHasShotSingle = 17,
};

// Single (SSN) trigger subtypes
enum class SingleTriggerType : int32_t {
    Null = 0,
    SingleSeesGroup = 1,
    SingleHasTargetedGroup = 2,
    SingleAtRedAlert = 3,
    SingleDestroyed = 4,
    SingleAlive = 5,
    SingleHasLostMoreUnits = 6,
    SingleAtWaypoint = 7,
    SingleIntact = 9,
    SingleIsWithinArea = 10,
    SingleHoldingGroup = 11,
    SingleHasMoreUnits = 12,
    SingleHasShotGroup = 13,
    SingleAtYellowAlert = 14,
    SingleHasTargetedSingle = 15,
    SingleSeesSingle = 16,
    SingleHasShotSingle = 17,
    SingleOnTopOf = 42,
    SingleFartherThan = 43,
    SingleHasNoLOS = 44,
    SingleDoesNotSeeOrFarther = 45,
};

// Mission variable trigger subtypes.
// [orig: EventTrigger_EvaluateCondition cat 4 @0x453620 — dword_C6B240[param1] <op> param2]
// Engine compares: 1:==, 2:<, 3:>, 4:<=, 5:>=. Values 3/4 were previously swapped (inherited from the C#
// reference); corrected here so the name matches what the engine evaluates. See notes/mission/param-semantics.md R6.
enum class MissionVariableTriggerType : int32_t {
    MissionVariableIsEqual = 1,             // ==
    MissionVariableIsLessThan = 2,          // <
    MissionVariableIsGreaterThan = 3,       // >  (engine sub_type 3)
    MissionVariableIsLessThanOrEqual = 4,   // <= (engine sub_type 4)
    MissionVariableIsGreaterThanOrEqual = 5,// >=
};

// Teammate trigger subtypes
enum class TeammateTriggerType : int32_t {
    TeammateIsEnabled = 1,
    TeammateMedicAssisting = 2,
    TeammateEvacuating = 3,
};

// Player trigger subtypes
enum class PlayerTriggerType : int32_t {
    PlayerBerserk = 18,
    PlayerFirstPerson = 19,
    PlayerThirdPerson = 20,
    PlayerCockpitView = 21,
    PlayerDialogDone = 34,
    PlayerDialogFinished = 35,
    PlayerAwol = 36,
    PlayerSatchel = 37,
    PlayerAttachedToSsn = 38,
    PlayerOnSsn = 39,
    PlayerDrivingSsn = 40,
    PlayerOnGun = 41,
};

// Action types
enum class ActionType : int32_t {
    Null = 0,
    RedirectGroupTo = 1,
    KillGroup = 2,
    ChangeGroupAI = 3,
    VaporizeGroup = 4,
    MisvarChange = 5,
    OutputText = 6,
    PlayWavList = 7,
    BlueWin = 8,
    RedWin = 9,
    GreenWin = 10,
    GroupVelocity = 11,
    AreaAiRed = 12,
    AreaAiBlue = 13,
    SubGoalWon = 14,
    SubGoalLost = 15,
    ChangeGTeamAction = 16,
    ChangeGroupAction = 17,
    GroupTeleportAction = 18,
    RedirectSingleTo = 19,
    KillSingle = 20,
    ChangeSingleAI = 21,
    VaporizeSingle = 22,
    SingleVelocity = 23,
    ChangeSteamAction = 24,
    SingleChangeGroup = 25,
    SingleTeleportAction = 26,
    ParticleEffectAction = 27,
    GroupOpenDoorAction = 30,
    GroupCloseDoorAction = 31,
    GroupResetHasVisited = 32,
    SingleResetHasVisited = 33,
    ResetEvent = 34,
    ShowWinSubgoal = 35,
    ShowLoseSubgoal = 36,
    AttachToEmplaced = 37,
    SetLightState = 38,
    Teammates = 39,
    ShowWaypoints = 40,
    ExecuteWac = 41,
    SsnTargetSsnPri = 42,
    SsnTargetSsnExc = 43,
    SsnTargetGroupPri = 44,
    SsnTargetGroupExc = 45,
    GroupTargetSsnPri = 46,
    GroupTargetSsnExc = 47,
    GroupTargetGroupPri = 48,
    GroupTargetGroupExc = 49,
};

// AI action subtypes — shared by CHANGE_GROUP_AI(3), AREA_AI_RED/BLUE(12/13), CHANGE_SINGLE_AI(21).
// [orig: dfx2med Med_ActionSubTypeName @0x445EE0 / param layout Med_AiSubTypeParams @0x44A920]
// Canonical names + values from the DFX2 mission editor token table (notes/mission/event-grill-dfx2med.md
// section 4). Several names were corrected against the editor and the alert/wpz sub-types added.
enum class AIActionSubType : int32_t {
    GuardBit = 2,
    RedAlert = 5,            // TO_RED_ALERT (added)
    GreenAlert = 6,          // TO_GREEN_ALERT (added)
    Accuracy = 8,
    BlindBit = 15,
    BerserkBit = 16,
    ClimberBit = 17,
    CowardBit = 21,
    YellowAlert = 22,        // TO_YELLOW_ALERT (added)
    DriveSkill = 26,         // was Skill1 — editor DRIVESKILL
    AimSkill = 27,           // was Skill2 — editor AIMSKILL
    AiSetState = 28,         // was AiState — editor AISETSTATE
    CombatSpeed = 29,        // was SpeedKmh1 — editor COMBATSPEED
    PatrolSpeed = 30,        // was SpeedKmh2 — editor PATROLSPEED
    FindAndUse = 31,         // was TargetSsn1 — editor FIND_AND_USE
    AiUseWpz = 32,           // AIUSEWPZ (added)
    AiClearWpz = 33,         // AICLEARWPZ (added)
    PlayPartAnim = 34,       // was AnimNum — editor PLAYPARTANIM (slots ANIMNUM, ANIMPLAYTYPE, ANIMTIME)
    HudItem = 37,            // HUD flash (slots HUDITEM, TICKS)
    TmateStatus = 39,
    AiNodePathBit = 40,
    AttackDistanceValue = 41,
    EngageDistanceMin = 42,  // ENGAGEDISTANCE (slots MIN, MAX)
    IndestructableBit = 43,
    TargetSsn = 44,          // was TargetSsn2 — editor TARGETSSN
    StartFiringBit = 45,
    FiringAngle = 46,
};

// Mission variable action subtypes
enum class MissionVariableActionSubType : int32_t {
    Null = 0,
    Set = 1,
    Add = 2,
    Subtract = 3,
    Increment = 4,
    Decrement = 5,
};

// Teammate action subtypes
enum class TeammateActionSubType : int32_t {
    Null = 0,
    MedicAssist = 1,
    EvacuateTt = 2,
    EvacuateAt = 3,
};

// ============================================================================
// Data Structures
// ============================================================================

#pragma pack(push, 1)

struct Header {
    char magic[4];                     // 'B','M','S', version byte (shipped JO = 19/0x13); gated by kMinVersion
    char mission_name[32];
    char designer[32];
    char terrain[48];                  // [orig editor: 3x char[16] packed by Med_WriteBmsFile @0x44f920 (RAM a1+416/432/448)]
    char default_str[16];
    ClimateType climate;
    AttribFlags attrib_flags;
    uint8_t unknown0[12];
    uint16_t water_override;
    uint32_t unknown1;
    uint16_t fog_override;
    uint8_t fog_color[3];
    uint8_t unknown2;
    uint32_t num_items;
    uint32_t num_buildings;
    uint32_t num_markers;
    uint32_t num_people;
    // The loader does NOT read events using this field; the event count comes from the dedicated
    // 3-count block [orig: EventTrigger_LoadAllData @0x453eb0]. Round-tripped verbatim regardless.
    uint32_t num_events;
    WeatherType weather_type;
    uint8_t win_conditions[8];
    uint8_t lose_conditions[8];
    uint8_t unknown3[16];
    char environment[16];
    uint8_t unknown4[10];
    uint8_t water_color[3];
    uint16_t murk;
    uint8_t something1;
    uint32_t wind_speed;
    uint32_t wind_direction;
    uint8_t unknown5[4];
    uint32_t health;
    uint32_t mana;
    uint32_t music;
    uint32_t reverb;
    char terrain_tile[16];
    char mission_briefing[256];
    int16_t unknown6;
    MissionType mission_type;
    uint8_t max_saves;
    // [orig editor: win_scores[8] + lose_scores[8], each = value/100; Med_WriteBmsFile @0x44f920 Buffer[556..571]]
    uint8_t win_scores[8];
    uint8_t lose_scores[8];
    float map_zoom;
    int16_t area_trigger_count;        // [orig: word_A76410 @hdr+0x240] count of 32-byte area-trigger records
    uint16_t weapon_loadout_chunk_len; // [orig: word_A76412 @hdr+0x242] length of the weapon-loadout chunk
    uint16_t bonus_expiration;
    // [orig: word_A76416 @hdr+0x246] length of a SECOND chunk after the weapon loadout. The engine always
    // seeks past it (Mission_LoadBMSFile @0x40f6d1 MP / @0x40f751 SP); usually 0. Modeled as item_availability.
    uint16_t secondary_chunk_len;
    uint16_t start_time;
    uint16_t minutes_per_day;
    uint8_t unknown9[28];
};

#pragma pack(pop)

static_assert(sizeof(Header) == kHeaderSize, "Header must be 616 bytes");

// Convert a float to the format's signed 16.16 fixed-point, clamping to the int32 range first.
// Positions / zone bounds are stored as int32 = value * 65536, so anything beyond ~±32768 mission
// units would overflow the float->int cast (undefined behavior, typically yields INT32_MIN and
// silently corrupts the record). An editor SpinBox can hand us such a value; clamp defensively.
// kMax is just under INT32_MAX/65536 so the multiply stays < 2^31; kMin = INT32_MIN/65536 exactly.
inline int32_t to_fixed_16_16(float v) {
    constexpr float kMax = 32767.99f;
    constexpr float kMin = -32768.0f;
    if (std::isnan(v)) {
        // NaN compares false against both bounds, so without this it would reach
        // static_cast<int32_t>(NaN) -- undefined behavior. Map it to the origin.
        v = 0.0f;
    } else if (v > kMax) {
        v = kMax;
    } else if (v < kMin) {
        v = kMin;
    }
    return static_cast<int32_t>(v * 65536.0f);
}

// [orig editor: dfx2med.exe. Every offset CONFIRMED byte-exact by the packer Med_PackEntityRecord @0x44c8e0
//  (RAM 448B -> disk 172B); canonical field NAMES come from the .mis text writer Med_WriteMisFile @0x454630
//  (literal keywords). Notes: yaw/pitch/roll stored % 360; w_accuracy1 clamped <= w_accuracy2; iai_name (name1)
//  is from the graphic .def table (not per-entity); ai_textfile = name2; unk42b@166/unk43@168 are NEVER written
//  (zero-filled) -> reserved/pad. write_mis_item in mission.cpp already uses the canonical names below; this is
//  validated against the original .mis writer. The full name table follows inline (2026-06-06 grill).
//  Canonical names: perception2/perfectionist2/wp_distance/wp_adv_trigger/wp_number/w_accuracy1,2/obliqueness/
//  alert_state/map_symbol/team_budget/color_override/max_attack_distance are CORRECT as-is. CORRECTIONS (raw
//  field -> canonical): spawns@62=movetimer; unk19@76=weapon_type(b0)|sweapon_type(b1); unk23/24@84-87=
//  blink_parent_a/b + blink_group_a/b; unk25+unk26@88=group_rel(i32); fire_timer@92=advancetimer;
//  unk30_31@100=wpgoal0..3(4 bytes); unk41@160=next_ssn; gen_string@120 is 31B and @152/154/155 hold
//  grenades/mission_critical/lfp_group.]
struct Entity {
    ItemType type;                     // Set during parsing, not serialized as separate field
    int32_t type_id;
    int32_t name_index;
    int32_t id;
    uint32_t bmsi_attributes;
    int32_t x, y, z;                   // Position in fixed-point 16.16 format
    int32_t wp_distance;
    int32_t perception2;
    int32_t perfectionist2;
    int32_t min_engagement_distance;
    int32_t max_engagement_distance;
    int32_t wp_number;
    int16_t w_accuracy2;
    int16_t w_accuracy1;
    int16_t yaw;
    int16_t pitch;
    int16_t roll;
    int16_t spawns;                    // .mis: movetimer (exposed as spawn_count in the binding — a misnomer)
    uint8_t crouch_timer;              // .mis: crouchtimer (low byte; unk15a is the high byte)
    uint8_t unk15a;
    int16_t shoot_timer;
    int16_t wp_adv_trigger;
    int16_t attention;
    uint8_t alert_state;
    uint8_t team;
    uint8_t no_more_than;
    uint8_t no_less_than;
    int16_t weapon_types;              // .mis: weapon_type (byte0) | sweapon_type (byte1)
    uint8_t group_id;
    uint8_t waypoint_id;
    uint8_t obliqueness;
    uint8_t map_symbol;
    int16_t unk22;                     // @82: not written by the editor (reserved/pad)
    int16_t blink_parent;              // .mis: blink_parent_a (byte0) | blink_parent_b (byte1)
    int16_t blink_group;               // .mis: blink_group_a (byte0) | blink_group_b (byte1)
    int16_t group_rel_lo;              // .mis: group_rel (i32, with group_rel_hi)
    int16_t group_rel_hi;
    int32_t advancetimer;              // .mis: advancetimer (was fire_timer)
    int32_t ttool_index;               // .mis: ttoolindex
    int32_t wp_goals;                  // .mis: wpgoal0..3 (4 packed bytes)
    char name1[8];                     // iai_name
    char name2[8];                     // ai_textfile
    char gen_string[31];               // .mis: gen_string is 31B @120-150
    uint8_t gen_reserved0;             // @151: reserved zero
    uint8_t grenades;                  // @152: .mis grenades
    uint8_t ref_num;                   // @153: registration/group id -> entity+533 refNum (was
                                       // "gen_reserved1" — the engine reads it [orig:
                                       // Entity_SpawnFromBMSRecord @0x40e9f0]; streamed as the 0x10
                                       // static record's flag-0x40 byte, §5.9/D-NET-94; zero in the
                                       // shipped corpus)
    uint8_t mission_critical;          // @154: .mis mission_critical
    uint8_t lfp_group;                 // @155: .mis lfp_group
    int32_t max_attack_distance;
    int32_t next_ssn;                  // .mis: next_ssn (was unk41)
    uint8_t color_override;
    uint8_t team_budget;
    int16_t unk42b;                    // @166: not written by the editor (reserved/pad)
    int32_t unk43;                     // @168: not written by the editor (reserved/pad)

    // --- .mis-interchange-only TRANSIENT fields (NOT serialized into .bms bytes) -----------
    // The 172-byte BMS entity record (kEntitySize) has no such fields; they are MED/Nile
    // text-authoring concepts that exist only in the .mis metafile. parse_entity/write_entity
    // (bms.cpp) serialize the record field-by-field and never touch these; equal() compares
    // them (they change write_mis_text output, so a difference is a real document change).
    // Semantics: a plain .mis item z is the editor-frame (terrain-relative) height;
    // height_lock nonzero declares z ABSOLUTE with extra_bheight carrying the baked base
    // height under the item, so the original editor recovers the relative offset as
    //   scene Y = z/65536 - (height_lock ? extra_bheight/65536 : 0)
    // [orig: MisLdr_ParseMisLine @ 0x100017b0 (extra_bheight->rec+292, height_lock->rec+356);
    //  MisLdr_WriteNileProjectXml @ 0x10004930 (<ABSOLUTE>TRUE</ABSOLUTE> iff height_lock);
    //  both misldr.dll]. BMS entity z is always absolute (bms-event-runtime-re.md §6.6), so
    // the .bms parse path and freshly authored entities mark height_lock = 1; .mis-parsed
    // entities round-trip their own values. See docs/mission/mis-format-re.md (D-MIS-4).
    // Layout invariant: Entity stays TRIVIAL -- no NSDMIs; every construction site value-initializes,
    // which zeroes all bytes including padding -- and carries no implicit tail padding (the explicit
    // mis_pad_ tail), because bms::equal byte-compares whole records; static_asserts below enforce both.
    int32_t mis_extra_bheight;         // .mis extra_bheight: baked base height, 16.16 fixed
    uint8_t mis_height_lock;           // .mis height_lock: nonzero => z ABSOLUTE + bheight baked
    uint8_t mis_pad_[3];               // explicit tail pad: keeps sizeof(Entity) free of implicit
                                       // padding so bms::equal's memcmp is deterministic (always
                                       // zero via value-init; never serialized)

    // Accessors for float positions (fixed-point 16.16 conversion)
    float get_x() const { return x / 65536.0f; }
    float get_y() const { return y / 65536.0f; }
    float get_z() const { return z / 65536.0f; }
    void set_x(float v) { x = to_fixed_16_16(v); }
    void set_y(float v) { y = to_fixed_16_16(v); }
    void set_z(float v) { z = to_fixed_16_16(v); }

    BmsiAttributeFlags get_ai_flags() const {
        return static_cast<BmsiAttributeFlags>(bmsi_attributes);
    }
};

static_assert(std::is_trivial<Entity>::value,
              "bms::Entity must stay trivial (no NSDMIs/user ctors): bms::equal memcmps whole records "
              "and relies on value-init zeroing + trivial copies preserving all bytes");
static_assert(std::is_trivially_copyable<Entity>::value,
              "bms::Entity must stay trivially copyable for bms::equal's byte compare");
static_assert(offsetof(Entity, mis_pad_) + sizeof(Entity::mis_pad_) == sizeof(Entity),
              "no implicit tail padding after the explicit .mis pad -- extend mis_pad_ (or re-lay the "
              "tail) when appending fields, or bms::equal's memcmp reads indeterminate bytes");

struct WaypointRecord {
    WaypointFlags flags;
    uint32_t marker_count;
    std::vector<uint32_t> waypoint_numbers;
    std::vector<uint8_t> padding;      // Remaining bytes after waypoint numbers
};

// [orig editor: Med_WriteBmsFile @0x44f920 packs each 32-byte group as: @0 flags (bit0/bit1 from the editor
//  group flags), @4=0, @8 = a value, @12 = constant 10, @16..28 = 0. The JO loader keeps @0/@8/@12; @12 is
//  the literal 10, @0 a 2-bit flags, @8 the only free int (2026-06-06 grill).]
struct GroupRecord {
    int32_t flags;
    int32_t value;
};

// [orig editor: Med_WriteBmsFile @0x44f920 copies the editor LAYER NAME (a string) into this 20-byte record.
//  JO reads-and-discards it, but the editor-authored bytes are a fixed-width name, not an opaque payload.]
struct LayerRecord {
    char name[kLayerRecordSize];
};

// 32-byte area-trigger / restriction-zone record.
// [orig: read raw into unk_A32D10 by Mission_LoadBMSFile @0x40fc45 (no field interpretation at load).
//  The byte layout comes from the bounds-check consumers: Entity_IsTeamInTriggerBounds @0x43c75c,
//  Entity_IsBmsRefInTriggerBounds @0x43e53b, Entity_IsLocalPlayerOutOfBounds @0x439d40 (zone_ptr =
//  &unk_A32D14 = base+4; zone_ptr[6]&1 is the @28 flags). Bounds are INTERLEAVED per axis
//  (x_min,x_max,y_min,y_max,z_min,z_max) — NOT min-triple/max-triple — and there is NO Y/Z swap.]
// [orig editor: dfx2med.exe AREA_TRIGGERS dialog Med_AreaTriggerDialogProc @0x40f400 confirms off-0 = the
//  designer zone id 1..99 (zones listed/addressed by id, `unk_221DEC4 + 312*id`, markers store it at
//  entity+144) and flags bit0 = "MISSION_AREA" boundary (CheckDlgButton 1175), bit1 = constrain-Z
//  (CheckDlgButton 1169) (2026-06-06 grill).]
struct AreaTrigger {
    int32_t id;                        // off 0: designer zone id 1..99 (editor-confirmed; markers reference it)
    int32_t x_min, x_max;              // off 4, 8   Fixed-point 16.16
    int32_t y_min, y_max;              // off 12, 16
    int32_t z_min, z_max;              // off 20, 24
    uint32_t flags;                    // off 28: bit0x01 = MISSION_AREA (mission boundary; read only by the
                                       //   boundary check, NOT *IsWithinArea); bit0x02 = constrain-Z (else Z unbounded)

    // [orig: when flags&0x02 is clear, the consumers use Z in [-1073741824, 0x40000000] = ±16384.0 (16.16)]
    static constexpr float kUnboundedZMin = -16384.0f;
    static constexpr float kUnboundedZMax = 16384.0f;

    // Accessors for float bounds
    float get_x_min() const { return x_min / 65536.0f; }
    float get_x_max() const { return x_max / 65536.0f; }
    float get_y_min() const { return y_min / 65536.0f; }
    float get_y_max() const { return y_max / 65536.0f; }
    float get_z_min() const { return z_min / 65536.0f; }
    float get_z_max() const { return z_max / 65536.0f; }
    // bit0 is the MISSION_AREA / mission-boundary flag (editor label, see flags note above), not a generic
    // "enabled" toggle; *IsWithinArea trigger zones ignore it. Accessor name kept to avoid an editor/binding
    // ripple.
    static constexpr uint32_t kFlagMissionArea = 0x1u; // editor "MISSION_AREA" checkbox 1175
    static constexpr uint32_t kFlagConstrainZ = 0x2u;  // editor constrain-Z checkbox 1169
    bool is_active() const { return (flags & kFlagMissionArea) != 0; }
    bool constrains_z() const { return (flags & kFlagConstrainZ) != 0; }
};

// [orig: EventTrigger_UpdateEntry @0x454c30; passes: PreMission = UpdateAllWithFlag2 @0x454dc0
//  (flags&2, mission-start context), PostMission = UpdateAllWithFlag4 @0x454e00 (flags&4,
//  debrief/video contexts), normal = quarter-list round-robin @0x454d50 gated to every 16th tick
//  in Server_TickUpdate @0x51d7e0]
// Runtime: reset_after/delay value (top 10 bits) becomes reload = value<<6, decremented 64 per
// processing pass. A normal event is processed once per 64 ticks (16-tick gate x 4 quarters), so
// one authored unit amortizes to 64 ticks (~1.02 s @ 62 Hz). delay = wait after conditions pass
// before actions run; reset_after = re-arm wait for a repeating event. flags bit0=ResetAfter
// (repeat; else fire-once), bit1=PreMission, bit2=PostMission (select the pass).
struct Event {
    EventFlags flags;
    int32_t trigger_index;
    int32_t action_index;
    int32_t reset_after;               // Upper 10 bits of raw 32-bit value (value << 22)
    int32_t delay;                     // Upper 10 bits of raw 32-bit value (value << 22)
    uint8_t unknown5;                  // [orig:] runtime ACTIVE/has-fired flag; 0 on disk; ResetEvent clears it
    uint8_t trigger_count;
    uint8_t action_count;
    uint8_t unknown6;                  // [orig:] never read by evaluator/scheduler => reserved
};

// [orig: EventTrigger_EvaluateCondition @0x453620 reads param1..4 as triggerParams[3..6]]
// Per-type param meaning (group/entity/zone/var/event refs, thresholds, distances) in
// notes/mission/param-semantics.md. *IsWithinArea (sub 10): param2 = area-trigger ARRAY INDEX, param1 = tested
// group/entity. Single distance subtypes (43-45): param3 = whole meters (engine uses param3<<16).
struct Trigger {
    int32_t condition_flags;
    TriggerMainType main_type;
    int32_t sub_type;
    int32_t param1;
    int32_t param2;
    int32_t param3;
    int32_t param4;
    int32_t unknown7;                  // [orig:] never read by evaluator => reserved

    // [orig: condition fold sub_454050 @0x454050] Each trigger's result is negated by ITS bit0; the combine
    // operator (or/xor/else and) is taken from the PREVIOUS trigger, i.e. these bits control how the NEXT
    // trigger joins. Last trigger's or/xor bits are unused; zero triggers => TRUE.
    static constexpr int32_t kConditionNegated = 0x1;
    static constexpr int32_t kConditionOr = 0x2;
    static constexpr int32_t kConditionXor = 0x4;
    bool is_negated() const { return (condition_flags & kConditionNegated) != 0; }
    bool is_or() const { return (condition_flags & kConditionOr) != 0; }
    bool is_xor() const { return (condition_flags & kConditionXor) != 0; }

    std::string get_logic_operator() const {
        if (is_or()) return "or";
        if (is_xor()) return "xor";
        return "and";
    }
};

// [orig: EventAction_Dispatch @0x4542e0 — switch(action_type) reads param1..4 as actionEntry[3..6]]
// Per-type param meaning in notes/mission/param-semantics.md. MisvarChange (5): action_sub_type
// 1=Set/2=Add/3=Sub/4=Inc/5=Dec on dword_C6B240[param1] with param2. ResetEvent (34): clears events[param1]
// active flag. reserved0/reserved1 unused by the dispatcher.
struct Action {
    int32_t reserved0;
    ActionType action_type;
    int32_t action_sub_type;
    int32_t param1;
    int32_t param2;
    int32_t param3;
    int32_t param4;
    int32_t reserved1;
};

struct BoundingBox {
    int32_t min_x, min_y, min_z;       // Fixed-point 16.16
    int32_t max_x, max_y, max_z;
    int32_t type;                      // shipped values: 1 or 5
    int32_t ref_id;                    // shipped values: -2/-1 or a positive marker/entity id
    int32_t reserved0;                 // always zero in the shipped corpus

    // Accessors for float bounds
    float get_min_x() const { return min_x / 65536.0f; }
    float get_min_y() const { return min_y / 65536.0f; }
    float get_min_z() const { return min_z / 65536.0f; }
    float get_max_x() const { return max_x / 65536.0f; }
    float get_max_y() const { return max_y / 65536.0f; }
    float get_max_z() const { return max_z / 65536.0f; }
};

// One weapon-loadout chunk tuple, kept as the four raw chunk strings so unusual authored
// text round-trips byte-exactly. The format is the engine-wide kit tuple
// {name\0 ammoPri\0 ammoSec\0 flags\0} [orig: restrictionData @ 0x24D4E00, sanitized on SP
// load by AIProfile_SanitizeConfigData @ 0x40cfe0; net-re §5.63]: ammo_primary/ammo_secondary
// are requested clip counts (-1 = the weapon's default fill), and flags is the per-ammo
// damage-class request byte (1 = x0.9, 2 = x1.1, every other value neutral).
struct WeaponLoadoutRecord {
    std::string name;
    std::string ammo_primary;
    std::string ammo_secondary;
    std::string flags = "-1";
};

struct WeaponLoadout {
    std::vector<WeaponLoadoutRecord> entries;
};

struct ItemAvailabilityEntry {
    std::string name;
    uint8_t status = 0;
};

// ============================================================================
// Mission File
// ============================================================================

struct File {
    Header header;
    WeaponLoadout loadout;
    // [orig: word_A76416 @hdr+0x246 bytes] second chunk after the weapon loadout (the engine seeks past it).
    // [orig editor: Med_WriteBmsFile @0x44f920 builds it via Med_BuildItemAvailabilityChunk @0x432c00 — an
    //  ITEM-AVAILABILITY list: for each enabled entry in the item/weapon table, one record = name\0 + 1 status
    //  byte, terminated by an empty name. Sibling of the weapon-loadout chunk. Usually empty in
    //  shipped JO missions.]
    std::vector<ItemAvailabilityEntry> item_availability;
    std::vector<Entity> items;
    std::vector<Entity> buildings;
    std::vector<Entity> markers;
    std::vector<Entity> organics;
    std::vector<WaypointRecord> waypoint_records;
    std::vector<GroupRecord> group_records;
    std::vector<LayerRecord> layer_records;
    std::vector<AreaTrigger> area_triggers;
    std::vector<Event> events;
    std::vector<Trigger> triggers;
    std::vector<Action> actions;
    std::vector<BoundingBox> bounding_boxes;

    // Counts read from file (for validation)
    int32_t events_count;
    int32_t trigger_count;
    int32_t action_count;
    int32_t bounding_box_count;

    // Helper to get all entities
    std::vector<const Entity*> all_entities() const;

    // Get mission name as string
    std::string get_mission_name() const;
    std::string get_designer() const;
    std::string get_terrain() const;
};

// ============================================================================
// API Functions
// ============================================================================

// Parse a BMS file from a byte buffer.
bool parse(const uint8_t* data, size_t size, File& out, std::string& error);

// Parse a BMS file from disk.
bool parse_file(const std::string& path, File& out, std::string& error);

// Write a BMS file to a byte buffer.
bool write(const File& file, std::vector<uint8_t>& out, std::string& error);

// Write only the canonical 616-byte BMS header that the retail game-session
// loader consumes before loading the mission body.
bool encode_header_blob(const File& file, std::vector<uint8_t>& out, std::string& error);

// Write a BMS file to disk.
bool write_file(const File& file, const std::string& path, std::string& error);

// Check if data starts with BMS magic.
bool is_bms(const uint8_t* data, size_t size);

// Value-equality of two in-memory missions: true iff they would serialize (write()) to the same
// bytes. The editor's undo / dirty tracking uses this to decide whether an edit changed anything,
// without round-tripping through the byte serializer. The fixed record structs are TRIVIAL, are
// value-initialized at every construction site (zeroing every byte, padding included), and carry
// no implicit tail padding (Entity ends in the explicit mis_pad_), so the byte-wise compare is
// exact and platform-deterministic -- and because the types are trivial, snapshots/copies
// (EditHistory) preserve all bytes, padding included. WaypointRecord (inner vectors) compares
// field-wise. NOTE: if you add a field that write() serializes, add it to equal() too.
// The Entity byte-compare also covers the .mis-interchange transient fields (mis_extra_bheight /
// mis_height_lock): they never reach .bms bytes, but they do change write_mis_text output, so a
// difference between them is a real document change for undo/dirty purposes.
bool equal(const File& a, const File& b);

} // namespace opennova::bms
