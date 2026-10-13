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

// A family's file in the writer's form from its records alone (each record's lines in the order it was read
// in, DefLineOrder, indented as the file was, DefLayout), read back through the family's parser and compared,
// field by field; a record whose order the reparse check refuses is written in the table's order
// (`reordered`).
DefWriteResult def_write_items(const DefItemsFile &file);
DefWriteResult def_write_weapons(const DefWeaponsFile &file);
DefWriteResult def_write_ammo(const DefAmmoFile &file);
// powerup.def: each row's `powerup "<name>"` block, its lines, its ammo rows and its written action
// blocks, read back through the family's parser and compared as the others are.
DefWriteResult def_write_powerup(const DefPowerupFile &file);

} // namespace opennova::def
