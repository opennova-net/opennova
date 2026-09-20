#include <runtime/replication/item_replication_catalog.h>

#include <cstring>
#include <string_view>
#include <unordered_set>
#include <utility>

#include <formats/def/def.h>
#include <base/io/strutil.h>

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

MotionFamily motion_family_from_tag(const std::string &tag) {
	// The retail update-callback table stores its keys as 4-byte fourccs
	// ([tag u32] rows @ 0x82ABC0) while items.def authors longer tokens onto
	// them — the shipped corpus writes `cbike`, `ctank`, `catv` and mixed-case
	// `CHel`, and the record names those exact rows "the cbike row via
	// Entity_DispatchPhysics_cbike @0x48EFF0" / "0x488AB0 is the ctank row's
	// mover" (docs/world/vehicle-client-movers-re.md). Exact whole-string
	// matching therefore strands every 5-char family on Unresolved; resolve
	// on the case-folded fourcc prefix instead.
	const std::string key = strutil::to_lower(
			std::string_view(tag).substr(0, 4));
	if (key.empty() || key == "null") return MotionFamily::Static;
	if (key == "plyr" || key == "org0" || key == "org1" || key == "org2")
		return MotionFamily::Person;
	// ctan stays at this catalog's GroundVehicle granularity; the WORLD
	// classifier routes it to VehicleFamily::Tank for the mover/solve split
	// (the tank mover @0x488AB0 + the wheeled solve @0x475DE0). This value
	// is catalog metadata — the world-side family owns dispatch.
	if (key == "cveh" || key == "ctrn" || key == "ctan" || key == "catv")
		return MotionFamily::GroundVehicle;
	if (key == "cbik") return MotionFamily::LightVehicle;
	if (key == "cbot") return MotionFamily::Watercraft;
	if (key == "chel" || key == "cpln") return MotionFamily::Aircraft;
	if (key == "rokt" || key == "stng" || key == "hlfr" || key == "jvln" ||
	    key == "arty" || key == "arti")
		return MotionFamily::Guided;
	return MotionFamily::Unresolved;
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
	profile.motion_family = motion_family_from_tag(definition.move_function);
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

	std::unordered_set<int32_t> ambiguous_definitions;
	std::unordered_set<uint16_t> ambiguous_wire_types;
	for (size_t i = 0; i < catalog.profiles_.size(); ++i) {
		const ItemReplicationProfile &profile = catalog.profiles_[i];
		if (ambiguous_definitions.count(profile.definition_id) == 0) {
			const auto inserted = catalog.definition_index_.emplace(profile.definition_id, i);
			if (!inserted.second) {
				catalog.definition_index_.erase(profile.definition_id);
				ambiguous_definitions.insert(profile.definition_id);
				catalog.issues_.push_back({ItemCatalogIssueCode::DuplicateDefinitionId,
						profile.definition_id, profile.wire_type_id});
			}
		}
		if (!profile.wire_type_id || ambiguous_wire_types.count(*profile.wire_type_id) != 0)
			continue;
		const auto inserted = catalog.wire_index_.emplace(*profile.wire_type_id, i);
		catalog.wire_class_resolutions_.emplace(
				*profile.wire_type_id, profile.wire_entity_class());
		if (!inserted.second) {
			catalog.wire_index_.erase(*profile.wire_type_id);
			// Keep the key present while making its result terminal/fail-closed.
			catalog.wire_class_resolutions_[*profile.wire_type_id] =
					EntityClass::Unknown;
			ambiguous_wire_types.insert(*profile.wire_type_id);
			catalog.issues_.push_back({ItemCatalogIssueCode::DuplicateWireTypeId,
					profile.definition_id, profile.wire_type_id});
		}
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
