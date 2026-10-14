#pragma once

#include <string>
#include <vector>

#include <formats/def/def.h>
#include <formats/def/def_schema.h>

namespace opennova::def {

struct DefWriteResult {
	std::string text;
	DefParseReport diagnostics;
	bool ok() const { return diagnostics.empty(); }
};

// A family's file in the writer's form from its records alone: each record's lines in its kind's property
// table's order, a tab a level, read back through the family's parser and compared, field by field (no original
// tool writes the catalogs: the form is OpenNova's). Its words are the game's: an attribute, a flag, a kill zone
// or a tracer type as the game's own name table spells it, a value no table names as its number.
DefWriteResult def_write_items(const DefItemsFile &file);
DefWriteResult def_write_weapons(const DefWeaponsFile &file);
DefWriteResult def_write_ammo(const DefAmmoFile &file);
// powerup.def: each row's `powerup "<name>"` block, its lines, its ammo rows and its written action
// blocks, read back through the family's parser and compared as the others are.
DefWriteResult def_write_powerup(const DefPowerupFile &file);

} // namespace opennova::def
