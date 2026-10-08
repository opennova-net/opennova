// The items.def ids the engine fixes (formats/def/reserved_items.h; docs/world/itemdef-re.md, "The ids
// and rows the engine fixes"): the table's shape, its lookups, the rule each family takes, and the
// runtime's own constants for the same ids.
#include <cstring>
#include <set>
#include <string>

#include <formats/def/def.h>
#include <formats/def/reserved_items.h>
#include <runtime/world/person_overlays.h>
#include <runtime/world/player_spawn.h>

#include "common/test_expect.h"

using namespace opennova::def;

// Every row: a type once, in order, its words said, a kind the parser makes; a place or an
// objective (Refuse) of the kind the engine looks for there, and every row retail ships named as
// retail names it.
static int test_table_shape() {
	size_t count = 0;
	const ReservedItem *rows = reserved_items(&count);
	TEST_EXPECT(rows && count == 46);
	std::set<int> types;
	for (size_t i = 0; i < count; ++i) {
		const ReservedItem &row = rows[i];
		TEST_EXPECT(types.insert(row.type).second);
		TEST_EXPECT(i == 0 || rows[i - 1].type < row.type);
		TEST_EXPECT(row.type > 0 && row.type < 10000);
		TEST_EXPECT(row.label && *row.label && row.use && *row.use && row.retail);
		TEST_EXPECT(row.kind == DEF_ITEM_TYPE_UNSET || row.kind == DEF_ITEM_TYPE_VEHICLE || row.kind == DEF_ITEM_TYPE_PERSON ||
		            row.kind == DEF_ITEM_TYPE_MARKER || row.kind == DEF_ITEM_TYPE_OBJECT);
		// A place or an objective is a kind the engine's pool walk finds it in.
		if (row.rule == ReservedItemRule::Refuse) TEST_EXPECT(row.kind != DEF_ITEM_TYPE_UNSET);
		TEST_EXPECT(reserved_item_by_type(row.type) == &row);
		TEST_EXPECT(reserved_item_by_id(DEF_ITEM_ID_BASE + row.type) == &row);
	}
	// The ids the code checks that retail ships no row of.
	for (const int type : {900, 2143, 4096, 4097, 4101, 4102, 4103, 5000})
		TEST_EXPECT(reserved_item_by_type(type) && !*reserved_item_by_type(type)->retail);
	TEST_EXPECT(!reserved_item_by_type(0) && !reserved_item_by_type(5310) && !reserved_item_by_type(6031) &&
	            !reserved_item_by_id(100000) && !reserved_item_by_id(6094) && !reserved_item_by_id(-1));
	TEST_EXPECT(DEF_ITEMS_FALLBACK_ROW == 0 && DEF_ITEMS_BRAKE_ROW == 1 && DEF_ITEM_ID_BASE == 100000);
	return 0;
}

// The families and the rule each takes: every start, waypoint, map marker, flag and bay is refused for
// another kind; a model or an actor the engine takes by its id is warned about.
static int test_families() {
	const auto refuses = [](int type, int kind) {
		const ReservedItem *row = reserved_item_by_type(type);
		return row && row->rule == ReservedItemRule::Refuse && row->kind == kind;
	};
	const auto warns = [](int type, int kind) {
		const ReservedItem *row = reserved_item_by_type(type);
		return row && row->rule == ReservedItemRule::Warn && row->kind == kind;
	};
	for (const int start : {6001, 6002, 6003, 6004, 6090, 6091, 6094, 6095, 6096, 6097, 6098, 6099})
		TEST_EXPECT(refuses(start, DEF_ITEM_TYPE_MARKER));
	for (const int marker : {2043, 2044, 6005, 6006, 6007, 6026, 6027, 6028, 6088, 6089, 6092, 6093})
		TEST_EXPECT(refuses(marker, DEF_ITEM_TYPE_MARKER));
	for (const int flag : {4091, 4093, 4095, 4096, 4097, 4098, 4100, 4101, 4102, 4103})
		TEST_EXPECT(refuses(flag, DEF_ITEM_TYPE_OBJECT));
	TEST_EXPECT(refuses(5305, DEF_ITEM_TYPE_PERSON));
	for (const int model : {185, 900, 1424, 1869, 1886, 1904, 2143}) TEST_EXPECT(warns(model, DEF_ITEM_TYPE_UNSET));
	TEST_EXPECT(warns(1281, DEF_ITEM_TYPE_VEHICLE) && warns(4520, DEF_ITEM_TYPE_PERSON) &&
	            warns(4529, DEF_ITEM_TYPE_PERSON) && warns(5000, DEF_ITEM_TYPE_PERSON));
	// The co-op insertion point, by its retail row and the base game's name.
	const ReservedItem *insertion = reserved_item_by_id(106094);
	TEST_EXPECT(insertion && std::strcmp(insertion->retail, "start, primary, player") == 0 &&
	            std::strcmp(insertion->label, "Insertion point") == 0);
	// A kind of any matches every type; a marker's only a marker.
	TEST_EXPECT(reserved_item_kind_matches(*reserved_item_by_type(185), DEF_ITEM_TYPE_VEHICLE) &&
	            reserved_item_kind_matches(*insertion, DEF_ITEM_TYPE_MARKER) &&
	            !reserved_item_kind_matches(*insertion, DEF_ITEM_TYPE_DECORATION));
	return 0;
}

// The runtime's constants for the same ids (the person overlays' three models, the player template) are
// rows of the table.
static int test_runtime_constants() {
	using namespace opennova::world;
	TEST_EXPECT(reserved_item_by_type(kParachuteItemTypeId) && reserved_item_by_type(kNightVisionGogglesItemTypeId) &&
	            reserved_item_by_type(kBinocularsItemTypeId) && reserved_item_by_type(kPlayerInfantryTypeId));
	TEST_EXPECT(reserved_item_by_type(kPlayerInfantryTypeId)->kind == DEF_ITEM_TYPE_PERSON);
	return 0;
}

int main() {
	int failures = 0;
	failures += test_table_shape();
	failures += test_families();
	failures += test_runtime_constants();
	if (failures == 0) std::printf("def_reserved_items: ok\n");
	return failures == 0 ? 0 : 1;
}
