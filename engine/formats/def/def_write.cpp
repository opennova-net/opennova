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

bool header(DefRecordWriter &writer, const char *key, const char *name, size_t capacity, bool quoted,
            const std::string &margin = std::string()) {
	const auto *end = static_cast<const char *>(std::memchr(name, 0, capacity));
	if (!end) { writer.fail("", key, "Name exceeds its capacity."); return false; }
	const std::string text(name, end);
	// A quote never survives (it would close the name); a comment start survives only
	// inside quotes, where the line tokenizer keeps it [orig: Terrain_TokenizeConfigLine
	// @ 0x53CB60].
	const bool comment_start = text.find("//") != std::string::npos || text.find(';') != std::string::npos;
	if (text.find_first_of("\r\n\"") != std::string::npos || (!quoted && comment_start)) {
		writer.fail(text, key, "Name contains an unsupported character."); return false;
	}
	writer.result.text += margin + key + " " + (quoted ? "\"" + text + "\"" : text) + "\r\n";
	return true;
}

void incomplete(DefRecordWriter &writer, size_t count, const std::string &name) {
	if (count) writer.fail(name, "", "The parsed document contains unsupported or malformed input; fix the reported source location before saving.");
}

DefWriteResult finish(DefRecordWriter &writer) {
	if (!writer.result.ok()) writer.result.text.clear();
	return std::move(writer.result);
}

// The order a write puts each record's lines down in: the order it was read in (`keep`), but for the
// records `table` marks, written in the table's; the records whose kept order the reparse check refused
// (`refused`, out); and whether a record written in the table's order though it has an order of its own
// is said (`report`, into DefWriteResult::reordered).
struct WriteOrder {
	bool keep = true;
	std::vector<bool> table;
	std::vector<size_t> refused;
	bool report = false;
	bool tabled(size_t record) const { return !keep || (record < table.size() && table[record]); }
};

// A writer over a file's layout (DefLayout): its indentation, and its records' lines in the order they
// were read in where its order keeps them (record by record: WriteOrder::tabled).
DefRecordWriter writer_for(const DefLayout &layout, const WriteOrder &order) {
	DefRecordWriter writer;
	writer.keep_order = order.keep;
	if (layout.indent[0]) writer.indent = layout.indent;
	return writer;
}

// Record `i`'s lines in the order it keeps, saying so where a record with an order of its own is written
// in the table's.
void order_record(DefRecordWriter &writer, const WriteOrder &order, size_t i, const DefLineOrder &own, const char *name) {
	writer.keep_order = !order.tabled(i);
	if (order.report && !writer.keep_order && own.count) writer.result.reordered.push_back(name);
}

// The reparse check of record `i`, which marks it refused where it added a failure.
template <class Check> void check_one(DefRecordWriter &writer, WriteOrder &order, size_t i, Check check) {
	const size_t before = writer.result.diagnostics.size();
	check();
	if (writer.result.diagnostics.size() != before) order.refused.push_back(i);
}

// A file written keeping each record's order; where the reparse check refuses a record's (an order the
// game reads otherwise than the record holds: a deceleration before the acceleration that defaults it, a
// kept line an edit leaves it unable to carry), that record in the table's order, the rest as read; else
// the whole file in the table's. Each record written in the table's order is named (reordered).
template <class File, class Write>
DefWriteResult write_in_order(const File &file, Write write) {
	WriteOrder kept_order;
	DefWriteResult kept = write(file, kept_order);
	if (kept.ok()) return kept;
	if (!kept_order.refused.empty()) {
		WriteOrder some;
		some.table.assign(file.count, false);
		for (size_t i : kept_order.refused) some.table[i] = true;
		some.report = true;
		DefWriteResult retry = write(file, some);
		if (retry.ok()) return retry;
	}
	WriteOrder table;
	table.keep = false;
	table.report = true;
	DefWriteResult all = write(file, table);
	return all.ok() ? all : kept;
}

