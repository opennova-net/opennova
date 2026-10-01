#include "def_schema.h"
#include "def.h"
#include "def_scan.h"
#include "def_write_record.h"
#include <algorithm>
#include <array>
#include <iterator>

#include <cmath>
#include <cstring>
#include <limits>

namespace opennova::def {

const DefField *def_field(DefRecordKind kind, const std::string &id) {
	for (const DefField &field : def_fields(kind)) if (field.id == id) return &field;
	return nullptr;
}

DefValue def_get(const void *record, const DefField &field) {
	const auto *p = static_cast<const uint8_t *>(record) + field.offset;
	switch (field.type) {
	case DefFieldType::Integer: return int64_t(*reinterpret_cast<const int32_t *>(p));
	case DefFieldType::Unsigned: return int64_t(*reinterpret_cast<const uint32_t *>(p));
	case DefFieldType::Byte: return int64_t(*p);
	case DefFieldType::Count: return int64_t(*reinterpret_cast<const size_t *>(p));
	case DefFieldType::Real: return double(*reinterpret_cast<const float *>(p));
	case DefFieldType::Text: {
		size_t n = 0;
		while (n < field.width && p[n] != 0) ++n;
		return std::string(reinterpret_cast<const char *>(p), n);
	}
	}
	return int64_t(0);
}

bool def_set(void *record, const DefField &field, const DefValue &value, std::string &error) {
	auto *p = static_cast<uint8_t *>(record) + field.offset;
	if (field.read_only) { error = "This field is derived from its authored properties."; return false; }
	if (field.type == DefFieldType::Text) {
		const auto *text = std::get_if<std::string>(&value);
		if (!text || text->size() >= field.width || text->find('\0') != std::string::npos ||
			text->find_first_of("\r\n\"") != std::string::npos || text->find("//") != std::string::npos) {
			error = "Text exceeds the field capacity or contains an unsupported character.";
			return false;
		}
		std::memset(p, 0, field.width);
		std::memcpy(p, text->data(), text->size());
		return true;
	}
	if (field.type == DefFieldType::Real) {
		const double n = std::holds_alternative<double>(value) ? std::get<double>(value) :
			std::holds_alternative<int64_t>(value) ? double(std::get<int64_t>(value)) :
			std::numeric_limits<double>::quiet_NaN();
		if (!std::isfinite(n) || std::abs(n) > std::numeric_limits<float>::max()) {
			error = "Enter a finite number in the field's range."; return false;
		}
		*reinterpret_cast<float *>(p) = static_cast<float>(n);
		return true;
	}
	const auto *number = std::get_if<int64_t>(&value);
	if (!number) { error = "This field takes an integer."; return false; }
	const int64_t n = *number;
	if (field.ranged && (n < field.min || n > field.max)) {
		error = "The game reads " + std::to_string(field.min) + " to " + std::to_string(field.max) + " here.";
		return false;
	}
	const int64_t minimum = field.type == DefFieldType::Integer ? INT32_MIN : 0;
	const int64_t capacity = field.id == "charfilter_count" ? 8 : field.id == "teamfilter_count" ? 4 :
		field.id == "function_args_count" ? 4 : INT32_MAX;
	const int64_t maximum = field.type == DefFieldType::Integer ? INT32_MAX :
		field.type == DefFieldType::Unsigned ? UINT32_MAX :
		field.type == DefFieldType::Byte ? 255 : capacity;
	if (n < minimum || n > maximum) { error = "The number exceeds the field's range."; return false; }
	switch (field.type) {
	case DefFieldType::Integer: *reinterpret_cast<int32_t *>(p) = static_cast<int32_t>(n); break;
	case DefFieldType::Unsigned: *reinterpret_cast<uint32_t *>(p) = static_cast<uint32_t>(n); break;
	case DefFieldType::Byte: *p = static_cast<uint8_t>(n); break;
	case DefFieldType::Count: *reinterpret_cast<size_t *>(p) = static_cast<size_t>(n); break;
	default: break;
	}
	return true;
}

void def_init_record(DefRecordKind kind, void *record) {
	std::memset(record, 0, def_record_size(kind));
	if (kind == DefRecordKind::Item) def_init_item(*static_cast<DefItemDef *>(record));
	if (kind == DefRecordKind::Weapon) def_init_weapon(*static_cast<DefWeaponDef *>(record));
	if (kind == DefRecordKind::Ammo) def_init_ammo(*static_cast<DefAmmoDef *>(record));
}

// Editor helpers over the native records. Parsing remains in the family parsers.
const std::vector<DefField> &def_native_fields(DefRecordKind kind);

namespace {

// --- what a member is beyond its storage: rule rows, applied in their order ---------------------
// Each rule names the records it applies to (a kind, or any) and the members by their id: the id
// itself (`is`), or what it starts with, holds or ends with (each given condition must hold); a
// later rule for a member overrides an earlier one. A new family's members take rows here.
constexpr int kAnyKind = -1;
struct Members {
	int kind = kAnyKind;
	const char *is = "";
	const char *starts = "";
	const char *holds = "";
	const char *ends = "";
	bool text_only = false;    // a text member only
	const char *except = "";   // a member the rule leaves out
};

bool matches(const Members &rule, DefRecordKind kind, const DefField &field) {
	const std::string &id = field.id;
	if (rule.kind != kAnyKind && rule.kind != int(kind)) return false;
	if (rule.text_only && field.type != DefFieldType::Text) return false;
	if (*rule.except && id == rule.except) return false;
	if (*rule.is && id != rule.is) return false;
	if (*rule.starts && id.rfind(rule.starts, 0) != 0) return false;
	if (*rule.holds && id.find(rule.holds) == std::string::npos) return false;
	const size_t tail = std::strlen(rule.ends);
	if (tail && (id.size() < tail || id.compare(id.size() - tail, tail, rule.ends) != 0)) return false;
	return true;
}

constexpr int K(DefRecordKind kind) { return int(kind); }
constexpr int kItem = K(DefRecordKind::Item), kWeapon = K(DefRecordKind::Weapon), kAmmo = K(DefRecordKind::Ammo),
              kAction = K(DefRecordKind::Action), kSight = K(DefRecordKind::Sight),
              kAttachment = K(DefRecordKind::Attachment), kPowerup = K(DefRecordKind::Powerup),
              kPowerupAction = K(DefRecordKind::PowerupAction);

// The counts an author sets (the filters' entries and a function's arguments), which a set of a
// lower one clears past (def_sync_derived); every other count follows its list, read only.
const char *const kAuthoredCounts[] = {"charfilter_count", "teamfilter_count", "function_args_count"};

// The members derived from others, which a set never changes: a mirror, an attachment slot, a mask,
// a fixed-point copy (def_sync_derived, the parsers' derives).
const Members kDerived[] = {
	{kAnyKind, "armor_blast"}, {kAnyKind, "emplacement_g_slot"}, {kAnyKind, "emplacement_c_slot"},
	{kAnyKind, "weapon_class_slot"}, {kAnyKind, "charfilter_mask"}, {kAnyKind, "teamfilter_mask"},
	{kAnyKind, "weaponweight_fp16"}, {kAnyKind, "clipweight_fp16"}, {kAnyKind, "", "error_fp16["},
};

// What a member names outside its record.
struct ReferenceRule {
	Members members;
	DefReference reference;
};
const ReferenceRule kReferences[] = {
	{{kAnyKind, "graphic"}, DefReference::Model}, {{kAnyKind, "graphic_enemy"}, DefReference::Model},
	{{kAnyKind, "husk"}, DefReference::Model}, {{kAnyKind, "huskfinal"}, DefReference::Model},
	{{kAnyKind, "gfx1"}, DefReference::Model}, {{kAnyKind, "gfx1a"}, DefReference::Model},
	{{kAnyKind, "gfx1b"}, DefReference::Model}, {{kAnyKind, "gfx3"}, DefReference::Model},
	{{kAnyKind, "anim_def"}, DefReference::AnimationMap}, {{kAnyKind, "animadm"}, DefReference::AnimationMap},
	{{kAnyKind, "default_aip"}, DefReference::AiProfile},
	{{kAnyKind, "primary_weapon"}, DefReference::Weapon},
	{{kAnyKind, "round_type", "", "", "", true}, DefReference::Ammo},
	{{kAnyKind, "notarmmed_ammo", "", "", "", true}, DefReference::Ammo},
	{{kAnyKind, "", "ammo_", "", "", true, "ammo_class"}, DefReference::Ammo},
	{{kAnyKind, "item_id"}, DefReference::Item},
	{{kAnyKind, "", "", "sound", "", true}, DefReference::Sound}, {{kAnyKind, "ai_launch", "", "", "", true}, DefReference::Sound},
	{{kAnyKind, "particle"}, DefReference::Particle}, {{kAnyKind, "hit_effect"}, DefReference::Particle},
	{{kAnyKind, "ai_launcheffect"}, DefReference::Particle}, {{kAnyKind, "heat_effect"}, DefReference::Particle},
	{{kAnyKind, "", "", ".effect"}, DefReference::Particle}, {{kAnyKind, "", "", ".secondary_effect"}, DefReference::Particle},
	{{kAnyKind, "particledeath"}, DefReference::Particle}, {{kAnyKind, "particleh2odeath"}, DefReference::Particle},
	{{kAnyKind, "particlefire"}, DefReference::Particle}, {{kAnyKind, "particleother"}, DefReference::Particle},
	{{kAnyKind, "particlefinale"}, DefReference::Particle}, {{kAnyKind, "particlespawn"}, DefReference::Particle},
	{{kAnyKind, "", "", "texture"}, DefReference::Texture}, {{kAnyKind, "loadout_menu_icon"}, DefReference::Texture},
	{{kAnyKind, "crosshair"}, DefReference::Texture}, {{kAnyKind, "crosshair_secondary"}, DefReference::Texture},
	{{kAnyKind, "hudicon"}, DefReference::Texture}, {{kAnyKind, "hud_image"}, DefReference::Texture},
	// An item's particle slots name a user point of its graphic model, found among the model's first
	// 16 points without case [orig: Game_ResolveItemMaterialsAndSpawnBoneTrails @ 0x522ee0 over
	// ItemDef_GetBoneMaskByName @ 0x49ea40].
	{{kItem, "", "particlefx", "", ".userpoint"}, DefReference::UserPoint},
	{{kAnyKind, "loadout_menu_textid"}, DefReference::GameText},
	{{kAnyKind, "loadout_menu_ttdesc"}, DefReference::OtherText}, {{kAnyKind, "attach_text_id"}, DefReference::OtherText},
	{{kAnyKind, "text_id"}, DefReference::OtherText}, {{kAnyKind, "text_token"}, DefReference::OtherText},
	// A powerup row's weapon resolves against the weapon table when the table is built, `all` being
	// every weapon [orig: PowerUpDef_ParseProperty @0x4431A7..0x443216]; its action blocks' keys are
	// the weapon actions' ActionDef keys [orig: ActionDef_ParseScriptLine @0x4023C0], their text token
	// a text key as an action's.
	{{kPowerup, "weapon"}, DefReference::Weapon},
	{{kPowerupAction, "texttoken"}, DefReference::OtherText},
};

// The values a member takes by name.
std::vector<DefChoice> item_types() {
	// A 0 no token names is written as no line: its choice names nothing.
	return {{"", 0, "Unset"}, {"vehicle", 1}, {"decoration", 2}, {"person", 3}, {"marker", 4}, {"building", 5},
	        {"powerup", 6}, {"effect", 8}};
}
std::vector<DefChoice> kill_zones() {
	std::vector<DefChoice> out = {{"", 0, "Unset"}};
	for (size_t n = 1; const char *name = def_ammo_kz_keyword(n); ++n) out.push_back({name, int64_t(n)});
	return out;
}
std::vector<DefChoice> tracer_types() {
	std::vector<DefChoice> out = {{"", 0, "None"}};
	for (size_t n = 0; const char *name = def_ammo_tracer_keyword(n); ++n) out.push_back({name, def_ammo_tracer_value(n)});
	return out;
}
// A husk piece's debris type, a row of the engine's table by its index [orig: g_DeathPieceTypes @
// 0x8404f0; DeathPieceType_FindByName @ 0x57b310].
std::vector<DefChoice> death_pieces() {
	std::vector<DefChoice> out;
	for (size_t n = 0; const char *name = defscan::death_piece_keyword(n); ++n) out.push_back({name, int64_t(n)});
	return out;
}
// The soldier and team tokens the game's two weapon.def readers give a mask bit; any other none. The
// weapon def's own reader, whose masks the host's loadout check reads [orig:
// NapiNPServerMsg_HandlePlayerLoadout @ 0x515790, the def's +0x7C / +0x80], walks charfilter's table @
// 0x830EB0 (5 rows) @ 0x543F40..0x543F5E and teamfilter's @ 0x830ED8 (red, blue) @ 0x543FB5..0x543FD3,
// warning "unrecognized character type" @ 0x543F82 / "unrecognized team" @ 0x543FFA [orig:
// WeaponDefs_ParseLineCallback @ 0x543680]; the loadout list's reader, whose masks the armory's weapon
// lists read [orig: PlayerInfo_PopulateWeaponSlotLists @ 0x560430], takes the same five classes and
// yellow as blue, violet as red too [orig: WeaponDef_ParseProperty @ 0x54d730, charfilter @
// 0x54d916..0x54d9be, teamfilter @ 0x54daae..0x54db08].
std::vector<DefChoice> character_filters() {
	return {{"medic", 1}, {"sniper", 2}, {"gunner", 4}, {"rifleman", 8}, {"engineer", 16}};
}
std::vector<DefChoice> team_filters() { return {{"red", 1}, {"blue", 2}, {"yellow", 2}, {"violet", 1}}; }
// `auto` or a tick count [orig: ActionDef_ParseScriptLine, the "auto" compare @ 0x40272B / 0x402B41].
std::vector<DefChoice> delays() { return {{"auto", -1}}; }
std::vector<DefChoice> sight_blends() {
	return {{"blend", 0}, {"add", 1}, {"blendat", 2}, {"multiply", 3}, {"addat", 4}, {"multiplyat", 5}};
}
std::vector<DefChoice> attachment_kinds() {
	return {{"addeweap", DEF_ITEM_EMPLACEMENT_ADDEWEAP}, {"addeweapg", DEF_ITEM_EMPLACEMENT_ADDEWEAP_G},
	        {"addeweapc", DEF_ITEM_EMPLACEMENT_ADDEWEAP_C}};
}
std::vector<DefChoice> weapon_classes() { return {{"accessory", 0}, {"primary", 1}, {"secondary", 2}, {"grenade", 3}}; }
// The values the game's code tells apart, named by what it does with each; a number no code singles
// out is typed as one (an open list) or reads as a listed one. A weapon's category is its row of the
// 12-row slot table, a handle being category * 65 + rank (the game's parser keeps 0..11, reading any
// other as 0) [orig: WeaponDefs_ParseLineCallback @ 0x543997..0x5439c6; Player_SwitchToWeaponByHandle @
// 0x4e0170]. Rows 1..9 are the ones the weapon keys select, named as their key bindings are (actions
// 201..209, keys 1..9) [orig: the binding rows @ 0x816578..0x8168d8; Input_HandleActionBinding_0 @
// 0x4e1144, actions 200..210 -> row action - 200]; row 11 holds a vehicle's mounted weapons, never
// carried off it [orig: Entity_DetachFromVehicle @ 0x43564b; Entity_DetachFromParent @ 0x494b9f;
// Entity_DetachFromMount @ 0x546d75; NetPacket_SerializePlayerState @ 0x4c209a]. Rows 0 and 10 have no
// key.
std::vector<DefChoice> weapon_categories() {
	return {{"1", 1, "Knife"},        {"2", 2, "Sidearm"},       {"3", 3, "Primary"},
	        {"4", 4, "Flashbang"},    {"5", 5, "Frag grenade"},  {"6", 6, "Smoke grenade"},
	        {"7", 7, "Accessory"},    {"8", 8, "Detonator"},     {"9", 9, "Medpack"},
	        {"11", 11, "Mounted weapon"}};
}
// The third-person hold pose: 1..8 select the body states 50..61, named as the anim table names them
// (5..8 one state on while scoped; 2 also reloads with reload2), any other value mirrors the body's
// own state as a rifle does [orig: Entity_UpdateInfantryPlayerBody @ 0x4b5dba..0x4b5e35;
// g_AnimStateNameTable @ 0x8135F0].
std::vector<DefChoice> hold_poses() {
	return {{"0", 0, "Rifle (the body's own state)"}, {"1", 1, "knife"}, {"2", 2, "pistol"}, {"3", 3, "grenade"},
	        {"4", 4, "stinger"}, {"5", 5, "designator"}, {"6", 6, "P90"}, {"7", 7, "MP7"}, {"8", 8, "javelin"}};
}
// What firing stamps on the body: 1 state 62 knife_attack, 2 state 63 grenade_attack, any other
// nothing [orig: WeaponAction_Fire @ 0x542bbc / 0x542bdb].
std::vector<DefChoice> attack_anims() { return {{"0", 0, "Nothing"}, {"1", 1, "knife_attack"}, {"2", 2, "grenade_attack"}}; }
// Whether a vehicle moves by its physics block: every reader tests it against 0 [orig:
// Entity_DispatchPhysics_cveh @ 0x48efc7; Entity_UpdateParentTransform @ 0x4a88bb].
std::vector<DefChoice> physics() { return {{"0", 0, "Its class's motor"}, {"1", 1, "The physics block"}}; }
// A vehicle's family and what each type does, by its byte [orig: ItemDef_ParseProperty @ 0x49ee47]: 1, 2
// and 12 land, 3 and 4 air, 5..8 water [orig: ItemDefs_LoadAndValidate @ 0x4a1fa7; Entity_GetVehicleClass
// @ 0x4f9e27]; the map's icon, the voice set and the death each type takes [orig:
// Entity_ClassifyForMinimap @ 0x50fa70; VMacros_BuildShaderPassName @ 0x5bf613;
// Entity_DispatchDeathCallback's table @ 0x815410]; 3 a helicopter, whose spinning rotor strikes [orig:
// Entity_MovementCollisionResolver @ 0x4b30c3]; 7 and 8 burning with the medium and the large fire
// [orig: Entity_ProcessAirVehiclePhysics @ 0x46fba1]; 9 its own score tally [orig:
// Score_ClassifyEntityForCounts @ 0x4fd0b1]; 11 a bridge. Any other byte takes every default.
std::vector<DefChoice> unit_types() {
	return {{"1", 1, "Land vehicle"},      {"2", 2, "Land vehicle, TN voice set"},
	        {"3", 3, "Helicopter"},        {"4", 4, "Air vehicle"},
	        {"5", 5, "Boat"},              {"6", 6, "Boat"},
	        {"7", 7, "Boat, medium fire"}, {"8", 8, "Boat, large fire"},
	        {"9", 9, "Own score tally"},   {"10", 10, "Falling wreck"},
	        {"11", 11, "Bridge"},          {"12", 12, "Land vehicle, DB voice set"}};
}
// A round's mark: 0 none; any other the glass decal of that scar table row where the hit model has
// glass, else the ring scar, which 2 never makes [orig: AmmoDef_ProcessImpactEffect @ 0x40a24e;
// Impact_SpawnGlassEffectsOrScar @ 0x5cf1b6, 0x5cf289; Scar_TextureForId @ 0x5cc360].
std::vector<DefChoice> scars() { return {{"0", 0, "No mark"}, {"2", 2, "Glass only"}}; }
std::vector<DefChoice> item_attributes() {
	std::vector<DefChoice> out;
	for (int n = 0; n < def_item_attrib_keyword_count(); ++n)
		out.push_back({def_item_attrib_keyword(n), def_item_attrib_keyword_bit(n)});
	return out;
}
std::vector<DefChoice> item_attributes2() {
	std::vector<DefChoice> out;
	for (int n = 0; n < def_item_attrib2_keyword_count(); ++n)
		out.push_back({def_item_attrib2_keyword(n), def_item_attrib2_keyword_bit(n)});
	return out;
}
std::vector<DefChoice> weapon_flags(bool second) {
	std::vector<DefChoice> out;
	for (size_t n = 0; const auto *flag = defscan::weapon_flag_at(n); ++n) {
		const uint32_t bit = uint32_t(second ? flag->bit2 : flag->bit);
		if (bit) out.push_back({flag->name, bit});
	}
	return out;
}
std::vector<DefChoice> weapon_flags1() { return weapon_flags(false); }
std::vector<DefChoice> weapon_flags2() { return weapon_flags(true); }
std::vector<DefChoice> ammo_flags() {
	std::vector<DefChoice> out;
	for (size_t n = 0; const char *name = def_ammo_flag_keyword(n); ++n) out.push_back({name, def_ammo_flag_bit(n)});
	return out;
}

struct ChoiceRule {
	Members members;
	std::vector<DefChoice> (*choices)(); // null: the member's bits name ids a file names (flags alone)
	bool flags = false; // the choices are bits of one integer
	bool open = false;  // the choices are the values known; the parser reads any other too
};
const ChoiceRule kChoices[] = {
	{{kItem, "type"}, item_types},
	{{kAmmo, "kztype"}, kill_zones},
	{{kAmmo, "tracer_type_friendly"}, tracer_types}, {{kAmmo, "tracer_type_enemy"}, tracer_types},
	{{kItem, "", "husk_sub_part_types["}, death_pieces},
	{{kWeapon, "", "charfilter["}, character_filters, false, true},
	{{kWeapon, "", "teamfilter["}, team_filters, false, true},
	{{kAction, "delaystart"}, delays, false, true}, {{kAction, "delayend"}, delays, false, true},
	{{kPowerupAction, "delaystart"}, delays, false, true}, {{kPowerupAction, "delayend"}, delays, false, true},
	// An item's vehicle spawn slots: one bit per id of the file-wide registry, whose ids an editor
	// names per file [orig: @0x4A0253; @0x49DFC0].
	{{kItem, "vehicle_spawn_mask"}, nullptr, true},
	{{kSight, "blend"}, sight_blends},
	{{kAttachment, "kind"}, attachment_kinds},
	{{kWeapon, "weapon_class"}, weapon_classes},
	{{kWeapon, "category"}, weapon_categories, false, true},
	{{kWeapon, "special_hold"}, hold_poses},
	{{kWeapon, "attack_anim"}, attack_anims},
	{{kItem, "physics"}, physics},
	{{kItem, "unit_type"}, unit_types, false, true},
	{{kAmmo, "scar_type"}, scars, false, true},
	{{kAnyKind, "attrib"}, item_attributes, true}, {{kAnyKind, "attrib2"}, item_attributes2, true},
	{{kWeapon, "flags"}, weapon_flags1, true}, {{kWeapon, "flags2"}, weapon_flags2, true},
	{{kAmmo, "flags"}, ammo_flags, true},
};

// The ranges the readers keep these to [orig: WeaponDefs_ParseLineCallback @ 0x543997..0x5439c6,
// 0x5439eb..0x543a1b; ItemDef_ParseProperty @ 0x49ee47]; a powerup row's two switches, which its
// parser sets to 1 or (the weapon's) 0 alone [orig: PowerUpDef_ParseProperty @0x4431DA, @0x443220].
struct RangeRule {
	Members members;
	int64_t min, max;
};
const RangeRule kRanges[] = {
	{{kWeapon, "category"}, 0, DEF_WEAPON_CATEGORIES - 1},
	{{kWeapon, "rank"}, 0, DEF_WEAPON_RANKS - 1},
	{{kItem, "unit_type"}, 0, 255},
	{{kPowerup, "weapon_all"}, 0, 1},
	{{kPowerup, "allammo"}, 0, 1},
};

} // namespace

const std::vector<DefField> &def_fields(DefRecordKind kind) {
	static const std::array<std::vector<DefField>, kDefRecordKindCount> fields = [] {
		std::array<std::vector<DefField>, kDefRecordKindCount> all;
		for (size_t i = 0; i < all.size(); ++i) {
			const auto k = static_cast<DefRecordKind>(i);
			all[i] = def_native_fields(k);
			for (DefField &f : all[i]) {
				f.read_only = f.type == DefFieldType::Count &&
				              std::none_of(std::begin(kAuthoredCounts), std::end(kAuthoredCounts),
				                           [&](const char *id) { return f.id == id; });
				for (const Members &rule : kDerived)
					if (matches(rule, k, f)) f.read_only = true;
				for (const ReferenceRule &rule : kReferences)
					if (matches(rule.members, k, f)) f.reference = rule.reference;
				for (const ChoiceRule &rule : kChoices) {
					if (!matches(rule.members, k, f)) continue;
					if (rule.choices) f.choices = rule.choices();
					f.flags = f.flags || rule.flags;
					f.open = f.open || rule.open;
				}
				for (const RangeRule &rule : kRanges) {
					if (!matches(rule.members, k, f)) continue;
					f.ranged = true;
					f.min = rule.min;
					f.max = rule.max;
				}
			}
		}
		return all;
	}();
	return fields.at(static_cast<size_t>(kind));
}

void def_sync_derived(DefRecordKind kind, void *value, const std::string &field) {
	if (kind == DefRecordKind::Item) {
		auto &item = *static_cast<DefItemDef *>(value);
		if (field == "armor_kz") item.armor_blast = item.armor_kz;
		if (field == "powerup_def" && item.powerup_def[0]) item.attrib |= DEF_ITEM_ATTRIB_POWERUP;
		if (field == "default_aip" && item.default_aip[0]) item.attrib |= DEF_ITEM_ATTRIB_AIDATA;
		return;
	}
	if (kind == DefRecordKind::Action) {
        auto &action = *static_cast<DefWeaponAction *>(value);
        if (field == "function_args_count")
            std::fill(action.function_args + action.function_args_count, action.function_args + 4, 0);
        return;
    }
    if (kind != DefRecordKind::Weapon) return;
	auto &weapon = *static_cast<DefWeaponDef *>(value);
    if (field == "charfilter_count")
        for (size_t i = weapon.charfilter_count; i < 8; ++i) std::memset(weapon.charfilter[i], 0, sizeof(weapon.charfilter[i]));
    if (field == "teamfilter_count")
        for (size_t i = weapon.teamfilter_count; i < 4; ++i) std::memset(weapon.teamfilter[i], 0, sizeof(weapon.teamfilter[i]));
	// Derive mirrors through the existing parser; this keeps its token mappings
	// and fixed-point conversions in one place.
	DefRecordWriter writer;
	writer.result.text = "weapon \"derived\"\n";
	writer.record(kind, value, "");
	writer.result.text += "end\n";
	DefWeaponsFile parsed{};
	def_parse_weapons_memory(reinterpret_cast<const uint8_t *>(writer.result.text.data()), writer.result.text.size(), &parsed);
	if (parsed.count == 1) {
		const auto &result = parsed.entries[0];
		weapon.weapon_class_slot = result.weapon_class_slot;
		weapon.charfilter_mask = result.charfilter_mask;
		weapon.teamfilter_mask = result.teamfilter_mask;
		weapon.weaponweight_fp16 = result.weaponweight_fp16;
		weapon.clipweight_fp16 = result.clipweight_fp16;
		std::copy(std::begin(result.error_fp16), std::end(result.error_fp16), std::begin(weapon.error_fp16));
	}
	def_free_weapons(&parsed);
}

} // namespace opennova::def
