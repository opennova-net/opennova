#include "def_schema.h"
#include "def.h"
#include "def_scan.h"
#include "def_write_record.h"
#include <array>
#include <algorithm>

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

const std::vector<DefField> &def_fields(DefRecordKind kind) {
	static const std::array<std::vector<DefField>, 8> fields = [] {
		std::array<std::vector<DefField>, 8> all;
		for (size_t i = 0; i < all.size(); ++i) {
			const auto k = static_cast<DefRecordKind>(i);
			all[i] = def_native_fields(k);
			for (auto &f : all[i]) {
				const auto &id = f.id;
				f.read_only = f.type == DefFieldType::Count &&
					id != "charfilter_count" && id != "teamfilter_count" && id != "function_args_count";
				if (id == "armor_blast" || id == "emplacement_g_slot" || id == "emplacement_c_slot" ||
					id == "weapon_class_slot" || id == "charfilter_mask" || id == "teamfilter_mask" ||
					id == "weaponweight_fp16" || id == "clipweight_fp16" || id.find("error_fp16[") == 0)
					f.read_only = true;
				if (id == "graphic" || id == "graphic_enemy" || id == "husk" || id == "huskfinal" ||
					id == "gfx1" || id == "gfx1a" || id == "gfx1b" || id == "gfx3") f.reference = DefReference::Model;
				if (id == "anim_def" || id == "animadm") f.reference = DefReference::AnimationMap;
				if (id == "default_aip") f.reference = DefReference::AiProfile;
				if (id == "primary_weapon") f.reference = DefReference::Weapon;
				if (id == "round_type" || id == "notarmmed_ammo" || id.find("ammo_") == 0)
					if (f.type == DefFieldType::Text && id != "ammo_class") f.reference = DefReference::Ammo;
				if (id == "item_id") f.reference = DefReference::Item;
				if (f.type == DefFieldType::Text && (id.find("sound") != std::string::npos || id == "ai_launch")) f.reference = DefReference::Sound;
				if (id == "particle" || id == "hit_effect" || id == "ai_launcheffect" || id == "heat_effect" ||
					id.find(".effect") != std::string::npos || id.find(".secondary_effect") != std::string::npos ||
					id == "particledeath" || id == "particleh2odeath" || id == "particlefire" ||
					id == "particleother" || id == "particlefinale" || id == "particlespawn") f.reference = DefReference::Particle;
				if (id.find("texture") != std::string::npos || id == "loadout_menu_icon" ||
					id == "crosshair" || id == "crosshair_secondary" || id == "hudicon" || id == "hud_image")
					f.reference = DefReference::Texture;
				if (id == "loadout_menu_textid") f.reference = DefReference::GameText;
				if (id == "loadout_menu_ttdesc" || id == "attach_text_id" || id == "text_id" || id == "text_token")
					f.reference = DefReference::OtherText;
				if (k == DefRecordKind::Item && id == "type")
					f.choices = {{"Unset",0},{"vehicle",1},{"decoration",2},{"person",3},{"marker",4},{"building",5},{"powerup",6},{"effect",8}};
				if (k == DefRecordKind::Ammo && id == "kztype") {
					f.choices.push_back({"Unset",0});
					for (size_t n = 1; const char *name = def_ammo_kz_keyword(n); ++n) f.choices.push_back({name,int64_t(n)});
				}
				if (k == DefRecordKind::Ammo && (id == "tracer_type_friendly" || id == "tracer_type_enemy")) {
                    f.choices.push_back({"None", 0});
                    for (size_t n = 0; const char *name = def_ammo_tracer_keyword(n); ++n)
                        f.choices.push_back({name, def_ammo_tracer_value(n)});
                }
                if (k == DefRecordKind::Sight && id == "blend")
					f.choices = {{"blend",0},{"add",1},{"blendat",2},{"multiply",3},{"addat",4},{"multiplyat",5}};
				if (k == DefRecordKind::Attachment && id == "kind")
					f.choices = {{"addeweap",DEF_ITEM_EMPLACEMENT_ADDEWEAP},{"addeweapg",DEF_ITEM_EMPLACEMENT_ADDEWEAP_G},{"addeweapc",DEF_ITEM_EMPLACEMENT_ADDEWEAP_C}};
				if (k == DefRecordKind::Weapon && id == "weapon_class")
					f.choices = {{"accessory",0},{"primary",1},{"secondary",2},{"grenade",3}};
				if (id == "attrib" || id == "attrib2") {
					const bool second = id == "attrib2";
					for (int n = 0; n < (second ? def_item_attrib2_keyword_count() : def_item_attrib_keyword_count()); ++n)
						f.choices.push_back({second ? def_item_attrib2_keyword(n) : def_item_attrib_keyword(n),
							second ? def_item_attrib2_keyword_bit(n) : def_item_attrib_keyword_bit(n)});
					f.flags = true;
				}
				if (k == DefRecordKind::Weapon && (id == "flags" || id == "flags2")) {
					for (size_t n = 0; const auto *flag = defscan::weapon_flag_at(n); ++n) {
						const uint32_t bit = uint32_t(id == "flags" ? flag->bit : flag->bit2);
						if (bit) f.choices.push_back({flag->name,bit});
					}
					f.flags = true;
				}
				if (k == DefRecordKind::Ammo && id == "flags") {
					for (size_t n = 0; const char *name = def_ammo_flag_keyword(n); ++n)
						f.choices.push_back({name,def_ammo_flag_bit(n)});
					f.flags = true;
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
