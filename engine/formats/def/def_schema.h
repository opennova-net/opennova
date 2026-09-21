#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace opennova::def {

enum class DefRecordKind { Item, Weapon, Ammo, Action, Sight, Attachment, Effect, Carry };
enum class DefFieldType { Integer, Unsigned, Byte, Count, Real, Text };
enum class DefReference { None, Model, AnimationMap, Ammo, Weapon, Item, Texture, Sound, Particle, AiProfile, GameText, OtherText };
using DefValue = std::variant<int64_t, double, std::string>;

struct DefChoice { const char *name; int64_t value; };

// A field addresses an actual native record member. The same description is used
// for equality checks, serialization and authoring; no editor data model exists.
struct DefField {
	std::string id;
	size_t offset = 0;
	size_t width = 0;
	DefFieldType type = DefFieldType::Integer;
	DefReference reference = DefReference::None;
	std::vector<DefChoice> choices;
	bool flags = false;
	bool read_only = false;
};

enum class DefEncoding {
	Plain, Fixed16, FixedSeconds, TurnRate, Degrees, HalfDegrees, ScaledInteger,
	ScaledReal, Delay, ItemType, AmmoKillZone, WeaponFlags, AmmoFlags,
	ItemAttrib, ItemAttrib2, ItemParent, WeaponClass, CharacterFilter,
	TeamFilter, FloatFixed, ScopeParallax, Heat, LightImpact, LightMove,
	ClassRounds, Pose, ItemDeathTime, DoorType, DoorOpenRate, DoorMaxAngle, HuskSwap,
	HuskSeconds, DeathPieces, SpawnMask, Function, ParticleSlot, Sight, Attachment,
};

// A property describes one authored line (possibly repeated for flags/lists).
// Numeric factors are the witnessed parser's conversions, inverted by the writer.
struct DefProperty {
	std::string key;
	std::vector<std::string> fields;
	DefEncoding encoding = DefEncoding::Plain;
	double factor = 1.0;
	std::string present_field;
};

const std::vector<DefField> &def_fields(DefRecordKind kind);
const std::vector<DefProperty> &def_properties(DefRecordKind kind);
const DefField *def_field(DefRecordKind kind, const std::string &id);
DefValue def_get(const void *record, const DefField &field);
bool def_set(void *record, const DefField &field, const DefValue &value, std::string &error);
void def_sync_derived(DefRecordKind kind, void *record, const std::string &field);
size_t def_record_size(DefRecordKind kind);
void def_init_record(DefRecordKind kind, void *record);

} // namespace opennova::def
