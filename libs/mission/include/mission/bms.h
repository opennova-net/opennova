// BMS mission file parser (NovaLogic's Binary Mission format).
// Used for mission files (.bms) in Delta Force and related games.
#pragma once

#include <cmath>
#include <cstdint>
#include <string>
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

// Mission attribute flags
enum class AttribFlags : uint32_t {
    None = 0,
    WaterOverrideEnable = 0x1,
    FogDistanceOverrideEnable = 0x2,
    FogColorOverrideEnable = 0x4,
    WeatherOverrideEnable = 0x8,
    RotateMap180 = 0x20,
    SinglePlayerRespawn = 0x40,
    AdvanceAndSecure = 0x10000,
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

// Event flags
enum class EventFlags : uint32_t {
    None = 0,
    ResetAfter = 1,
    PreMission = 2,
    PostMission = 4,
    Unknown4 = 16,
    Unknown5 = 32,
};

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

// AI action subtypes
enum class AIActionSubType : int32_t {
    GuardBit = 2,
    Accuracy = 8,
    BlindBit = 15,
    BerserkBit = 16,
    ClimberBit = 17,
    CowardBit = 21,
    Skill1 = 26,
    Skill2 = 27,
    AiState = 28,
    SpeedKmh1 = 29,
    SpeedKmh2 = 30,
    TargetSsn1 = 31,
    AnimNum = 34,
    HudItem = 37,
    TmateStatus = 39,
    AiNodePathBit = 40,
    AttackDistanceValue = 41,
    EngageDistanceMin = 42,
    IndestructableBit = 43,
    TargetSsn2 = 44,
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
    char terrain[48];
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
    uint8_t unknown7[16];
    float map_zoom;
    int16_t area_trigger_count;        // [orig: word_A76410 @hdr+0x240] count of 32-byte area-trigger records
    uint16_t weapon_loadout_chunk_len; // [orig: word_A76412 @hdr+0x242] length of the weapon-loadout chunk
    uint16_t bonus_expiration;
    // [orig: word_A76416 @hdr+0x246] length of a SECOND chunk after the weapon loadout. The engine always
    // seeks past it (Mission_LoadBMSFile @0x40f6d1 MP / @0x40f751 SP); usually 0. Its bytes live in File::secondary_chunk.
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
    int16_t spawns;
    uint8_t crouch_timer;
    uint8_t unk15a;
    int16_t shoot_timer;
    int16_t wp_adv_trigger;
    int16_t attention;
    uint8_t alert_state;
    uint8_t team;
    uint8_t no_more_than;
    uint8_t no_less_than;
    int16_t unk19;
    uint8_t group_id;
    uint8_t waypoint_id;
    uint8_t obliqueness;
    uint8_t map_symbol;
    int16_t unk22;
    int16_t unk23;
    int16_t unk24;
    int16_t unk25;
    int16_t unk26;
    int32_t fire_timer;
    int32_t ttool_index;
    int32_t unk30_31;
    char name1[8];                     // iai_name
    char name2[8];                     // ai_textfile
    char gen_string[36];
    int32_t max_attack_distance;
    int32_t unk41;
    uint8_t color_override;
    uint8_t team_budget;
    int16_t unk42b;
    int32_t unk43;

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

struct WaypointRecord {
    WaypointFlags flags;
    uint32_t marker_count;
    std::vector<uint32_t> waypoint_numbers;
    std::vector<uint8_t> padding;      // Remaining bytes after waypoint numbers
};

struct GroupRecord {
    uint8_t raw_data[kGroupRecordSize];
};

struct LayerRecord {
    uint8_t raw_data[kLayerRecordSize];
};

// 32-byte area-trigger / restriction-zone record.
// [orig: read raw into unk_A32D10 by Mission_LoadBMSFile @0x40fc45 (no field interpretation at load).
//  The byte layout comes from the bounds-check consumers: Entity_IsTeamInTriggerBounds @0x43c75c,
//  Entity_IsBmsRefInTriggerBounds @0x43e53b, Entity_IsLocalPlayerOutOfBounds @0x439d40 (zone_ptr =
//  &unk_A32D14 = base+4; zone_ptr[6]&1 is the @28 flags). Bounds are INTERLEAVED per axis
//  (x_min,x_max,y_min,y_max,z_min,z_max) — NOT min-triple/max-triple — and there is NO Y/Z swap.]
struct AreaTrigger {
    int32_t id;                        // off 0: not read by the bounds checks (likely a zone id); Phase-5 RE
    int32_t x_min, x_max;              // off 4, 8   Fixed-point 16.16
    int32_t y_min, y_max;              // off 12, 16
    int32_t z_min, z_max;              // off 20, 24
    uint32_t flags;                    // off 28: bit0x01 = zone active; bit0x02 = constrain-Z (else Z unbounded)

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
    bool is_active() const { return (flags & 0x1u) != 0; }
    bool constrains_z() const { return (flags & 0x2u) != 0; }
};

// [orig: EventTrigger_UpdateEntry @0x454c30; per-frame passes UpdateAllWithFlag2/4 @0x454dc0/0x454e00]
// Runtime: reset_after/delay value (top 10 bits) becomes reload = value*64, decremented 64/tick => value
// = TICKS. delay = wait after conditions pass before actions run; reset_after = re-arm wait for a repeating
// event. flags bit0=ResetAfter (repeat; else fire-once), bit1=PreMission, bit2=PostMission (gate the pass).
struct Event {
    EventFlags flags;
    int32_t trigger_index;
    int32_t action_index;
    int32_t reset_after;               // Upper 10 bits of raw 32-bit value (value << 22); = ticks at runtime
    int32_t delay;                     // Upper 10 bits of raw 32-bit value (value << 22); = ticks at runtime
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
    bool is_negated() const { return (condition_flags & 1) != 0; }
    bool is_or() const { return (condition_flags & 2) != 0; }
    bool is_xor() const { return (condition_flags & 4) != 0; }

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
    uint8_t unknown_data[12];

    // Accessors for float bounds
    float get_min_x() const { return min_x / 65536.0f; }
    float get_min_y() const { return min_y / 65536.0f; }
    float get_min_z() const { return min_z / 65536.0f; }
    float get_max_x() const { return max_x / 65536.0f; }
    float get_max_y() const { return max_y / 65536.0f; }
    float get_max_z() const { return max_z / 65536.0f; }
};

struct WeaponLoadout {
    std::vector<uint8_t> raw_data;
};

// ============================================================================
// Mission File
// ============================================================================

struct File {
    Header header;
    WeaponLoadout loadout;
    // [orig: word_A76416 @hdr+0x246 bytes] opaque chunk the engine seeks past after the loadout.
    // Preserved verbatim for round-trip fidelity; almost always empty in shipped missions.
    std::vector<uint8_t> secondary_chunk;
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

// Write a BMS file to disk.
bool write_file(const File& file, const std::string& path, std::string& error);

// Check if data starts with BMS magic.
bool is_bms(const uint8_t* data, size_t size);

// Value-equality of two in-memory missions: true iff they would serialize (write()) to the same
// bytes. The editor's undo / dirty tracking uses this to decide whether an edit changed anything,
// without round-tripping through the byte serializer. The fixed record structs are trivially
// copyable and always value-initialized, so they compare byte-wise; WaypointRecord (inner vectors)
// compares field-wise. NOTE: if you add a field that write() serializes, add it to equal() too.
bool equal(const File& a, const File& b);

} // namespace opennova::bms
