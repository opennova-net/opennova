#include <runtime/replication/item_replication_catalog.h>

#include <formats/def/def.h>

#include <cstdio>
#include <cstring>
#include <optional>
#include <vector>

namespace {

namespace ns = opennova::netsim;
using opennova::EntityClass;

bool expect(bool condition, const char *message) {
	if (condition) return true;
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

void copy_tag(char (&out)[16], const char *value) {
	std::snprintf(out, sizeof(out), "%s", value);
}

DefItemDef make_item(int id, int type, const char *ai, const char *move,
		uint32_t attrib = 0, int physics = 0) {
	DefItemDef item{};
	item.id = id;
	item.type = type;
	item.attrib = attrib;
	item.physics = physics;
	copy_tag(item.ai_function, ai);
	copy_tag(item.move_function, move);
	return item;
}

bool run_items_def_catalog() {
	DefItemDef entries[] = {
			make_item(105305, DEF_ITEM_TYPE_PERSON, "plyr", "org2",
					DEF_ITEM_ATTRIB_AIDATA | DEF_ITEM_ATTRIB_PLAYERCONTROL),
			make_item(105311, DEF_ITEM_TYPE_PERSON, "org1", "org1",
					DEF_ITEM_ATTRIB_AIDATA),
			// Wire callback and physical mover deliberately differ.
			make_item(101291, DEF_ITEM_TYPE_VEHICLE, "chel", "cveh",
					DEF_ITEM_ATTRIB_PLAYERCONTROL, 1),
			make_item(101400, DEF_ITEM_TYPE_VEHICLE, "cveh", "cbik", 0, 1),
			// Pool-1 emplacements are not vehicle compact records.
			make_item(101419, DEF_ITEM_TYPE_OBJECT, "ewep", "cveh",
					DEF_ITEM_ATTRIB_EWEAP),
			make_item(101500, DEF_ITEM_TYPE_EFFECT, "rokt", "rokt",
					DEF_ITEM_ATTRIB_MISSILE),
			// A known motion family must not rescue an unknown wire callback.
			make_item(101501, DEF_ITEM_TYPE_PERSON, "zzzz", "org1"),
			make_item(101502, DEF_ITEM_TYPE_BUILDING, "", ""),
			// Catalogued by full id, but not addressable by the 16-bit wire id.
			make_item(200000, DEF_ITEM_TYPE_OBJECT, "null", "null"),
	};
	copy_tag(entries[0].render_function, "pers");
	copy_tag(entries[0].disk_function, "orgd");
	entries[0].attrib2 = 0x12345678u;
	DefItemsFile items{entries, sizeof(entries) / sizeof(entries[0])};
	const ns::ItemReplicationCatalog catalog =
			ns::ItemReplicationCatalog::from_items_def(items);
	if (!expect(catalog.valid(), "unique items.def builds a valid catalog")) return false;
	if (!expect(catalog.profiles().size() == 9, "all item definitions retained")) return false;

	const ns::ItemReplicationProfile *player = catalog.by_wire_type(5305);
	if (!expect(player != nullptr, "player Person indexed by wire type")) return false;
	if (!expect(player->compact_codec == ns::WireCompactCodec::PlayerPerson &&
	                    player->motion_family == ns::MotionFamily::Person &&
	                    player->wire_entity_class() == EntityClass::Player,
	            "player Person keeps codec and motion as separate axes")) return false;
	if (!expect(player->allocation.retail_pool == ns::StoragePool::Unresolved,
	            "catalog does not guess the retail allocation pool")) return false;
	const std::optional<EntityClass> player_resolution =
			catalog.resolve_wire_entity_class(5305);
	if (!expect(player_resolution.has_value() &&
				*player_resolution == EntityClass::Player,
			"catalog presence and resolved wire class are distinct")) return false;
	if (!expect(player->allocation.attrib2 == 0x12345678u &&
	                    player->callbacks.ai_function == "plyr" &&
	                    player->callbacks.move_function == "org2" &&
	                    player->callbacks.render_function == "pers" &&
	                    player->callbacks.disk_function == "orgd",
	            "all raw allocation inputs and callback axes are retained")) return false;
	if (!expect(ns::has_capability(player->capabilities,
	                    ns::ItemReplicationCapability::AiData) &&
	                    ns::has_capability(player->capabilities,
	                            ns::ItemReplicationCapability::PlayerControl) &&
	                    ns::has_capability(player->capabilities,
	                            ns::ItemReplicationCapability::CompactReplication),
	            "raw item capabilities retained")) return false;

	const ns::ItemReplicationProfile *soldier = catalog.by_definition_id(105311);
	if (!expect(soldier != nullptr &&
	                    soldier->compact_codec == ns::WireCompactCodec::OrganicPerson &&
	                    soldier->motion_family == ns::MotionFamily::Person,
	            "AI-controlled Person is a Person, not a Player")) return false;

	const ns::ItemReplicationProfile *buggy = catalog.by_wire_type(1291);
	if (!expect(buggy != nullptr && buggy->compact_codec == ns::WireCompactCodec::Vehicle &&
	                    buggy->motion_family == ns::MotionFamily::GroundVehicle &&
	                    buggy->callbacks.effective_network_callback == "chel",
	            "ai callback selects wire codec while move callback selects motion")) return false;
	if (!expect(ns::has_capability(buggy->capabilities,
	                    ns::ItemReplicationCapability::VehiclePhysics),
	            "vehicle physics capability retained")) return false;

	const ns::ItemReplicationProfile *bike = catalog.by_wire_type(1400);
	if (!expect(bike != nullptr && bike->compact_codec == ns::WireCompactCodec::Vehicle &&
	                    bike->motion_family == ns::MotionFamily::LightVehicle,
	            "light vehicle motion remains independent of vehicle codec")) return false;

	const ns::ItemReplicationProfile *emplacement = catalog.by_wire_type(1419);
	if (!expect(emplacement != nullptr &&
	                    emplacement->wire_path == ns::WireReplicationPath::None &&
	                    emplacement->compact_codec == ns::WireCompactCodec::None &&
	                    emplacement->motion_family == ns::MotionFamily::GroundVehicle &&
	                    emplacement->wire_entity_class() == EntityClass::NoNetworkCallback,
	            "emplacement is not guessed to be a vehicle compact")) return false;

	const ns::ItemReplicationProfile *guided = catalog.by_wire_type(1500);
	if (!expect(guided != nullptr &&
	                    guided->wire_path == ns::WireReplicationPath::Guided &&
	                    guided->compact_codec == ns::WireCompactCodec::None &&
	                    guided->motion_family == ns::MotionFamily::Guided,
	            "guided path remains separate from compact codecs")) return false;

	const ns::ItemReplicationProfile *mystery = catalog.by_wire_type(1501);
	if (!expect(mystery != nullptr &&
	                    mystery->wire_path == ns::WireReplicationPath::Unresolved &&
	                    mystery->compact_codec == ns::WireCompactCodec::Unresolved &&
	                    mystery->motion_family == ns::MotionFamily::Person &&
	                    catalog.wire_entity_class_for(1501) == EntityClass::Unknown,
	            "unknown callback fails closed despite known motion")) return false;
	const std::optional<EntityClass> mystery_resolution =
			catalog.resolve_wire_entity_class(1501);
	if (!expect(mystery_resolution.has_value() &&
				*mystery_resolution == EntityClass::Unknown,
			"catalogued unresolved callback is a terminal Unknown")) return false;

	const ns::ItemReplicationProfile *blank = catalog.by_wire_type(1502);
	if (!expect(blank != nullptr && blank->wire_path == ns::WireReplicationPath::None &&
	                    blank->motion_family == ns::MotionFamily::Static,
	            "absent callbacks are an explicit non-replicated profile")) return false;

	const ns::ItemReplicationProfile *wide = catalog.by_definition_id(200000);
	if (!expect(wide != nullptr && !wide->wire_type_id.has_value(),
	            "non-wire definition remains available by full id")) return false;
	return expect(!catalog.resolve_wire_entity_class(0xFFFF).has_value() &&
	                      catalog.by_wire_type(0xFFFF) == nullptr &&
	                      catalog.compact_codec_for(0xFFFF) ==
	                              ns::WireCompactCodec::Unresolved &&
	                      catalog.wire_entity_class_for(0xFFFF) == EntityClass::Unknown,
	              "missing wire id fails closed");
}

// The shipped JOX corpus authors these families as 5-char and mixed-case
// tokens (`cbike`, `ctank`, `catv`, `CHel`, `ENVS`); the retail table keys
// are 4-byte fourccs the loader resolves them onto (vehicle-client-movers-re
// family table). Exact-string matching stranded every one of them — the
// Motorcycle ran the Ground motor and the tanks/ATVs came back Unresolved.
bool run_retail_corpus_tokens() {
	DefItemDef entries[] = {
			make_item(101100, DEF_ITEM_TYPE_VEHICLE, "cbik", "cbike",
					DEF_ITEM_ATTRIB_PLAYERCONTROL, 1),   // Motorcycle
			make_item(101101, DEF_ITEM_TYPE_VEHICLE, "cveh", "ctank",
					DEF_ITEM_ATTRIB_PLAYERCONTROL, 1),   // M1A1 / T80
			make_item(101102, DEF_ITEM_TYPE_VEHICLE, "cveh", "catv",
					DEF_ITEM_ATTRIB_PLAYERCONTROL, 1),   // ATV / BTR-80
			make_item(101103, DEF_ITEM_TYPE_VEHICLE, "CHel", "CHel",
					DEF_ITEM_ATTRIB_PLAYERCONTROL, 1),   // authored-case heli
			make_item(101104, DEF_ITEM_TYPE_OBJECT, "ENVS", "ENVS"),
	};
	DefItemsFile items{entries, sizeof(entries) / sizeof(entries[0])};
	const ns::ItemReplicationCatalog catalog =
			ns::ItemReplicationCatalog::from_items_def(items);
	if (!expect(catalog.valid(), "corpus-token items build a valid catalog")) return false;

	const ns::ItemReplicationProfile *bike = catalog.by_wire_type(1100);
	if (!expect(bike != nullptr &&
	                    bike->motion_family == ns::MotionFamily::LightVehicle,
	            "cbike resolves the bike fourcc row, not Ground")) return false;
	const ns::ItemReplicationProfile *tank = catalog.by_wire_type(1101);
	if (!expect(tank != nullptr &&
	                    tank->motion_family == ns::MotionFamily::GroundVehicle,
	            "ctank resolves the ground fourcc row")) return false;
	const ns::ItemReplicationProfile *atv = catalog.by_wire_type(1102);
	if (!expect(atv != nullptr &&
	                    atv->motion_family == ns::MotionFamily::GroundVehicle,
	            "catv resolves the ground fourcc row")) return false;
	const ns::ItemReplicationProfile *heli = catalog.by_wire_type(1103);
	if (!expect(heli != nullptr &&
	                    heli->motion_family == ns::MotionFamily::Aircraft &&
	                    heli->wire_entity_class() == EntityClass::Vehicle,
	            "authored-case CHel keeps the air family and vehicle codec")) return false;
	const ns::ItemReplicationProfile *decoration = catalog.by_wire_type(1104);
	return expect(decoration != nullptr &&
	                      decoration->wire_entity_class() ==
	                              EntityClass::NoNetworkCallback,
	              "uppercase ENVS is the witnessed null-callback class, "
	              "not a fail-closed Unknown");
}

bool run_duplicate_ids_fail_closed() {
	ns::ItemReplicationDefinition first;
	first.definition_id = 101000;
	first.ai_function = "org1";
	first.move_function = "org1";
	ns::ItemReplicationDefinition second = first;
	second.ai_function = "cveh";
	second.move_function = "cveh";
	const ns::ItemReplicationCatalog catalog =
			ns::ItemReplicationCatalog::from_definitions({first, second});
	return expect(!catalog.valid() && catalog.issues().size() == 2,
	                      "duplicate definition and wire ids are diagnosed") &&
			expect(catalog.by_definition_id(101000) == nullptr &&
			                      catalog.by_wire_type(1000) == nullptr &&
			                      catalog.resolve_wire_entity_class(1000).has_value() &&
			                      catalog.wire_entity_class_for(1000) == EntityClass::Unknown,
			              "duplicate wire codec remains present and fails closed");
}

} // namespace

int main() {
	const bool ok = run_items_def_catalog() && run_retail_corpus_tokens() &&
			run_duplicate_ids_fail_closed();
	if (ok) std::printf("item_replication_catalog: OK\n");
	return ok ? 0 : 1;
}
