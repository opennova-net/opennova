#include <formats/mission/mission_field.h>

// The rows of every mission record's fields (mission_field.h). The rules each setter keeps came from
// the setters by name they replace (bms_edit's header and entity setters, the event record's apply),
// with their reasons.

#include "mission_detail.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <limits>

namespace opennova::mission {

using namespace detail;

namespace {

constexpr int64_t kI16Min = std::numeric_limits<int16_t>::min();
constexpr int64_t kI16Max = std::numeric_limits<int16_t>::max();
constexpr int64_t kI32Min = std::numeric_limits<int32_t>::min();
constexpr int64_t kI32Max = std::numeric_limits<int32_t>::max();
constexpr int64_t kU8Max = std::numeric_limits<uint8_t>::max();
constexpr int64_t kU16Max = std::numeric_limits<uint16_t>::max();
constexpr int64_t kU32Max = std::numeric_limits<uint32_t>::max();

// reset_after / delay are stored in the upper 10 bits of their u32 slot (bms.cpp write_event packs
// them << 22, parse reads >> 22), so the representable value range is 0..1023.
constexpr int64_t kMaxEventDelayTicks = 1023;

// --- the values ------------------------------------------------------------------------------------

bool integer_of(const MissionValue &value, int64_t &out, std::string &error) {
	if (const int64_t *integer = std::get_if<int64_t>(&value)) {
		out = *integer;
		return true;
	}
	error = "This field takes a whole number.";
	return false;
}

bool real_of(const MissionValue &value, double &out, std::string &error) {
	if (const double *real = std::get_if<double>(&value)) {
		out = *real;
		return true;
	}
	if (const int64_t *integer = std::get_if<int64_t>(&value)) {
		out = double(*integer);
		return true;
	}
	error = "This field takes a number.";
	return false;
}

const std::string *text_of(const MissionValue &value, std::string &error) {
	if (const std::string *text = std::get_if<std::string>(&value)) return text;
	error = "This field takes a text.";
	return nullptr;
}

template <class R> const R &as(const void *record) { return *static_cast<const R *>(record); }
template <class R> R &as(void *record) { return *static_cast<R *>(record); }

// A number member, read as it is stored (an enum by its value).
template <class R, class T, T R::*Member>
bool get_number(const void *record, MissionValue &out) {
	out = static_cast<int64_t>(as<R>(record).*Member);
	return true;
}
// A number member written as the setters by name wrote it: cast to its width (the editor keeps a
// value inside the field's range before it gets here).
template <class R, class T, T R::*Member>
bool set_cast(void *record, const MissionValue &value, std::string &error) {
	int64_t number = 0;
	if (!integer_of(value, number, error)) return false;
	as<R>(record).*Member = static_cast<T>(number);
	return true;
}
// A byte member that saturates: an out-of-range value from a programmatic caller clamps rather than
// silently wrapping (map_symbol 300 would otherwise write 44).
template <class R, uint8_t R::*Member>
bool set_clamp_u8(void *record, const MissionValue &value, std::string &error) {
	int64_t number = 0;
	if (!integer_of(value, number, error)) return false;
	as<R>(record).*Member = static_cast<uint8_t>(std::clamp<int64_t>(number, 0, kU8Max));
	return true;
}

// A fixed-width on-disk text slot (`Size` bytes, the whole slot or its first part): read to its first
// NUL or its end, written with copy_fixed_field, which keeps every byte of the slot (a name a shipped
// mission fills completely loses no byte to a forced NUL) and zero-pads a shorter text.
template <class R, size_t N, char (R::*Slot)[N], size_t Size = N>
bool get_slot(const void *record, MissionValue &out) {
	out = fixed_string(as<R>(record).*Slot, Size);
	return true;
}
template <class R, size_t N, char (R::*Slot)[N], size_t Size = N>
bool set_slot(void *record, const MissionValue &value, std::string &error) {
	const std::string *text = text_of(value, error);
	if (!text) return false;
	copy_fixed_field(as<R>(record).*Slot, Size, *text);
	return true;
}

// A 16.16 position or bound, read and written exactly in mission units (bms::to_fixed_16_16's clamp).
template <class R, int32_t R::*Member>
bool get_fixed(const void *record, MissionValue &out) {
	out = double(as<R>(record).*Member) / 65536.0;
	return true;
}
template <class R, int32_t R::*Member>
bool set_fixed(void *record, const MissionValue &value, std::string &error) {
	double real = 0.0;
	if (!real_of(value, real, error)) return false;
	as<R>(record).*Member = bms::to_fixed_16_16(real);
	return true;
}

// A loadout entry's string, which the chunk ends with a NUL: a NUL inside it would end it early.
template <std::string bms::WeaponLoadoutRecord::*Member>
bool get_loadout(const void *record, MissionValue &out) {
	out = as<bms::WeaponLoadoutRecord>(record).*Member;
	return true;
}
bool loadout_text(const MissionValue &value, const std::string *&text, std::string &error) {
	text = text_of(value, error);
	if (!text) return false;
	if (text->find('\0') != std::string::npos) {
		error = "A weapon loadout string ends at its first NUL; it cannot hold one.";
		return false;
	}
	return true;
}
template <std::string bms::WeaponLoadoutRecord::*Member>
bool set_loadout(void *record, const MissionValue &value, std::string &error) {
	const std::string *text = nullptr;
	if (!loadout_text(value, text, error)) return false;
	as<bms::WeaponLoadoutRecord>(record).*Member = *text;
	return true;
}

// --- the choices -------------------------------------------------------------------------------------

constexpr MissionChoice kClimates[] = {{"Desert", 0}, {"Jungle", 1}, {"Snow", 2}};
constexpr MissionChoice kWeathers[] = {{"NiceDay", 0}, {"Rainy", 1}, {"Snow", 2}};
constexpr MissionChoice kMissionTypes[] = {{"NormalMission", 1}, {"CombatVehicleMission", 2}, {"TenthMountain", 3}};
constexpr MissionChoice kAttribFlags[] = {
	{"WaterOverrideEnable", 0x1},     {"FogDistanceOverrideEnable", 0x2}, {"FogColorOverrideEnable", 0x4},
	{"WeatherOverrideEnable", 0x8},   {"ForceIndoors", 0x10},             {"RotateMap180", 0x20},
	{"SinglePlayerRespawn", 0x40},    {"AdvanceAndSecure", 0x10000},      {"ConquerAndControl", 0x20000},
	{"EnableNVG", 0x100000},          {"StartWithNVGOn", 0x400000},       {"AttackAndDefend", 0x800000},
	{"Coop", 0x1000000},              {"Deathmatch", 0x2000000},          {"KingOfTheHill", 0x4000000},
	{"FlagBall", 0x8000000},          {"CaptureTheFlag", 0x10000000},     {"TeamDeathmatch", 0x20000000},
	{"TeamKingOfTheHill", 0x40000000}, {"SearchAndDestroy", 0x80000000ll},
};
constexpr MissionChoice kAiAttributes[] = {
	{"Blind", 1 << 0},          {"Guarding", 1 << 1},       {"RemoveIfLessThan", 1 << 4},
	{"RemoveIfMoreThan", 1 << 5}, {"SinglePlayerOnly", 1 << 6}, {"MultiplayerOnly", 1 << 7},
	{"Berserk", 1 << 11},       {"FlyingOrganic", 1 << 14}, {"Coward", 1 << 16},
	{"EngineRunning", 1 << 17}, {"AdvancedAmmo", 1 << 18},  {"Indestructible", 1 << 21},
	{"NavigationWaypoint", 1 << 22}, {"Reflective", 1 << 23}, {"NoShadow", 1 << 24},
};
constexpr MissionChoice kWaypointFlags[] = {{"DoesNotLoop", 1 << 0}, {"BlueTeam", 1 << 1}, {"RedTeam", 1 << 2}};
constexpr MissionChoice kAreaFlags[] = {{"MissionArea", bms::AreaTrigger::kFlagMissionArea},
                                        {"ConstrainZ", bms::AreaTrigger::kFlagConstrainZ}};
constexpr MissionChoice kEventFlags[] = {{"ResetAfter", 1 << 0}, {"PreMission", 1 << 1}, {"PostMission", 1 << 2}};
constexpr MissionChoice kConditionFlags[] = {{"Negated", bms::Trigger::kConditionNegated},
                                             {"Or", bms::Trigger::kConditionOr},
                                             {"Xor", bms::Trigger::kConditionXor}};
constexpr MissionChoice kTriggerMainTypes[] = {
	{"Group", 1},           {"Single", 2},   {"Event", 3},  {"MissionVariable", 4},
	{"SecondTimeThrough", 5}, {"Teammate", 6}, {"Player", 7},
};
constexpr MissionChoice kActionTypes[] = {
	{"Null", 0},                   {"RedirectGroupTo", 1},       {"KillGroup", 2},
	{"ChangeGroupAI", 3},          {"VaporizeGroup", 4},         {"MisvarChange", 5},
	{"OutputText", 6},             {"PlayWavList", 7},           {"BlueWin", 8},
	{"RedWin", 9},                 {"GreenWin", 10},             {"GroupVelocity", 11},
	{"AreaAiRed", 12},             {"AreaAiBlue", 13},           {"SubGoalWon", 14},
	{"SubGoalLost", 15},           {"ChangeGTeamAction", 16},    {"ChangeGroupAction", 17},
	{"GroupTeleportAction", 18},   {"RedirectSingleTo", 19},     {"KillSingle", 20},
	{"ChangeSingleAI", 21},        {"VaporizeSingle", 22},       {"SingleVelocity", 23},
	{"ChangeSteamAction", 24},     {"SingleChangeGroup", 25},    {"SingleTeleportAction", 26},
	{"ParticleEffectAction", 27},  {"SpecialSubType", 28},       {"GroupOpenDoorAction", 30},
	{"GroupCloseDoorAction", 31},  {"GroupResetHasVisited", 32}, {"SingleResetHasVisited", 33},
	{"ResetEvent", 34},            {"ShowWinSubgoal", 35},       {"ShowLoseSubgoal", 36},
	{"AttachToEmplaced", 37},      {"SetLightState", 38},        {"Teammates", 39},
	{"ShowWaypoints", 40},         {"ExecuteWac", 41},           {"SsnTargetSsnPri", 42},
	{"SsnTargetSsnExc", 43},       {"SsnTargetGroupPri", 44},    {"SsnTargetGroupExc", 45},
	{"GroupTargetSsnPri", 46},     {"GroupTargetSsnExc", 47},    {"GroupTargetGroupPri", 48},
	{"GroupTargetGroupExc", 49},
};

// Each choice the value of its enum in bms.h, so the names here are those of the values the format
// reads (bms.h stays the one place the values are).
static_assert(kClimates[2].value == int64_t(bms::ClimateType::Snow) && kWeathers[1].value == int64_t(bms::WeatherType::Rainy));
static_assert(kMissionTypes[2].value == int64_t(bms::MissionType::TenthMountain));
static_assert(kAttribFlags[19].value == int64_t(uint32_t(bms::AttribFlags::SearchAndDestroy)) &&
              kAttribFlags[4].value == int64_t(uint32_t(bms::AttribFlags::ForceIndoors)));
static_assert(kAiAttributes[14].value == int64_t(uint32_t(bms::BmsiAttributeFlags::NoShadow)) &&
              kAiAttributes[9].value == int64_t(uint32_t(bms::BmsiAttributeFlags::EngineRunning)));
static_assert(kWaypointFlags[2].value == int64_t(uint32_t(bms::WaypointFlags::RedTeam)));
static_assert(kEventFlags[2].value == int64_t(uint32_t(bms::EventFlags::PostMission)));
static_assert(kTriggerMainTypes[6].value == int64_t(bms::TriggerMainType::Player));
static_assert(kActionTypes[28].value == int64_t(bms::ActionType::SpecialSubType) &&
              kActionTypes[29].value == int64_t(bms::ActionType::GroupOpenDoorAction) &&
              kActionTypes[std::size(kActionTypes) - 1].value == int64_t(bms::ActionType::GroupTargetGroupExc));

#define MISSION_CHOICES(rows) rows, std::size(rows)

// --- the header ----------------------------------------------------------------------------------------

using H = bms::Header;

// header.terrain[48] is three 16-byte fixed slots: terrain@+0, cnv_file@+16, tt_file@+32 (see
// mission_mis_writer.cpp's write_mis_general_information). The field is the first slot alone, so a
// terrain edit does not zero-fill (and lose) the cnv_file / tt_file references; the read is bounded to
// it so a full slot never bleeds into cnv_file.
constexpr size_t kTerrainSlot = 16;

// water_override is s16 half-world-units, stored in an unsigned word; it only takes effect when
// attrib_flags WaterOverrideEnable (0x1) is set.
bool get_water_override(const void *record, MissionValue &out) {
	out = int64_t(static_cast<int16_t>(as<H>(record).water_override));
	return true;
}

bool get_map_zoom(const void *record, MissionValue &out) {
	out = double(as<H>(record).map_zoom);
	return true;
}
bool set_map_zoom(void *record, const MissionValue &value, std::string &error) {
	double real = 0.0;
	if (!real_of(value, real, error)) return false;
	as<H>(record).map_zoom = float(real);
	return true;
}

// The header's fields, by the keys of the setters by name they replace. The name, the designer and the
// briefing are fixed-width slots read back at full width (mission_info), so they are copied with
// copy_fixed_field, never a NUL-forcing copy; environment[16] likewise.
const MissionField kHeaderFields[] = {
	{MissionRecord::Header, "mission_name", MissionFieldType::Text, sizeof(H::mission_name), 0, 0, nullptr, 0, false,
	 get_slot<H, 32, &H::mission_name>, set_slot<H, 32, &H::mission_name>},
	{MissionRecord::Header, "designer", MissionFieldType::Text, sizeof(H::designer), 0, 0, nullptr, 0, false,
	 get_slot<H, 32, &H::designer>, set_slot<H, 32, &H::designer>},
	{MissionRecord::Header, "briefing", MissionFieldType::Text, sizeof(H::mission_briefing), 0, 0, nullptr, 0, false,
	 get_slot<H, 256, &H::mission_briefing>, set_slot<H, 256, &H::mission_briefing>},
	{MissionRecord::Header, "terrain", MissionFieldType::Text, kTerrainSlot, 0, 0, nullptr, 0, false,
	 get_slot<H, 48, &H::terrain, kTerrainSlot>, set_slot<H, 48, &H::terrain, kTerrainSlot>},
	{MissionRecord::Header, "environment", MissionFieldType::Text, sizeof(H::environment), 0, 0, nullptr, 0, false,
	 get_slot<H, 16, &H::environment>, set_slot<H, 16, &H::environment>},
	{MissionRecord::Header, "climate", MissionFieldType::Integer, 0, 0, kU32Max, MISSION_CHOICES(kClimates), false,
	 get_number<H, bms::ClimateType, &H::climate>, set_cast<H, bms::ClimateType, &H::climate>},
	{MissionRecord::Header, "weather", MissionFieldType::Integer, 0, 0, kU32Max, MISSION_CHOICES(kWeathers), false,
	 get_number<H, bms::WeatherType, &H::weather_type>, set_cast<H, bms::WeatherType, &H::weather_type>},
	{MissionRecord::Header, "mission_type", MissionFieldType::Integer, 0, 0, kU8Max, MISSION_CHOICES(kMissionTypes),
	 false, get_number<H, bms::MissionType, &H::mission_type>, set_cast<H, bms::MissionType, &H::mission_type>},
	{MissionRecord::Header, "attrib_flags", MissionFieldType::Integer, 0, 0, kU32Max, MISSION_CHOICES(kAttribFlags),
	 true, get_number<H, bms::AttribFlags, &H::attrib_flags>, set_cast<H, bms::AttribFlags, &H::attrib_flags>},
	{MissionRecord::Header, "start_time", MissionFieldType::Integer, 0, 0, kU16Max, nullptr, 0, false,
	 get_number<H, uint16_t, &H::start_time>, set_cast<H, uint16_t, &H::start_time>},
	{MissionRecord::Header, "minutes_per_day", MissionFieldType::Integer, 0, 0, kU16Max, nullptr, 0, false,
	 get_number<H, uint16_t, &H::minutes_per_day>, set_cast<H, uint16_t, &H::minutes_per_day>},
	{MissionRecord::Header, "player_health", MissionFieldType::Integer, 0, 0, kU32Max, nullptr, 0, false,
	 get_number<H, uint32_t, &H::health>, set_cast<H, uint32_t, &H::health>},
	{MissionRecord::Header, "max_saves", MissionFieldType::Integer, 0, 0, kU8Max, nullptr, 0, false,
	 get_number<H, uint8_t, &H::max_saves>, set_cast<H, uint8_t, &H::max_saves>},
	{MissionRecord::Header, "music", MissionFieldType::Integer, 0, 0, kU32Max, nullptr, 0, false,
	 get_number<H, uint32_t, &H::music>, set_cast<H, uint32_t, &H::music>},
	{MissionRecord::Header, "reverb", MissionFieldType::Integer, 0, 0, kU32Max, nullptr, 0, false,
	 get_number<H, uint32_t, &H::reverb>, set_cast<H, uint32_t, &H::reverb>},
	{MissionRecord::Header, "wind_speed", MissionFieldType::Integer, 0, 0, kU32Max, nullptr, 0, false,
	 get_number<H, uint32_t, &H::wind_speed>, set_cast<H, uint32_t, &H::wind_speed>},
	{MissionRecord::Header, "wind_direction", MissionFieldType::Integer, 0, 0, kU32Max, nullptr, 0, false,
	 get_number<H, uint32_t, &H::wind_direction>, set_cast<H, uint32_t, &H::wind_direction>},
	{MissionRecord::Header, "water_override", MissionFieldType::Integer, 0, kI16Min, kI16Max, nullptr, 0, false,
	 get_water_override, set_cast<H, uint16_t, &H::water_override>},
	// Fog distance in world units; only takes effect when attrib_flags FogDistanceOverrideEnable (0x2) is set.
	{MissionRecord::Header, "fog_override", MissionFieldType::Integer, 0, 0, kU16Max, nullptr, 0, false,
	 get_number<H, uint16_t, &H::fog_override>, set_cast<H, uint16_t, &H::fog_override>},
	{MissionRecord::Header, "map_zoom", MissionFieldType::Real, 0, 0, 0, nullptr, 0, false, get_map_zoom,
	 set_map_zoom},
};

// --- an entity -------------------------------------------------------------------------------------------

using E = bms::Entity;

constexpr uint32_t kKnownAiAttributeMask =
	static_cast<uint32_t>(bms::BmsiAttributeFlags::Blind) |
	static_cast<uint32_t>(bms::BmsiAttributeFlags::Guarding) |
	static_cast<uint32_t>(bms::BmsiAttributeFlags::RemoveIfLessThan) |
	static_cast<uint32_t>(bms::BmsiAttributeFlags::RemoveIfMoreThan) |
	static_cast<uint32_t>(bms::BmsiAttributeFlags::SinglePlayerOnly) |
	static_cast<uint32_t>(bms::BmsiAttributeFlags::MultiplayerOnly) |
	static_cast<uint32_t>(bms::BmsiAttributeFlags::Berserk) |
	static_cast<uint32_t>(bms::BmsiAttributeFlags::FlyingOrganic) |
	static_cast<uint32_t>(bms::BmsiAttributeFlags::Coward) |
	static_cast<uint32_t>(bms::BmsiAttributeFlags::EngineRunning) |
	static_cast<uint32_t>(bms::BmsiAttributeFlags::AdvancedAmmo) |
	static_cast<uint32_t>(bms::BmsiAttributeFlags::Indestructible) |
	static_cast<uint32_t>(bms::BmsiAttributeFlags::NavigationWaypoint) |
	static_cast<uint32_t>(bms::BmsiAttributeFlags::Reflective) |
	static_cast<uint32_t>(bms::BmsiAttributeFlags::NoShadow);

// ai_flags refuses a bit past the known attribute mask.
bool set_ai_flags(void *record, const MissionValue &value, std::string &error) {
	int64_t number = 0;
	if (!integer_of(value, number, error)) return false;
	if ((static_cast<uint32_t>(number) & ~kKnownAiAttributeMask) != 0) {
		error = "Mission entity AI flags include unsupported bits";
		return false;
	}
	as<E>(record).bmsi_attributes = static_cast<uint32_t>(number);
	return true;
}

// The items.def id a record names (its type_id + kItemIdOffset): what the record is, which an Add
// names; shown only.
bool get_item(const void *record, MissionValue &out) {
	out = int64_t(bms_type_id_to_item_id(as<E>(record).type_id));
	return true;
}

// The entity's fields: the editable int properties by the binding's keys (the uint8-backed ones clamp),
// the fixed 8-byte AI class / AI script slots (name1 = iai_name, name2 = ai_textfile, copied at full
// width: a NUL-forcing copy would truncate an 8-char name at byte 7 on every edit), and its transform
// (the 16.16 position in mission units, the integer-degree eulers).
const MissionField kEntityFields[] = {
	{MissionRecord::Entity, "item", MissionFieldType::Integer, 0, kI32Min, kI32Max, nullptr, 0, false, get_item,
	 nullptr},
	{MissionRecord::Entity, "x", MissionFieldType::Real, 0, 0, 0, nullptr, 0, false, get_fixed<E, &E::x>,
	 set_fixed<E, &E::x>},
	{MissionRecord::Entity, "y", MissionFieldType::Real, 0, 0, 0, nullptr, 0, false, get_fixed<E, &E::y>,
	 set_fixed<E, &E::y>},
	{MissionRecord::Entity, "z", MissionFieldType::Real, 0, 0, 0, nullptr, 0, false, get_fixed<E, &E::z>,
	 set_fixed<E, &E::z>},
	{MissionRecord::Entity, "pitch", MissionFieldType::Integer, 0, kI16Min, kI16Max, nullptr, 0, false,
	 get_number<E, int16_t, &E::pitch>, set_cast<E, int16_t, &E::pitch>},
	{MissionRecord::Entity, "yaw", MissionFieldType::Integer, 0, kI16Min, kI16Max, nullptr, 0, false,
	 get_number<E, int16_t, &E::yaw>, set_cast<E, int16_t, &E::yaw>},
	{MissionRecord::Entity, "roll", MissionFieldType::Integer, 0, kI16Min, kI16Max, nullptr, 0, false,
	 get_number<E, int16_t, &E::roll>, set_cast<E, int16_t, &E::roll>},
	{MissionRecord::Entity, "group", MissionFieldType::Integer, 0, 0, kU8Max, nullptr, 0, false,
	 get_number<E, uint8_t, &E::group_id>, set_clamp_u8<E, &E::group_id>},
	{MissionRecord::Entity, "waypoint_id", MissionFieldType::Integer, 0, 0, kU8Max, nullptr, 0, false,
	 get_number<E, uint8_t, &E::waypoint_id>, set_clamp_u8<E, &E::waypoint_id>},
	{MissionRecord::Entity, "wp_number", MissionFieldType::Integer, 0, kI32Min, kI32Max, nullptr, 0, false,
	 get_number<E, int32_t, &E::wp_number>, set_cast<E, int32_t, &E::wp_number>},
	{MissionRecord::Entity, "team", MissionFieldType::Integer, 0, 0, kU8Max, nullptr, 0, false,
	 get_number<E, uint8_t, &E::team>, set_clamp_u8<E, &E::team>},
	{MissionRecord::Entity, "lfp_group", MissionFieldType::Integer, 0, 0, kU8Max, nullptr, 0, false,
	 get_number<E, uint8_t, &E::lfp_group>, set_clamp_u8<E, &E::lfp_group>},
	{MissionRecord::Entity, "ai_flags", MissionFieldType::Integer, 0, 0, kU32Max, MISSION_CHOICES(kAiAttributes),
	 true, get_number<E, uint32_t, &E::bmsi_attributes>, set_ai_flags},
	{MissionRecord::Entity, "perception", MissionFieldType::Integer, 0, kI32Min, kI32Max, nullptr, 0, false,
	 get_number<E, int32_t, &E::perception2>, set_cast<E, int32_t, &E::perception2>},
	{MissionRecord::Entity, "accuracy", MissionFieldType::Integer, 0, kI16Min, kI16Max, nullptr, 0, false,
	 get_number<E, int16_t, &E::w_accuracy1>, set_cast<E, int16_t, &E::w_accuracy1>},
	{MissionRecord::Entity, "alert_state", MissionFieldType::Integer, 0, 0, kU8Max, nullptr, 0, false,
	 get_number<E, uint8_t, &E::alert_state>, set_clamp_u8<E, &E::alert_state>},
	{MissionRecord::Entity, "min_engagement_distance", MissionFieldType::Integer, 0, kI32Min, kI32Max, nullptr, 0,
	 false, get_number<E, int32_t, &E::min_engagement_distance>, set_cast<E, int32_t, &E::min_engagement_distance>},
	{MissionRecord::Entity, "max_engagement_distance", MissionFieldType::Integer, 0, kI32Min, kI32Max, nullptr, 0,
	 false, get_number<E, int32_t, &E::max_engagement_distance>, set_cast<E, int32_t, &E::max_engagement_distance>},
	{MissionRecord::Entity, "max_attack_distance", MissionFieldType::Integer, 0, kI32Min, kI32Max, nullptr, 0, false,
	 get_number<E, int32_t, &E::max_attack_distance>, set_cast<E, int32_t, &E::max_attack_distance>},
	// .mis movetimer (spawn_count is the binding's name for it).
	{MissionRecord::Entity, "spawn_count", MissionFieldType::Integer, 0, kI16Min, kI16Max, nullptr, 0, false,
	 get_number<E, int16_t, &E::spawns>, set_cast<E, int16_t, &E::spawns>},
	// no_more_than (byte 74), paired with the RemoveIfMoreThan AI flag.
	{MissionRecord::Entity, "max_simultaneous", MissionFieldType::Integer, 0, 0, kU8Max, nullptr, 0, false,
	 get_number<E, uint8_t, &E::no_more_than>, set_clamp_u8<E, &E::no_more_than>},
	{MissionRecord::Entity, "no_less_than", MissionFieldType::Integer, 0, 0, kU8Max, nullptr, 0, false,
	 get_number<E, uint8_t, &E::no_less_than>, set_clamp_u8<E, &E::no_less_than>},
	{MissionRecord::Entity, "map_symbol", MissionFieldType::Integer, 0, 0, kU8Max, nullptr, 0, false,
	 get_number<E, uint8_t, &E::map_symbol>, set_clamp_u8<E, &E::map_symbol>},
	{MissionRecord::Entity, "name1", MissionFieldType::Text, sizeof(E::name1), 0, 0, nullptr, 0, false,
	 get_slot<E, 8, &E::name1>, set_slot<E, 8, &E::name1>},
	{MissionRecord::Entity, "name2", MissionFieldType::Text, sizeof(E::name2), 0, 0, nullptr, 0, false,
	 get_slot<E, 8, &E::name2>, set_slot<E, 8, &E::name2>},
};

// --- a waypoint path, a group, a layer, an area trigger ---------------------------------------------------

using W = bms::WaypointRecord;

// A path's flags. An authored edit of a path rewrites its record, so a stored marker count above its
// 32 slots (CP19.bms ships 39) no longer describes it: the count resyncs to the slots the path holds,
// as every authored waypoint edit does (resize_waypoint_padding), or the engine would walk phantom
// waypoints.
bool set_waypoint_flags(void *record, const MissionValue &value, std::string &error) {
	int64_t number = 0;
	if (!integer_of(value, number, error)) return false;
	W &path = as<W>(record);
	path.flags = static_cast<bms::WaypointFlags>(static_cast<uint32_t>(number));
	resize_waypoint_padding(path, /*preserve_over_count=*/false);
	return true;
}

const MissionField kWaypointPathFields[] = {
	{MissionRecord::WaypointPath, "flags", MissionFieldType::Integer, 0, 0, kU32Max, MISSION_CHOICES(kWaypointFlags),
	 true, get_number<W, bms::WaypointFlags, &W::flags>, set_waypoint_flags},
};

using G = bms::GroupRecord;

// [orig editor: Med_WriteBmsFile @0x44f920 packs each 32-byte group as: @0 flags (bit0/bit1 from the
// editor group flags), @8 the one free int, @12 the constant 10 the writer emits.] The flags take bits
// 0 and 1 alone.
bool set_group_flags(void *record, const MissionValue &value, std::string &error) {
	int64_t number = 0;
	if (!integer_of(value, number, error)) return false;
	if ((number & ~int64_t(0x3)) != 0) {
		error = "Group flags may only use bits 0 and 1";
		return false;
	}
	as<G>(record).flags = static_cast<int32_t>(number);
	return true;
}

const MissionField kGroupFields[] = {
	{MissionRecord::Group, "flags", MissionFieldType::Integer, 0, 0, 3, nullptr, 0, false,
	 get_number<G, int32_t, &G::flags>, set_group_flags},
	{MissionRecord::Group, "value", MissionFieldType::Integer, 0, kI32Min, kI32Max, nullptr, 0, false,
	 get_number<G, int32_t, &G::value>, set_cast<G, int32_t, &G::value>},
};

using L = bms::LayerRecord;

// [orig editor: Med_WriteBmsFile @0x44f920 copies the editor LAYER NAME into the 20-byte record] a
// fixed-width name JO reads and discards.
const MissionField kLayerFields[] = {
	{MissionRecord::Layer, "name", MissionFieldType::Text, bms::kLayerRecordSize, 0, 0, nullptr, 0, false,
	 get_slot<L, bms::kLayerRecordSize, &L::name>, set_slot<L, bms::kLayerRecordSize, &L::name>},
};

using A = bms::AreaTrigger;

// [orig: the bounds consumers Entity_IsTeamInTriggerBounds @0x43c75c, Entity_IsBmsRefInTriggerBounds
// @0x43e53b, Entity_IsLocalPlayerOutOfBounds @0x439d40 read the bounds interleaved per axis and the @28
// flags; dfx2med's AREA_TRIGGERS dialog Med_AreaTriggerDialogProc @0x40f400 the zone id 1..99 at @0 and
// the MISSION_AREA (bit0) and constrain-Z (bit1) checkboxes.] The flags word keeps its other bits.
const MissionField kAreaFields[] = {
	{MissionRecord::Area, "id", MissionFieldType::Integer, 0, kI32Min, kI32Max, nullptr, 0, false,
	 get_number<A, int32_t, &A::id>, set_cast<A, int32_t, &A::id>},
	{MissionRecord::Area, "x_min", MissionFieldType::Real, 0, 0, 0, nullptr, 0, false, get_fixed<A, &A::x_min>,
	 set_fixed<A, &A::x_min>},
	{MissionRecord::Area, "x_max", MissionFieldType::Real, 0, 0, 0, nullptr, 0, false, get_fixed<A, &A::x_max>,
	 set_fixed<A, &A::x_max>},
	{MissionRecord::Area, "y_min", MissionFieldType::Real, 0, 0, 0, nullptr, 0, false, get_fixed<A, &A::y_min>,
	 set_fixed<A, &A::y_min>},
	{MissionRecord::Area, "y_max", MissionFieldType::Real, 0, 0, 0, nullptr, 0, false, get_fixed<A, &A::y_max>,
	 set_fixed<A, &A::y_max>},
	{MissionRecord::Area, "z_min", MissionFieldType::Real, 0, 0, 0, nullptr, 0, false, get_fixed<A, &A::z_min>,
	 set_fixed<A, &A::z_min>},
	{MissionRecord::Area, "z_max", MissionFieldType::Real, 0, 0, 0, nullptr, 0, false, get_fixed<A, &A::z_max>,
	 set_fixed<A, &A::z_max>},
	{MissionRecord::Area, "flags", MissionFieldType::Integer, 0, 0, kU32Max, MISSION_CHOICES(kAreaFlags), true,
	 get_number<A, uint32_t, &A::flags>, set_cast<A, uint32_t, &A::flags>},
};

// --- the event logic -----------------------------------------------------------------------------------------

using V = bms::Event;

// The author's bits (ResetAfter, PreMission, PostMission) and the internal 0x10/0x20 shipped missions
// use: the internal ones the event holds are kept, a bit past the known ones dropped [orig: dfx2med
// Med_EventDialogCommit @0x4118d0 sets bits 0/1/2 only and leaves the rest].
bool set_event_flags(void *record, const MissionValue &value, std::string &error) {
	int64_t number = 0;
	if (!integer_of(value, number, error)) return false;
	V &event = as<V>(record);
	const uint32_t preserved_internal = static_cast<uint32_t>(event.flags) & bms::kEventInternalFlagMask;
	const uint32_t requested_known = static_cast<uint32_t>(number) & bms::kEventKnownFlagMask;
	event.flags = static_cast<bms::EventFlags>(preserved_internal | requested_known);
	return true;
}

// reset_after / delay occupy only the upper 10 bits on disk, so the value range is 0..1023: an
// out-of-range value clamps here at the library boundary, or it would wrap on serialize (2000 << 22
// truncates and reparses as 976) with no error.
template <int32_t V::*Member>
bool set_event_ticks(void *record, const MissionValue &value, std::string &error) {
	int64_t number = 0;
	if (!integer_of(value, number, error)) return false;
	as<V>(record).*Member = static_cast<int32_t>(std::clamp<int64_t>(number, 0, kMaxEventDelayTicks));
	return true;
}

const MissionField kEventFields[] = {
	{MissionRecord::Event, "flags", MissionFieldType::Integer, 0, 0, kU32Max, MISSION_CHOICES(kEventFlags), true,
	 get_number<V, bms::EventFlags, &V::flags>, set_event_flags},
	{MissionRecord::Event, "reset_after", MissionFieldType::Integer, 0, 0, kMaxEventDelayTicks, nullptr, 0, false,
	 get_number<V, int32_t, &V::reset_after>, set_event_ticks<&V::reset_after>},
	{MissionRecord::Event, "delay", MissionFieldType::Integer, 0, 0, kMaxEventDelayTicks, nullptr, 0, false,
	 get_number<V, int32_t, &V::delay>, set_event_ticks<&V::delay>},
};

using T = bms::Trigger;

// [orig: EventTrigger_EvaluateCondition @0x453620 reads param1..4; the condition fold
// EventTrigger_EvaluateChain @0x454050 negates each result by its bit0 and joins the next by its or /
// xor bits.]
const MissionField kTriggerFields[] = {
	{MissionRecord::Trigger, "condition_flags", MissionFieldType::Integer, 0, kI32Min, kI32Max,
	 MISSION_CHOICES(kConditionFlags), true, get_number<T, int32_t, &T::condition_flags>,
	 set_cast<T, int32_t, &T::condition_flags>},
	{MissionRecord::Trigger, "main_type", MissionFieldType::Integer, 0, kI32Min, kI32Max,
	 MISSION_CHOICES(kTriggerMainTypes), false, get_number<T, bms::TriggerMainType, &T::main_type>,
	 set_cast<T, bms::TriggerMainType, &T::main_type>},
	{MissionRecord::Trigger, "sub_type", MissionFieldType::Integer, 0, kI32Min, kI32Max, nullptr, 0, false,
	 get_number<T, int32_t, &T::sub_type>, set_cast<T, int32_t, &T::sub_type>},
	{MissionRecord::Trigger, "param1", MissionFieldType::Integer, 0, kI32Min, kI32Max, nullptr, 0, false,
	 get_number<T, int32_t, &T::param1>, set_cast<T, int32_t, &T::param1>},
	{MissionRecord::Trigger, "param2", MissionFieldType::Integer, 0, kI32Min, kI32Max, nullptr, 0, false,
	 get_number<T, int32_t, &T::param2>, set_cast<T, int32_t, &T::param2>},
	{MissionRecord::Trigger, "param3", MissionFieldType::Integer, 0, kI32Min, kI32Max, nullptr, 0, false,
	 get_number<T, int32_t, &T::param3>, set_cast<T, int32_t, &T::param3>},
	{MissionRecord::Trigger, "param4", MissionFieldType::Integer, 0, kI32Min, kI32Max, nullptr, 0, false,
	 get_number<T, int32_t, &T::param4>, set_cast<T, int32_t, &T::param4>},
};

using X = bms::Action;

// [orig: EventAction_Dispatch @0x4542e0 switches on the action type and reads param1..4.]
const MissionField kActionFields[] = {
	{MissionRecord::Action, "action_type", MissionFieldType::Integer, 0, kI32Min, kI32Max,
	 MISSION_CHOICES(kActionTypes), false, get_number<X, bms::ActionType, &X::action_type>,
	 set_cast<X, bms::ActionType, &X::action_type>},
	{MissionRecord::Action, "action_sub_type", MissionFieldType::Integer, 0, kI32Min, kI32Max, nullptr, 0, false,
	 get_number<X, int32_t, &X::action_sub_type>, set_cast<X, int32_t, &X::action_sub_type>},
	{MissionRecord::Action, "param1", MissionFieldType::Integer, 0, kI32Min, kI32Max, nullptr, 0, false,
	 get_number<X, int32_t, &X::param1>, set_cast<X, int32_t, &X::param1>},
	{MissionRecord::Action, "param2", MissionFieldType::Integer, 0, kI32Min, kI32Max, nullptr, 0, false,
	 get_number<X, int32_t, &X::param2>, set_cast<X, int32_t, &X::param2>},
	{MissionRecord::Action, "param3", MissionFieldType::Integer, 0, kI32Min, kI32Max, nullptr, 0, false,
	 get_number<X, int32_t, &X::param3>, set_cast<X, int32_t, &X::param3>},
	{MissionRecord::Action, "param4", MissionFieldType::Integer, 0, kI32Min, kI32Max, nullptr, 0, false,
	 get_number<X, int32_t, &X::param4>, set_cast<X, int32_t, &X::param4>},
};

// --- a weapon loadout entry ------------------------------------------------------------------------------

using O = bms::WeaponLoadoutRecord;

// The .bms loadout chunk serializes an empty name as a leading NUL, which the loader reads as the
// chunk's end: a nameless entry cannot be stored and would drop it and every entry after it.
bool set_loadout_name(void *record, const MissionValue &value, std::string &error) {
	const std::string *text = nullptr;
	if (!loadout_text(value, text, error)) return false;
	if (text->empty()) {
		error = "Weapon loadout entries require a name";
		return false;
	}
	as<O>(record).name = *text;
	return true;
}

// The per-ammo damage-class request [orig: g_SpawnLoadoutBuffer @ 0x24D4E00, sanitized on SP load by
// AIProfile_SanitizeConfigData @ 0x40cfe0]: an empty one is written as "-1", the neutral default.
bool set_loadout_flags(void *record, const MissionValue &value, std::string &error) {
	const std::string *text = nullptr;
	if (!loadout_text(value, text, error)) return false;
	as<O>(record).flags = text->empty() ? std::string("-1") : *text;
	return true;
}

const MissionField kLoadoutFields[] = {
	{MissionRecord::Loadout, "name", MissionFieldType::Text, 0, 0, 0, nullptr, 0, false, get_loadout<&O::name>,
	 set_loadout_name},
	{MissionRecord::Loadout, "ammo_primary", MissionFieldType::Text, 0, 0, 0, nullptr, 0, false,
	 get_loadout<&O::ammo_primary>, set_loadout<&O::ammo_primary>},
	{MissionRecord::Loadout, "ammo_secondary", MissionFieldType::Text, 0, 0, 0, nullptr, 0, false,
	 get_loadout<&O::ammo_secondary>, set_loadout<&O::ammo_secondary>},
	{MissionRecord::Loadout, "flags", MissionFieldType::Text, 0, 0, 0, nullptr, 0, false, get_loadout<&O::flags>,
	 set_loadout_flags},
};

#undef MISSION_CHOICES

// Every record's rows, in MissionRecord's order.
const MissionFields kRecords[] = {
	{kHeaderFields, std::size(kHeaderFields)},       {kEntityFields, std::size(kEntityFields)},
	{kWaypointPathFields, std::size(kWaypointPathFields)}, {kGroupFields, std::size(kGroupFields)},
	{kLayerFields, std::size(kLayerFields)},         {kAreaFields, std::size(kAreaFields)},
	{kEventFields, std::size(kEventFields)},         {kTriggerFields, std::size(kTriggerFields)},
	{kActionFields, std::size(kActionFields)},       {kLoadoutFields, std::size(kLoadoutFields)},
};
static_assert(std::size(kRecords) == kMissionRecordCount, "one row set per mission record");

} // namespace

MissionFields mission_fields(MissionRecord record) {
	const size_t at = size_t(record);
	return at < kMissionRecordCount ? kRecords[at] : MissionFields{};
}

const MissionField *find_mission_field(MissionRecord record, const std::string &key) {
	for (const MissionField &field : mission_fields(record))
		if (key == field.key) return &field;
	return nullptr;
}

const char *mission_choice_name(const MissionChoice *choices, size_t count, int64_t value) {
	for (size_t i = 0; i < count; ++i)
		if (choices[i].value == value) return choices[i].name;
	return nullptr;
}

MissionChoices trigger_main_types() { return {kTriggerMainTypes, std::size(kTriggerMainTypes)}; }
MissionChoices action_types() { return {kActionTypes, std::size(kActionTypes)}; }

} // namespace opennova::mission
