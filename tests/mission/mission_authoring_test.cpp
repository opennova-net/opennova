// Unit test for the items.def type -> BMS entity-pool classification used
// while importing .mis text.

#include "common/test_expect.h"
#include "def/def.h"
#include "mission/authoring.h"

using opennova::mission::EntityKind;
namespace authoring = opennova::mission::authoring;

int main() {
	// Types are the witnessed engine values at ItemDef+0x5C [orig:
	// ItemDef_ParseProperty @ 0x49eb00; docs/world/itemdef-re.md D-ITEMDEF-1]:
	// vehicle=1, decoration/foliage=2, person=3, marker=4, building=5,
	// powerup/object=6, effect=8; 0=unset, 7 unused. Decoration and Foliage
	// (value 2) land in Building alongside Building (5); every other type is Item.
	TEST_EXPECT(authoring::entity_kind_for_item_type(DEF_ITEM_TYPE_PERSON) == EntityKind::Organic);      // 3
	TEST_EXPECT(authoring::entity_kind_for_item_type(DEF_ITEM_TYPE_BUILDING) == EntityKind::Building);   // 5
	TEST_EXPECT(authoring::entity_kind_for_item_type(DEF_ITEM_TYPE_DECORATION) == EntityKind::Building); // 2
	TEST_EXPECT(authoring::entity_kind_for_item_type(DEF_ITEM_TYPE_FOLIAGE) == EntityKind::Building);    // 2 (= decoration)
	TEST_EXPECT(authoring::entity_kind_for_item_type(DEF_ITEM_TYPE_MARKER) == EntityKind::Marker);       // 4
	TEST_EXPECT(authoring::entity_kind_for_item_type(DEF_ITEM_TYPE_UNSET) == EntityKind::Item);          // 0
	TEST_EXPECT(authoring::entity_kind_for_item_type(DEF_ITEM_TYPE_VEHICLE) == EntityKind::Item);        // 1
	TEST_EXPECT(authoring::entity_kind_for_item_type(DEF_ITEM_TYPE_POWERUP) == EntityKind::Item);        // 6
	TEST_EXPECT(authoring::entity_kind_for_item_type(DEF_ITEM_TYPE_OBJECT) == EntityKind::Item);         // 6 (= powerup)
	TEST_EXPECT(authoring::entity_kind_for_item_type(DEF_ITEM_TYPE_EFFECT) == EntityKind::Item);         // 8
	TEST_EXPECT(authoring::entity_kind_for_item_type(7) == EntityKind::Item);                            // unused engine value
	TEST_EXPECT(authoring::entity_kind_for_item_type(-1) == EntityKind::Item);                           // no db loaded

	return 0;
}
