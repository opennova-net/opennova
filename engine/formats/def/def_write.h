#pragma once

#include <formats/def/def.h>
#include <formats/def/def_schema.h>

namespace opennova::def {

struct DefWriteResult {
	std::string text;
	DefParseReport diagnostics;
	bool ok() const { return diagnostics.empty(); }
};

DefWriteResult def_write_items(const DefItemsFile &file);
DefWriteResult def_write_weapons(const DefWeaponsFile &file);
DefWriteResult def_write_ammo(const DefAmmoFile &file);

} // namespace opennova::def
