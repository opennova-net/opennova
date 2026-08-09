#include "network/item_replication_catalog_adapter.h"

#include "object/nova_item_database.h"

#include <utility>
#include <vector>

namespace godot {

std::shared_ptr<const opennova::netsim::ItemReplicationCatalog>
build_item_replication_catalog(const Ref<ItemDatabase> &item_database) {
	if (item_database.is_null()) return {};
	std::vector<opennova::netsim::ItemReplicationDefinition> definitions;
	const auto &records =
			item_database->get_replication_definition_records();
	definitions.reserve(records.size());
	for (const ItemDatabase::ReplicationDefinitionRecord &record : records) {
		opennova::netsim::ItemReplicationDefinition definition;
		definition.definition_id = record.definition_id;
		definition.item_type = record.item_type;
		definition.attrib = record.attrib;
		definition.attrib2 = record.attrib2;
		definition.physics = record.physics;
		definition.ai_function = record.ai_function.utf8().get_data();
		definition.move_function = record.move_function.utf8().get_data();
		definition.render_function = record.render_function.utf8().get_data();
		definition.disk_function = record.disk_function.utf8().get_data();
		definitions.push_back(std::move(definition));
	}
	return std::make_shared<const opennova::netsim::ItemReplicationCatalog>(
			opennova::netsim::ItemReplicationCatalog::from_definitions(definitions));
}

} // namespace godot
