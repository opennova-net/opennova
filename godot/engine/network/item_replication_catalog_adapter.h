#pragma once

#include <godot_cpp/classes/ref.hpp>

#include <netsim/item_replication_catalog.h>

#include <memory>

namespace godot {

class NovaItemDatabase;

// One Godot binding adapter from NovaItemDatabase to the portable immutable
// catalog. Both live simulation and spectate call this; callback interpretation
// stays canonical in engine/net/netsim.
std::shared_ptr<const opennova::netsim::ItemReplicationCatalog>
build_item_replication_catalog(const Ref<NovaItemDatabase> &item_database);

} // namespace godot
