#pragma once

#include <string>
#include <vector>

#include <formats/def/def.h>
#include <formats/def/def_schema.h>

namespace opennova::def {

struct DefWriteResult {
	std::string text;
	DefParseReport diagnostics;
	// The records written in the table's order although they were read in an order of their own: their
	// own would read back otherwise (def_write.cpp's write_in_order). Empty: every record kept its order.
	std::vector<std::string> reordered;
	bool ok() const { return diagnostics.empty(); }
};

struct DefTextNotes;

// A family's file from its records, read back through the family's parser and compared, field by field.
// Given the notes it was read with (def_notes.h), a record's lines are put down as the file has them while
// what the writer puts down for them is unchanged, and a changed line keeps its blanks, its comment and its
// unchanged words; a record's notes the reparse check refuses give way to the writer's own lines, then to
// the table's order.
DefWriteResult def_write_items(const DefItemsFile &file, const DefTextNotes *notes = nullptr);
DefWriteResult def_write_weapons(const DefWeaponsFile &file, const DefTextNotes *notes = nullptr);
DefWriteResult def_write_ammo(const DefAmmoFile &file, const DefTextNotes *notes = nullptr);
// powerup.def: each row's `powerup "<name>"` block, its lines, its ammo rows and its written action
// blocks, read back through the family's parser and compared as the others are.
DefWriteResult def_write_powerup(const DefPowerupFile &file, const DefTextNotes *notes = nullptr);

} // namespace opennova::def
