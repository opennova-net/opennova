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
	// The records written in the writer's form over a file's layout (their own would read back otherwise),
	// each a line saying what of the file's form it does not keep (def_compose's `lost`).
	std::vector<std::string> rewritten;
	bool ok() const { return diagnostics.empty(); }
};

struct DefTextNotes;

// A family's file from its records, read back through the family's parser and compared, field by field.
// Given the layout it was read with (def_notes.h), every line is generated from the records and that data
// in the file's order and form (an unchanged file comes out as it was, a changed line differs by the words
// that changed); a record whose generated form the reparse check refuses is written in the writer's own
// lines (its comment lines and the lines the game skips kept, what else it loses named: `rewritten`), then
// in the table's order (`reordered`).
DefWriteResult def_write_items(const DefItemsFile &file, const DefTextNotes *notes = nullptr);
DefWriteResult def_write_weapons(const DefWeaponsFile &file, const DefTextNotes *notes = nullptr);
DefWriteResult def_write_ammo(const DefAmmoFile &file, const DefTextNotes *notes = nullptr);
// powerup.def: each row's `powerup "<name>"` block, its lines, its ammo rows and its written action
// blocks, read back through the family's parser and compared as the others are.
DefWriteResult def_write_powerup(const DefPowerupFile &file, const DefTextNotes *notes = nullptr);

} // namespace opennova::def
