// The order and the indentation a def file was read with (def.h's DefLineOrder and DefLayout), which
// an authoring writer keeps: no text of the file, a place in the property table per line and the
// blanks one level of a block takes.
#include "def_schema.h"

#include <array>
#include <cctype>
#include <string>
#include <unordered_map>

namespace opennova::def {
namespace {

// The keys a line's first token names its property by, any case: each property's own key; the squib,
// door and part keys sharing the death, door and clip words, each its own step (def.h) [orig:
// ItemDef_ParseProperty @ 0x49EB00, the squib arms @0x49F06F, num_doors @0x49F766, rotor_parts
// @0x49EF5D]; the aliases the parsers read into a property's members (particletesttime the nameless
// dawn timing; animcal the anim map animadm fills [orig: WeaponDefs_ParseLineCallback @ 0x543D77]; an
// action's delay its delayend [orig: ActionDef_ParseScriptLine @0x402B2C]); and the keys that open a
// record's rows and blocks.
struct Alias {
	DefRecordKind kind;
	const char *key;
	const char *property; // the property's key; "" for a step of rows or blocks
	uint8_t step;
};
const Alias kAliases[] = {
	// The names sharing the death, door and clip words, each a step of its own (def.h).
	{DefRecordKind::Item, "sqb_rate", "", DEF_LINE_ORDER_SQB_RATE},
	{DefRecordKind::Item, "num_doors", "", DEF_LINE_ORDER_NUM_DOORS},
	{DefRecordKind::Item, "first_door", "", DEF_LINE_ORDER_FIRST_DOOR},
	{DefRecordKind::Item, "first_subobject", "", DEF_LINE_ORDER_FIRST_SUBOBJECT},
	{DefRecordKind::Item, "rotor_parts", "", DEF_LINE_ORDER_ROTOR_PARTS},
	{DefRecordKind::Item, "aux_parts", "", DEF_LINE_ORDER_AUX_PARTS},
	{DefRecordKind::Item, "sqb_error", "", DEF_LINE_ORDER_SQB_ERROR},
	{DefRecordKind::Item, "sqb_distance", "", DEF_LINE_ORDER_SQB_DISTANCE},
	{DefRecordKind::Item, "door_dir", "", DEF_LINE_ORDER_DOOR_DIR},
	{DefRecordKind::Item, "particletesttime", "dawnshot", 0},
	{DefRecordKind::Item, "addeweap", "", DEF_LINE_ORDER_ROWS},
	{DefRecordKind::Item, "addeweapg", "", DEF_LINE_ORDER_ROWS},
	{DefRecordKind::Item, "addeweapc", "", DEF_LINE_ORDER_ROWS},
	{DefRecordKind::Weapon, "animcal", "animadm", 0},
	{DefRecordKind::Weapon, "sights", "", DEF_LINE_ORDER_ROWS},
	{DefRecordKind::Weapon, "action", "", DEF_LINE_ORDER_BLOCKS},
	{DefRecordKind::Action, "delay", "delayend", 0},
	{DefRecordKind::Ammo, "effects_table", "", DEF_LINE_ORDER_ROWS},
};

std::string lower(const char *text, size_t length) {
	std::string out(text, length);
	for (char &c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
	return out;
}

// Each kind's keys to their steps, made once.
const std::unordered_map<std::string, uint8_t> &steps_of(DefRecordKind kind) {
	static const std::array<std::unordered_map<std::string, uint8_t>, kDefRecordKindCount> tables = [] {
		std::array<std::unordered_map<std::string, uint8_t>, kDefRecordKindCount> out;
		for (size_t k = 0; k < kDefRecordKindCount; ++k) {
			const std::vector<DefProperty> &properties = def_properties(DefRecordKind(k));
			for (size_t i = 0; i < properties.size() && i < DEF_LINE_ORDER_SQB_RATE; ++i)
				if (!properties[i].key.empty())
					out[k].emplace(lower(properties[i].key.data(), properties[i].key.size()), uint8_t(i));
		}
		for (const Alias &alias : kAliases) {
			auto &table = out[size_t(alias.kind)];
			if (alias.step) {
				table.emplace(alias.key, alias.step);
				continue;
			}
			const auto found = table.find(alias.property);
			if (found != table.end()) table.emplace(alias.key, found->second);
		}
		return out;
	}();
	return tables[size_t(kind)];
}

bool blank(char c) { return c == ' ' || c == '\t'; }

} // namespace

DefLineOrder *def_line_order(DefRecordKind kind, void *record) {
	switch (kind) {
	case DefRecordKind::Item: return &static_cast<DefItemDef *>(record)->line_order;
	case DefRecordKind::Weapon: return &static_cast<DefWeaponDef *>(record)->line_order;
	case DefRecordKind::Ammo: return &static_cast<DefAmmoDef *>(record)->line_order;
	case DefRecordKind::Action: return &static_cast<DefWeaponAction *>(record)->line_order;
	default: return nullptr;
	}
}

const DefLineOrder *def_line_order(DefRecordKind kind, const void *record) {
	return def_line_order(kind, const_cast<void *>(record));
}

void def_note_line(DefRecordKind kind, DefLineOrder &order, const char *line, size_t length) {
	size_t start = 0;
	while (start < length && blank(line[start])) ++start;
	size_t end = start;
	while (end < length && !blank(line[end]) && line[end] != '\r' && line[end] != '\n' && line[end] != '"' &&
	       line[end] != ',')
		++end;
	if (end == start) return;
	const auto &steps = steps_of(kind);
	const auto found = steps.find(lower(line + start, end - start));
	if (found == steps.end()) return;
	for (size_t i = 0; i < order.count; ++i)
		if (order.steps[i] == found->second) return;
	if (order.count < DEF_LINE_ORDER_MAX) order.steps[order.count++] = found->second;
}

void def_note_indent(DefLayout &layout, bool &noted, const char *line, size_t length) {
	if (noted) return;
	noted = true;
	size_t blanks = 0;
	while (blanks < length && blank(line[blanks]) && blanks + 1 < sizeof(layout.indent)) ++blanks;
	for (size_t i = 0; i < blanks; ++i) layout.indent[i] = line[i];
	layout.indent[blanks] = '\0';
}

} // namespace opennova::def