// Whether the items' own `pcvehicle_spawnlist` lines, each listing its mask's ids in slot order, build
// the file's registry as it is: the parser gives each id a slot as a line first names it [orig: @0x4A0253;
// @0x49DFC0], so they do when every slot's id is first named by the item that first named it (a file read
// and kept as read). Else the first item registers every slot before its own line.
bool lines_build_registry(const DefItemsFile &file) {
	std::vector<int> built;
	for (size_t i = 0; i < file.count; ++i)
		for (int slot = 0; slot < file.vehicle_spawn_id_count && slot < DEF_VEHICLE_SPAWN_SLOTS; ++slot)
			if (file.entries[i].vehicle_spawn_mask & (uint32_t(1) << slot)) {
				const int id = file.vehicle_spawn_ids[slot];
				if (std::find(built.begin(), built.end(), id) == built.end()) built.push_back(id);
			}
	return built.size() == size_t(file.vehicle_spawn_id_count) &&
	       std::equal(built.begin(), built.end(), file.vehicle_spawn_ids);
}

DefWriteResult write_items(const DefItemsFile &file, WriteOrder &order) {
	DefRecordWriter writer = writer_for(file.layout, order);
	writer.result.text = "// Item definitions\r\n\r\n";
	writer.items = &file;
	incomplete(writer, file.unmodeled_count, "");
	if (file.vehicle_spawn_id_count < 0 || file.vehicle_spawn_id_count > DEF_VEHICLE_SPAWN_SLOTS ||
		(file.vehicle_spawn_id_count && !file.count))
		writer.fail("", "pcvehicle_spawnlist", "The vehicle spawn registry cannot be represented.");
	const bool own_lines = order.keep && lines_build_registry(file);
	for (size_t i = 0; i < file.count; ++i) {
		const auto &item = file.entries[i];
		order_record(writer, order, i, item.line_order, item.display_name);
		if (!header(writer, "begin", item.display_name, sizeof(item.display_name), true)) continue;
        incomplete(writer, item.unmodeled_count, item.display_name);
        if (item.powerup_def[0] && (item.deathtime_ticks || item.clipsize || item.door_type ||
            item.door_open_rate_q16 || item.door_max_angle_bam || item.door_open_sound[0] || item.door_close_sound[0]))
            writer.fail(item.display_name, "powerup_def", "Powerup names and door/death numeric values share a native union; choose one branch.");
		// Register file-wide slots in their original order, then set this item's mask, where the items'
		// own lines do not build the registry. The parser's registry is cumulative; a repeated list
		// replaces only the mask.
		if (!own_lines && i == 0 && file.vehicle_spawn_id_count > 0 &&
		    file.vehicle_spawn_id_count <= DEF_VEHICLE_SPAWN_SLOTS) {
			std::vector<std::string> ids;
			for (int j = 0; j < file.vehicle_spawn_id_count; ++j) ids.push_back(std::to_string(file.vehicle_spawn_ids[j]));
			writer.line("pcvehicle_spawnlist", ids);
			if (!item.vehicle_spawn_mask) writer.line("pcvehicle_spawnlist", {});
		}
		writer.record(DefRecordKind::Item, &item, item.display_name, [&](uint8_t step) {
			if (step != DEF_LINE_ORDER_ROWS) return;
			for (size_t j = 0; j < item.emplacement_attachments_count; ++j)
				writer.record(DefRecordKind::Attachment, &item.emplacement_attachments[j], item.display_name);
		});
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
		check_one(writer, order, i, [&] {
			check_record(writer, DefRecordKind::Item, &a, &b, a.display_name);
			for (size_t j = 0; j < std::min(a.emplacement_attachments_count, b.emplacement_attachments_count); ++j)
				check_record(writer, DefRecordKind::Attachment, &a.emplacement_attachments[j], &b.emplacement_attachments[j], a.display_name);
		});
	}
	if (file.vehicle_spawn_id_count != parsed.vehicle_spawn_id_count ||
		std::memcmp(file.vehicle_spawn_ids, parsed.vehicle_spawn_ids, sizeof(file.vehicle_spawn_ids)))
		writer.fail("", "pcvehicle_spawnlist", "Vehicle spawn registry changed during serialization.");
	def_free_items(&parsed);
	return finish(writer);
}

