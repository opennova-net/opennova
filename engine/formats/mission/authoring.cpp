#include <formats/mission/authoring.h>

#include <formats/def/def.h>

namespace opennova::mission::authoring {

EntityKind entity_kind_for_item_type(int def_item_type) {
	// DefItemType carries the witnessed engine values [orig: ItemDef_ParseProperty
	// @ 0x49eb00; docs/world/itemdef-re.md D-ITEMDEF-1]; foliage shares 2 with
	// decoration and object shares 6 with powerup, so one case label covers each pair.
	switch (def_item_type) {
		case DEF_ITEM_TYPE_PERSON: // 3
			return EntityKind::Organic;
		case DEF_ITEM_TYPE_BUILDING:   // 5
		case DEF_ITEM_TYPE_DECORATION: // 2 (= DEF_ITEM_TYPE_FOLIAGE)
			return EntityKind::Building;
		case DEF_ITEM_TYPE_MARKER: // 4
			return EntityKind::Marker;
		default: // vehicle / powerup=object / effect / unset
			return EntityKind::Item;
	}
}

} // namespace opennova::mission::authoring
