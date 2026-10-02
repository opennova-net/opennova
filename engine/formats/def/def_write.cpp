#include "def_write_record.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

namespace opennova::def {
namespace {

// A line whose present flag is clear is not written, so its members read back as the parser's
// defaults: their latent values are no part of the file and are not compared.
void check_record(DefRecordWriter &writer, DefRecordKind kind, const void *before,
                  const void *after, const std::string &name) {
	std::vector<const std::string *> unwritten;
	for (const DefProperty &property : def_properties(kind)) {
		const DefField *flag = property.present_field.empty() ? nullptr : def_field(kind, property.present_field);
		if (flag && def_get(before, *flag) == DefValue(int64_t(0)))
			for (const std::string &member : property.fields) unwritten.push_back(&member);
	}
	for (const DefField &field : def_fields(kind)) {
		if (std::any_of(unwritten.begin(), unwritten.end(), [&](const std::string *id) { return *id == field.id; })) continue;
		if (def_get(before, field) != def_get(after, field))
			writer.fail(name, field.id, "This value cannot be saved without changing its native meaning.");
	}
}

// `capacity`: the characters the family's reader copies of the name, its terminator included
// (an item's 46 [orig: ItemDef_ParseProperty @0x49EBFB], a weapon's 32 [orig:
// WeaponDefs_ParseLineCallback @0x543737], an ammo's 31 [orig: AmmoDef_AllocateSlot
// @0x409B01..0x409B24], a powerup's 16 [orig: PowerUpDef_ParseProperty @0x442F3F]).
bool header(DefRecordWriter &writer, const char *key, const char *name, size_t capacity, bool quoted) {
	const auto *end = static_cast<const char *>(std::memchr(name, 0, capacity));
	if (!end) { writer.fail("", key, "Name exceeds its capacity."); return false; }
	const std::string text(name, end);
	// A quote never survives (it would close the name); a delimiter or a comment start
	// survives only inside quotes, where the line tokenizer keeps it [orig:
	// Terrain_TokenizeConfigLine @ 0x53CB60, delimiters @0x53CC33..0x53CC4C]: the reader
	// takes the name from token 1, quoted or not.
	if (text.find_first_of("\r\n\"") != std::string::npos) {
		writer.fail(text, key, "Name contains an unsupported character."); return false;
	}
	quoted |= text.find_first_of(" ,\t;") != std::string::npos || text.find("//") != std::string::npos;
	writer.result.text += std::string(key) + " " + (quoted ? "\"" + text + "\"" : text) + "\r\n";
	return true;
}

void incomplete(DefRecordWriter &writer, size_t count, const std::string &name) {
	if (count) writer.fail(name, "", "The parsed document contains unsupported or malformed input; fix the reported source location before saving.");
}

DefWriteResult finish(DefRecordWriter &writer) {
	if (!writer.result.ok()) writer.result.text.clear();
	return std::move(writer.result);
}

} // namespace

DefWriteResult def_write_items(const DefItemsFile &file) {
	DefRecordWriter writer;
	writer.result.text = "// Item definitions\r\n\r\n";
	writer.items = &file;
	incomplete(writer, file.unmodeled_count, "");
	if (file.vehicle_spawn_id_count < 0 || file.vehicle_spawn_id_count > DEF_VEHICLE_SPAWN_SLOTS ||
		(file.vehicle_spawn_id_count && !file.count))
		writer.fail("", "pcvehicle_spawnlist", "The vehicle spawn registry cannot be represented.");
	for (size_t i = 0; i < file.count; ++i) {
		const auto &item = file.entries[i];
		if (!header(writer, "begin", item.display_name, 47, true)) continue;
        incomplete(writer, item.unmodeled_count, item.display_name);
        if (item.powerup_def[0] && (item.deathtime_ticks || item.clipsize || item.door_type ||
            item.door_open_rate_q16 || item.door_max_angle_bam || item.door_open_sound[0] || item.door_close_sound[0]))
            writer.fail(item.display_name, "powerup_def", "Powerup names and door/death numeric values share a native union; choose one branch.");
		// Register file-wide slots in their original order, then set this item's mask.
		// The parser's registry is cumulative; a repeated list replaces only the mask.
		if (i == 0 && file.vehicle_spawn_id_count > 0 && file.vehicle_spawn_id_count <= DEF_VEHICLE_SPAWN_SLOTS) {
			std::vector<std::string> ids;
			for (int j = 0; j < file.vehicle_spawn_id_count; ++j) ids.push_back(std::to_string(file.vehicle_spawn_ids[j]));
			writer.line("pcvehicle_spawnlist", ids);
			if (!item.vehicle_spawn_mask) writer.line("pcvehicle_spawnlist", {});
		}
		writer.record(DefRecordKind::Item, &item, item.display_name);
		for (size_t j = 0; j < item.emplacement_attachments_count; ++j)
			writer.record(DefRecordKind::Attachment, &item.emplacement_attachments[j], item.display_name);
		writer.result.text += "end\r\n\r\n";
	}
	if (!writer.result.ok()) return finish(writer);
	DefItemsFile parsed{};
	DefParseReport issues;
	def_parse_items_memory(reinterpret_cast<const uint8_t *>(writer.result.text.data()), writer.result.text.size(), &parsed, &issues);
	for (const auto &issue : issues) writer.fail(issue.record, issue.field, "Generated property failed native input validation.");
	if (file.count != parsed.count) writer.fail("", "", "Record count changed during serialization.");
	for (size_t i = 0; i < std::min(file.count, parsed.count); ++i) {
		const auto &a = file.entries[i]; const auto &b = parsed.entries[i];
		check_record(writer, DefRecordKind::Item, &a, &b, a.display_name);
		for (size_t j = 0; j < std::min(a.emplacement_attachments_count, b.emplacement_attachments_count); ++j)
			check_record(writer, DefRecordKind::Attachment, &a.emplacement_attachments[j], &b.emplacement_attachments[j], a.display_name);
	}
	if (file.vehicle_spawn_id_count != parsed.vehicle_spawn_id_count ||
		std::memcmp(file.vehicle_spawn_ids, parsed.vehicle_spawn_ids, sizeof(file.vehicle_spawn_ids)))
		writer.fail("", "pcvehicle_spawnlist", "Vehicle spawn registry changed during serialization.");
	def_free_items(&parsed);
	return finish(writer);
}

DefWriteResult def_write_weapons(const DefWeaponsFile &file) {
	DefRecordWriter writer;
	writer.result.text = "// Weapon definitions\r\n\r\n";
	incomplete(writer, file.unmodeled_count, "");
	for (size_t i = 0; i < file.ammo_classes_count; ++i)
		writer.record(DefRecordKind::Carry, &file.ammo_classes[i], file.ammo_classes[i].name);
	for (size_t i = 0; i < file.count; ++i) {
		const auto &weapon = file.entries[i];
		if (!header(writer, "weapon", weapon.weapon_name, 33, true)) continue;
		incomplete(writer, weapon.unmodeled_count, weapon.weapon_name);
		writer.record(DefRecordKind::Weapon, &weapon, weapon.weapon_name);
		for (size_t j = 0; j < weapon.sights_count; ++j)
			writer.record(DefRecordKind::Sight, &weapon.sights[j], weapon.weapon_name);
		for (size_t j = 0; j < weapon.actions_count; ++j) {
			const auto &action = weapon.actions[j];
			if (!header(writer, "action", action.name, sizeof(action.name), true)) continue;
			incomplete(writer, action.unmodeled_count, action.name);
			writer.record(DefRecordKind::Action, &action, action.name);
			writer.result.text += "\tend\r\n";
		}
		writer.result.text += "end\r\n\r\n";
	}
	if (!writer.result.ok()) return finish(writer);
	DefWeaponsFile parsed{};
	DefParseReport issues;
	def_parse_weapons_memory(reinterpret_cast<const uint8_t *>(writer.result.text.data()), writer.result.text.size(), &parsed, &issues);
	for (const auto &issue : issues) writer.fail(issue.record, issue.field, "Generated property failed native input validation.");
	if (file.count != parsed.count || file.ammo_classes_count != parsed.ammo_classes_count)
		writer.fail("", "", "Record count changed during serialization.");
	for (size_t i = 0; i < std::min(file.ammo_classes_count, parsed.ammo_classes_count); ++i)
		check_record(writer, DefRecordKind::Carry, &file.ammo_classes[i], &parsed.ammo_classes[i], file.ammo_classes[i].name);
	for (size_t i = 0; i < std::min(file.count, parsed.count); ++i) {
		const auto &a = file.entries[i]; const auto &b = parsed.entries[i];
		check_record(writer, DefRecordKind::Weapon, &a, &b, a.weapon_name);
		for (size_t j = 0; j < std::min(a.actions_count, b.actions_count); ++j)
			check_record(writer, DefRecordKind::Action, &a.actions[j], &b.actions[j], a.weapon_name);
		for (size_t j = 0; j < std::min(a.sights_count, b.sights_count); ++j)
			check_record(writer, DefRecordKind::Sight, &a.sights[j], &b.sights[j], a.weapon_name);
	}
	def_free_weapons(&parsed);
	return finish(writer);
}

DefWriteResult def_write_ammo(const DefAmmoFile &file) {
	DefRecordWriter writer;
	writer.result.text = "// Ammo definitions\r\n\r\n";
	incomplete(writer, file.unmodeled_count, "");
	for (size_t i = 0; i < file.count; ++i) {
		const auto &ammo = file.entries[i];
		if (!header(writer, "ammo", ammo.name, 32, false)) continue;
		incomplete(writer, ammo.unmodeled_count, ammo.name);
		writer.record(DefRecordKind::Ammo, &ammo, ammo.name);
		if (ammo.effects_table_count) {
			writer.line("effects_table", {});
			for (size_t j = 0; j < ammo.effects_table_count; ++j)
				writer.record(DefRecordKind::Effect, &ammo.effects_table[j], ammo.name);
			writer.line("end", {});
		}
		writer.result.text += "end\r\n\r\n";
	}
	if (!writer.result.ok()) return finish(writer);
	DefAmmoFile parsed{};
	DefParseReport issues;
	def_parse_ammo_memory(reinterpret_cast<const uint8_t *>(writer.result.text.data()), writer.result.text.size(), &parsed, &issues);
	for (const auto &issue : issues) writer.fail(issue.record, issue.field, "Generated property failed native input validation.");
	if (file.count != parsed.count) writer.fail("", "", "Record count changed during serialization.");
	for (size_t i = 0; i < std::min(file.count, parsed.count); ++i) {
		const auto &a = file.entries[i]; const auto &b = parsed.entries[i];
		check_record(writer, DefRecordKind::Ammo, &a, &b, a.name);
		for (size_t j = 0; j < std::min(a.effects_table_count, b.effects_table_count); ++j)
			check_record(writer, DefRecordKind::Effect, &a.effects_table[j], &b.effects_table[j], a.name);
	}
	def_free_ammo(&parsed);
	return finish(writer);
}

DefWriteResult def_write_powerup(const DefPowerupFile &file) {
	DefRecordWriter writer;
	writer.result.text = "// Powerup definitions\r\n\r\n";
	incomplete(writer, file.unmodeled_count, "");
	// The action blocks a row writes, by the name its `action` line gives each [orig:
	// PowerUpDef_ParseProperty @0x443056..0x4430F4]: a block the row holds is written, one it does not
	// is no part of the file.
	const auto actions = [](const DefPowerupDef &row) {
		return std::array<std::pair<const char *, const DefPowerupAction *>, 2>{
		        {{"pickup", &row.pickup}, {"respawn", &row.respawn}}};
	};
	for (size_t i = 0; i < file.count; ++i) {
		const DefPowerupDef &row = file.entries[i];
		if (!header(writer, "powerup", row.name, sizeof(row.name), true)) continue;
		writer.record(DefRecordKind::Powerup, &row, row.name);
		for (size_t j = 0; j < row.ammo_count; ++j) writer.record(DefRecordKind::PowerupAmmo, &row.ammo[j], row.name);
		for (const auto &[name, action] : actions(row)) {
			if (!action->present) continue;
			writer.result.text += std::string("\taction \"") + name + "\"\r\n";
			writer.record(DefRecordKind::PowerupAction, action, row.name);
			writer.result.text += "\tend\r\n";
		}
		writer.result.text += "end\r\n\r\n";
	}
	if (!writer.result.ok()) return finish(writer);
	DefPowerupFile parsed{};
	DefParseReport issues;
	def_parse_powerup_memory(reinterpret_cast<const uint8_t *>(writer.result.text.data()), writer.result.text.size(),
	                         &parsed, &issues);
	for (const auto &issue : issues) writer.fail(issue.record, issue.field, "Generated property failed native input validation.");
	if (file.count != parsed.count) writer.fail("", "", "Record count changed during serialization.");
	for (size_t i = 0; i < std::min(file.count, parsed.count); ++i) {
		const DefPowerupDef &a = file.entries[i], &b = parsed.entries[i];
		check_record(writer, DefRecordKind::Powerup, &a, &b, a.name);
		if (a.ammo_count != b.ammo_count) writer.fail(a.name, "ammo", "Ammo rows changed during serialization.");
		for (size_t j = 0; j < std::min(a.ammo_count, b.ammo_count); ++j)
			check_record(writer, DefRecordKind::PowerupAmmo, &a.ammo[j], &b.ammo[j], a.name);
		const auto before = actions(a), after = actions(b);
		for (size_t k = 0; k < before.size(); ++k) {
			if (before[k].second->present != after[k].second->present)
				writer.fail(a.name, before[k].first, "An action block changed during serialization.");
			else if (before[k].second->present)
				check_record(writer, DefRecordKind::PowerupAction, before[k].second, after[k].second, a.name);
		}
	}
	def_free_powerup(&parsed);
	return finish(writer);
}

} // namespace opennova::def
