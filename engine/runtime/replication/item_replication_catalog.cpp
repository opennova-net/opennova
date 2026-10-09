#include <runtime/replication/item_replication_catalog.h>

#include <cstring>
#include <utility>

#include <formats/def/def.h>
#include <runtime/world/physics_class_table.h>

using namespace opennova::def;

namespace opennova::replication {

namespace {

std::string bounded_string(const char *value, size_t capacity) {
	if (value == nullptr || capacity == 0) return {};
	const void *end = std::memchr(value, '\0', capacity);
	const size_t length = end == nullptr
			? capacity
			: static_cast<size_t>(static_cast<const char *>(end) - value);
	return std::string(value, length);
}

// The catalog's motion family of the physics row a move_function binds
// (world/physics_class_table.h): the whole token, any case, and a name the
// table lacks binds the null row, which is Static. A row this catalog has no
// family for (ewep, door, upfx, ...) is Unresolved.
MotionFamily motion_family_from_move_function(const std::string &move_function) {
	using world::PhysicsClass;
	switch (world::physics_class_from_move_function(move_function)) {
	case PhysicsClass::Null: return MotionFamily::Static;
	case PhysicsClass::Org0:
	case PhysicsClass::Org1:
	case PhysicsClass::Org2: return MotionFamily::Person;
	// ctank stays at this catalog's GroundVehicle granularity; the WORLD
	// classifier routes it to VehicleFamily::Tank for the mover/solve split
	// (the tank mover @0x488AB0 + the wheeled solve @0x475DE0). This value
	// is catalog metadata — the world-side family owns dispatch.
	case PhysicsClass::Cveh:
	case PhysicsClass::Ctrn:
	case PhysicsClass::Ctank:
	case PhysicsClass::Catv: return MotionFamily::GroundVehicle;
	case PhysicsClass::Cbike: return MotionFamily::LightVehicle;
	case PhysicsClass::Cbot: return MotionFamily::Watercraft;
	case PhysicsClass::Chel:
	case PhysicsClass::Cpln: return MotionFamily::Aircraft;
	case PhysicsClass::Rokt:
	case PhysicsClass::Stng:
	case PhysicsClass::Hlfr:
	case PhysicsClass::Jvln:
	case PhysicsClass::Arty:
	case PhysicsClass::Arti: return MotionFamily::Guided;
	default: return MotionFamily::Unresolved;
	}
}

ItemReplicationProfile profile_from(const ItemReplicationDefinition &definition) {
	ItemReplicationProfile profile;
	profile.definition_id = definition.definition_id;
	const int64_t wire_id = static_cast<int64_t>(definition.definition_id) -
			ItemReplicationCatalog::kDefinitionIdOffset;
	if (wire_id >= 0 && wire_id <= UINT16_MAX)
		profile.wire_type_id = static_cast<uint16_t>(wire_id);

	profile.callbacks.ai_function = definition.ai_function;
	profile.callbacks.move_function = definition.move_function;
	profile.callbacks.render_function = definition.render_function;
	profile.callbacks.disk_function = definition.disk_function;
	// Network dispatch resolves ai_function first and uses move_function only
	// when it is absent. [orig: admission @0x50E6D3 / dispatch @0x50F2E2]
	profile.callbacks.effective_network_callback = !definition.ai_function.empty()
			? definition.ai_function
			: definition.move_function;

	// Physical movement dispatch is a separate ItemDef callback lookup.
	// [orig: EntityDef_LookupPhysicsCallback @0x4A9240]
	profile.motion_family = motion_family_from_move_function(definition.move_function);
	profile.allocation.item_type = definition.item_type;
	profile.allocation.attrib = definition.attrib;
	profile.allocation.attrib2 = definition.attrib2;
	profile.allocation.ai_data = (definition.attrib & DEF_ITEM_ATTRIB_AIDATA) != 0;
	profile.allocation.player_control =
			(definition.attrib & DEF_ITEM_ATTRIB_PLAYERCONTROL) != 0;
	profile.allocation.emplaced_weapon =
			(definition.attrib & DEF_ITEM_ATTRIB_EWEAP) != 0;
	profile.allocation.spawn_point =
			(definition.attrib & DEF_ITEM_ATTRIB_SPAWNPOINT) != 0;
	profile.allocation.vehicle_physics = definition.physics != 0;

	if (profile.allocation.ai_data)
		profile.capabilities = profile.capabilities | ItemReplicationCapability::AiData;
	if (profile.allocation.player_control)
		profile.capabilities = profile.capabilities | ItemReplicationCapability::PlayerControl;
	if (profile.allocation.emplaced_weapon)
		profile.capabilities = profile.capabilities | ItemReplicationCapability::EmplacedWeapon;
	if ((definition.attrib & DEF_ITEM_ATTRIB_MISSILE) != 0)
		profile.capabilities = profile.capabilities | ItemReplicationCapability::Missile;
	if (profile.allocation.spawn_point)
		profile.capabilities = profile.capabilities | ItemReplicationCapability::SpawnPoint;
	if (profile.allocation.vehicle_physics)
		profile.capabilities = profile.capabilities | ItemReplicationCapability::VehiclePhysics;

	if (profile.callbacks.effective_network_callback.empty()) {
		profile.wire_path = WireReplicationPath::None;
		profile.compact_codec = WireCompactCodec::None;
		return profile;
	}

	switch (class_from_tag(profile.callbacks.effective_network_callback.c_str())) {
	case EntityClass::Player:
		profile.wire_path = WireReplicationPath::Compact;
		profile.compact_codec = WireCompactCodec::PlayerPerson;
		break;
	case EntityClass::Infantry:
		profile.wire_path = WireReplicationPath::Compact;
		profile.compact_codec = WireCompactCodec::OrganicPerson;
		break;
	case EntityClass::Vehicle:
		profile.wire_path = WireReplicationPath::Compact;
		profile.compact_codec = WireCompactCodec::Vehicle;
		break;
	case EntityClass::Guided:
		profile.wire_path = WireReplicationPath::Guided;
		profile.compact_codec = WireCompactCodec::None;
		break;
	case EntityClass::NoNetworkCallback:
		profile.wire_path = WireReplicationPath::None;
		profile.compact_codec = WireCompactCodec::None;
		break;
	case EntityClass::Unknown:
		profile.wire_path = WireReplicationPath::Unresolved;
		profile.compact_codec = WireCompactCodec::Unresolved;
		break;
	}

	if (profile.wire_path == WireReplicationPath::Compact)
		profile.capabilities =
				profile.capabilities | ItemReplicationCapability::CompactReplication;
	else if (profile.wire_path == WireReplicationPath::Guided)
		profile.capabilities =
				profile.capabilities | ItemReplicationCapability::GuidedReplication;
	return profile;
}

} // namespace

EntityClass ItemReplicationProfile::wire_entity_class() const noexcept {
	if (wire_path == WireReplicationPath::Guided) return EntityClass::Guided;
	if (wire_path == WireReplicationPath::None) return EntityClass::NoNetworkCallback;
	if (wire_path != WireReplicationPath::Compact) return EntityClass::Unknown;
	switch (compact_codec) {
	case WireCompactCodec::PlayerPerson: return EntityClass::Player;
	case WireCompactCodec::OrganicPerson: return EntityClass::Infantry;
	case WireCompactCodec::Vehicle: return EntityClass::Vehicle;
	case WireCompactCodec::None:
	case WireCompactCodec::Unresolved: return EntityClass::Unknown;
	}
	return EntityClass::Unknown;
}

ItemReplicationCatalog ItemReplicationCatalog::from_definitions(
		const std::vector<ItemReplicationDefinition> &definitions) {
	ItemReplicationCatalog catalog;
	catalog.profiles_.reserve(definitions.size());
	for (const ItemReplicationDefinition &definition : definitions)
		catalog.profiles_.push_back(profile_from(definition));

	// A repeated definition or wire id resolves to its FIRST definition, the row
	// every retail type-id lookup returns: the host's serialize callback and the
	// client's record width both come from that row, so a later duplicate never
	// selects a width. The repeats stay listed in issues() for tooling.
	// [orig: ItemList_FindIndexByTypeId @0x49E100 returns the first match; the
	//  client's entity creation resolves the wire type through it and takes
	//  +0x1C/+0x20 and the row's callbacks from that index,
	//  NapiNPClientMsg_0x00D @0x4332DA..0x43331A]
	for (size_t i = 0; i < catalog.profiles_.size(); ++i) {
		const ItemReplicationProfile &profile = catalog.profiles_[i];
		if (!catalog.definition_index_.emplace(profile.definition_id, i).second)
			catalog.issues_.push_back({ItemCatalogIssueCode::DuplicateDefinitionId,
					profile.definition_id, profile.wire_type_id});
		if (!profile.wire_type_id) continue;
		if (catalog.wire_index_.emplace(*profile.wire_type_id, i).second)
			catalog.wire_class_resolutions_.emplace(
					*profile.wire_type_id, profile.wire_entity_class());
		else
			catalog.issues_.push_back({ItemCatalogIssueCode::DuplicateWireTypeId,
					profile.definition_id, profile.wire_type_id});
	}
	return catalog;
}

ItemReplicationCatalog ItemReplicationCatalog::from_items_def(const opennova::def::DefItemsFile &items) {
	std::vector<ItemReplicationDefinition> definitions;
	definitions.reserve(items.count);
	for (size_t i = 0; i < items.count; ++i) {
		const DefItemDef &item = items.entries[i];
		ItemReplicationDefinition definition;
		definition.definition_id = item.id;
		definition.item_type = item.type;
		definition.attrib = item.attrib;
		definition.attrib2 = item.attrib2;
		definition.physics = item.physics;
		definition.ai_function = bounded_string(item.ai_function, sizeof(item.ai_function));
		definition.move_function = bounded_string(item.move_function, sizeof(item.move_function));
		definition.render_function =
				bounded_string(item.render_function, sizeof(item.render_function));
		definition.disk_function =
				bounded_string(item.disk_function, sizeof(item.disk_function));
		definitions.push_back(std::move(definition));
	}
	return from_definitions(definitions);
}

const ItemReplicationProfile *ItemReplicationCatalog::by_definition_id(
		int32_t definition_id) const noexcept {
	const auto it = definition_index_.find(definition_id);
	return it == definition_index_.end() ? nullptr : &profiles_[it->second];
}

const ItemReplicationProfile *ItemReplicationCatalog::by_wire_type(
		uint16_t wire_type_id) const noexcept {
	const auto it = wire_index_.find(wire_type_id);
	return it == wire_index_.end() ? nullptr : &profiles_[it->second];
}

std::optional<EntityClass> ItemReplicationCatalog::resolve_wire_entity_class(
		uint16_t wire_type_id) const noexcept {
	const auto it = wire_class_resolutions_.find(wire_type_id);
	if (it == wire_class_resolutions_.end()) return std::nullopt;
	return it->second;
}

} // namespace opennova::replication
