#include "blank_makers.h"
#include <editor/session/finding_codes.h>
#include <formats/def/def_write.h>
#include <cstring>

namespace opennova::editor {
namespace {
bool emit(const def::DefWriteResult &result, std::vector<uint8_t> &out, Diagnostic &error) {
	if (!result.ok()) {
		error = make_finding(CoreFinding::BlankDef, DiagnosticSeverity::Error, result.diagnostics.front().message);
		return false;
	}
	out.assign(result.text.begin(), result.text.end()); return true;
}
}
bool make_blank_items_def(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error) {
	def::DefItemDef item{}; def::def_init_item(item);
	std::memcpy(item.display_name, "Null", 5);
	item.id = 100000; item.type = def::DEF_ITEM_TYPE_MARKER;
	def::DefItemsFile file{}; file.entries = &item; file.count = 1;
	return emit(def::def_write_items(file), out, error);
}
bool make_blank_weapon_def(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error) {
	return emit(def::def_write_weapons({}), out, error);
}
bool make_blank_ammo_def(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &error) {
	def::DefAmmoDef ammo{}; def::def_init_ammo(ammo);
	std::memcpy(ammo.name, "AT_NULL", 8); ammo.drag_fp16 = 0x10000;
	def::DefAmmoFile file{}; file.entries = &ammo; file.count = 1;
	return emit(def::def_write_ammo(file), out, error);
}
bool make_blank_charattr_def(const BlankRequest &request, std::vector<uint8_t> &out, Diagnostic &) {
	blank_text_to_bytes("// Character attributes of " +
		(request.project_title.empty() ? std::string("the project") : request.project_title) +
		". Classes go in [CHARACTER1] .. [CHARACTER16] sections.\n", out);
	return true;
}
} // namespace opennova::editor