DefWriteResult write_weapons(const DefWeaponsFile &file, WriteOrder &order) {
	DefRecordWriter writer = writer_for(file.layout, order);
	writer.result.text = "// Weapon definitions\r\n\r\n";
	incomplete(writer, file.unmodeled_count, "");
	// The carry limits at the top level, as the file states them.
	writer.depth = 0;
	for (size_t i = 0; i < file.ammo_classes_count; ++i)
		writer.record(DefRecordKind::Carry, &file.ammo_classes[i], file.ammo_classes[i].name);
	writer.depth = 1;
	for (size_t i = 0; i < file.count; ++i) {
		const auto &weapon = file.entries[i];
		order_record(writer, order, i, weapon.line_order, weapon.weapon_name);
		if (!header(writer, "weapon", weapon.weapon_name, sizeof(weapon.weapon_name), true)) continue;
		incomplete(writer, weapon.unmodeled_count, weapon.weapon_name);
		writer.record(DefRecordKind::Weapon, &weapon, weapon.weapon_name, [&](uint8_t step) {
			if (step == DEF_LINE_ORDER_ROWS) {
				for (size_t j = 0; j < weapon.sights_count; ++j)
					writer.record(DefRecordKind::Sight, &weapon.sights[j], weapon.weapon_name);
				return;
			}
			// Each action block a level in, its lines a level further.
			for (size_t j = 0; j < weapon.actions_count; ++j) {
				const auto &action = weapon.actions[j];
				if (!header(writer, "action", action.name, sizeof(action.name), true, writer.margin(1))) continue;
				incomplete(writer, action.unmodeled_count, action.name);
				writer.depth = 2;
				writer.record(DefRecordKind::Action, &action, action.name);
				writer.depth = 1;
				writer.result.text += writer.margin(1) + "end\r\n";
			}
		});
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
		check_one(writer, order, i, [&] {
			check_record(writer, DefRecordKind::Weapon, &a, &b, a.weapon_name);
			for (size_t j = 0; j < std::min(a.actions_count, b.actions_count); ++j)
				check_record(writer, DefRecordKind::Action, &a.actions[j], &b.actions[j], a.weapon_name);
			for (size_t j = 0; j < std::min(a.sights_count, b.sights_count); ++j)
				check_record(writer, DefRecordKind::Sight, &a.sights[j], &b.sights[j], a.weapon_name);
		});
	}
	def_free_weapons(&parsed);
	return finish(writer);
}

DefWriteResult write_ammo(const DefAmmoFile &file, WriteOrder &order) {
	DefRecordWriter writer = writer_for(file.layout, order);
	writer.result.text = "// Ammo definitions\r\n\r\n";
	incomplete(writer, file.unmodeled_count, "");
	for (size_t i = 0; i < file.count; ++i) {
		const auto &ammo = file.entries[i];
		order_record(writer, order, i, ammo.line_order, ammo.name);
		if (!header(writer, "ammo", ammo.name, sizeof(ammo.name), false)) continue;
		incomplete(writer, ammo.unmodeled_count, ammo.name);
		writer.record(DefRecordKind::Ammo, &ammo, ammo.name, [&](uint8_t step) {
			if (step != DEF_LINE_ORDER_ROWS || !ammo.effects_table_count) return;
			// The table a level in, its rows a level further.
			writer.line("effects_table", {});
			writer.depth = 2;
			for (size_t j = 0; j < ammo.effects_table_count; ++j)
				writer.record(DefRecordKind::Effect, &ammo.effects_table[j], ammo.name);
			writer.depth = 1;
			writer.line("end", {});
		});
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
		check_one(writer, order, i, [&] {
			check_record(writer, DefRecordKind::Ammo, &a, &b, a.name);
			for (size_t j = 0; j < std::min(a.effects_table_count, b.effects_table_count); ++j)
				check_record(writer, DefRecordKind::Effect, &a.effects_table[j], &b.effects_table[j], a.name);
		});
	}
	def_free_ammo(&parsed);
	return finish(writer);
}

} // namespace

DefWriteResult def_write_items(const DefItemsFile &file) { return write_in_order(file, write_items); }
DefWriteResult def_write_weapons(const DefWeaponsFile &file) { return write_in_order(file, write_weapons); }
DefWriteResult def_write_ammo(const DefAmmoFile &file) { return write_in_order(file, write_ammo); }

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
