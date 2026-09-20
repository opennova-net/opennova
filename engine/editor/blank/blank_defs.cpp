#include "blank_makers.h"

namespace opennova::editor {

// The .def family has no structured writer yet (it lands with the item/weapon/ammo
// catalog); until then these are constant text templates, authored from scratch and
// written CRLF. Each holds the least the loaders accept: the Null marker item
// [orig: ItemDefs_LoadAndValidate @ 0x4a1da0 parses whatever blocks are present],
// a weapon table with no weapons [orig: WeaponDef_LoadAll @ 0x54dd10, a missing or
// empty table leaves the single "None" entry], the null ammo round [orig:
// AmmoDef_LoadAll @ 0x40b0b0], and a character-attribute file with no CHARACTER
// sections [orig: CharAttr_LoadFromDef @ 0x412140, absent sections leave the table
// inactive].

bool make_blank_items_def(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &) {
	blank_text_to_bytes(
	        "begin \"Null\"\n"
	        "  id 100000\n"
	        "  type marker\n"
	        "end\n",
	        out);
	return true;
}

bool make_blank_weapon_def(const BlankRequest &request, std::vector<uint8_t> &out, Diagnostic &) {
	blank_text_to_bytes("// Weapons of " + (request.project_title.empty() ? std::string("the project") : request.project_title) +
	                            ". Add weapons with the editor's catalog.\n",
	                    out);
	return true;
}

bool make_blank_ammo_def(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &) {
	blank_text_to_bytes(
	        "ammo AT_NULL\n"
	        "\tvelocity            0\n"
	        "\tmax_age             0\n"
	        "\tdrag                1\n"
	        "\tmin_damage          0\n"
	        "\tmax_damage          0\n"
	        "end\n",
	        out);
	return true;
}

bool make_blank_charattr_def(const BlankRequest &request, std::vector<uint8_t> &out, Diagnostic &) {
	blank_text_to_bytes("// Character attributes of " +
	                            (request.project_title.empty() ? std::string("the project") : request.project_title) +
	                            ". Classes go in [CHARACTER1] .. [CHARACTER16] sections.\n",
	                    out);
	return true;
}

} // namespace opennova::editor
