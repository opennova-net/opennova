#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace opennova::def {

// The records the tables describe: the item, weapon and ammo tables' rows and what they hold, a
// weapon table's carry limits, and a powerup table's rows, their ammo rows and the action blocks a
// row holds (pickup and respawn alike).
enum class DefRecordKind {
	Item, Weapon, Ammo, Action, Sight, Attachment, Effect, Carry, Powerup, PowerupAmmo, PowerupAction,
};
inline constexpr size_t kDefRecordKindCount = size_t(DefRecordKind::PowerupAction) + 1;
enum class DefFieldType { Integer, Unsigned, Byte, Count, Real, Text };
enum class DefReference { None, Model, AnimationMap, Ammo, Weapon, Item, Texture, Sound, Particle, AiProfile, GameText, OtherText, UserPoint, Powerup };
using DefValue = std::variant<int64_t, double, std::string>;

// A value a field takes by name: the token the file writes (or the number's name) and what
// an editor shows for it ("" = the name).
struct DefChoice { const char *name; int64_t value; const char *label = ""; };

// A field addresses an actual native record member. The same description is used
// for equality checks, serialization and authoring; no editor data model exists.
// What a member is beyond its storage (derived, ranged, what it names, the tokens it
// takes) comes from the rule rows beside the member inventory (def_schema.cpp), applied
// in their order, each a family's line in a table: a new family adds rows, never a branch.
struct DefField {
	std::string id;
	size_t offset = 0;
	size_t width = 0;
	DefFieldType type = DefFieldType::Integer;
	DefReference reference = DefReference::None;
	std::vector<DefChoice> choices;
	bool flags = false;
	bool read_only = false;
	bool open = false; // the choices are the values the parser knows; it reads any other too
	// The range the game's reader keeps the member to (a weapon's category and rank, an item's
	// unit_type byte): a set outside it is refused (`ranged`, min..max inclusive).
	bool ranged = false;
	int64_t min = 0, max = 0;
	// A text the member cannot hold, compared without case as its parser compares it: its line,
	// written, reads back as another (a powerup's weapon named `all` is every weapon); a set of it
	// is refused with `refused_why`. "" = none.
	const char *refused = "";
	const char *refused_why = "";
};

enum class DefEncoding {
	Plain, Fixed16, FixedSeconds, TurnRate, Degrees, HalfDegrees, ScaledInteger,
	ScaledReal, Delay, ItemType, AmmoKillZone, WeaponFlags, AmmoFlags,
	ItemAttrib, ItemAttrib2, ItemParent, WeaponClass, CharacterFilter,
	TeamFilter, FloatFixed, ScopeParallax, Heat, LightImpact, LightMove,
	ClassRounds, Pose, ItemDeathTime, DoorType, DoorOpenRate, DoorMaxAngle, HuskSwap,
	HuskSeconds, DeathPieces, SpawnMask, Function, ParticleSlot, Sight, Attachment,
	Percent,    // an integer percentage read with atoi, clamped 0..100, stored as float * 0.01f
	ShotTiming, // a region's flag name plus two second counts in ticks; region 0 without a
	            // name is what `particletesttime` authors
	Switch,     // a key alone, its member 1 (powerup.def's `allammo`): written while it is not 0
	PowerupWeapon, // powerup.def's `weapon <name>` or `weapon all`: a name, then the all flag
};

// A property describes one authored line (possibly repeated for flags/lists).
// Numeric factors are the witnessed parser's conversions, inverted by the writer.
// What an editor says of the line comes only from what its parser witnesses: a name for
// each member ("" = none), the heading the parser groups it under, the unit each member's
// number is written in (one entry serves every member; "" = none), and a note on what the
// parser makes of the number.
struct DefProperty {
	std::string key;
	std::vector<std::string> fields;
	DefEncoding encoding = DefEncoding::Plain;
	double factor = 1.0;
	std::string present_field;
	std::vector<std::string> labels;
	std::string section;
	std::vector<std::string> units;
	std::string note;
};

const std::vector<DefField> &def_fields(DefRecordKind kind);
const std::vector<DefProperty> &def_properties(DefRecordKind kind);
const DefField *def_field(DefRecordKind kind, const std::string &id);
// The property whose line writes the member `id`, and the member's place among its fields.
const DefProperty *def_member_property(DefRecordKind kind, const std::string &id, size_t *index = nullptr);

// A member written as a number of its own on its property's line, in the units the file
// writes it (ADR 0046 S12): the encodings with one number per member (Fixed16, FixedSeconds,
// TurnRate, Degrees, HalfDegrees, ScaledInteger, ScaledReal, Percent, Heat, Pose, a light's
// radius and fade, a shot timing's two times). A text member, a light's colour and every
// other encoding have none (None): an editor shows them as they are stored.
enum class DefAuthored { None, Integer, Real };

// A member as the kind's tables describe it, found once by its id (def_member): its native field,
// the property whose line writes it and its place on that line, the line's members in its order,
// the number the line writes it as, and the line's present flag (null: always written). What an
// editor keeps for a field, so a set of it looks nothing up again.
struct DefMember {
	DefRecordKind kind = DefRecordKind::Item;
	const DefField *field = nullptr;
	const DefProperty *property = nullptr;
	size_t index = 0;
	std::vector<const DefField *> line;
	DefAuthored authored = DefAuthored::None;
	const DefField *present = nullptr;
	explicit operator bool() const { return field != nullptr; }
};
DefMember def_member(DefRecordKind kind, const std::string &id);

// The member's number as the writer puts it on its line (the line written or not): an
// integer or a real by its authored type. False for a member with none, and for a stored word
// the line, written alone and read back, does not keep (an overflowed turn rate, an unset fade
// the parser reads as 10 ticks): an editor shows that one as stored.
bool def_authored_get(const DefMember &member, const void *record, DefValue &out);
// The member's number set as the file would write it: the record's line rewritten with the
// new number (its shortest decimal; a whole number within the 32 bits the parser reads), read
// back through the family's parser, and that line's members copied back, so what a line's
// order decides (an acceleration's default deceleration) is the parser's; refused, the record
// untouched, where the writer could not write the result back. A number the member already
// writes changes nothing, as does, for a member shown as stored, its stored number.
bool def_authored_set(const DefMember &member, void *record, const DefValue &value, std::string &error);
// What the authored reads and sets of the calling thread have run the family's parser over so far: a
// line written alone (def_authored_get's check that its line keeps the stored word) and a whole record
// written (def_authored_set's two read-backs). What a keystroke in an authored member costs, which the
// editor's tests count; a count, never a cache.
struct DefAuthoredParses {
	size_t lines = 0;
	size_t records = 0;
};
DefAuthoredParses def_authored_parses();
DefValue def_get(const void *record, const DefField &field);
bool def_set(void *record, const DefField &field, const DefValue &value, std::string &error);
void def_sync_derived(DefRecordKind kind, void *record, const std::string &field);
size_t def_record_size(DefRecordKind kind);
void def_init_record(DefRecordKind kind, void *record);

} // namespace opennova::def
