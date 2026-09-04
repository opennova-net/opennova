#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <net/npwire/entity_class.h>

namespace opennova::def {
struct DefItemsFile;  // formats/def/def.h
}

namespace opennova::replication {

// A tag-1 S2C 0x0A record uses exactly one of these fixed-width bodies. Guided
// entities use a separate variable-length path and therefore have None here.
// Unresolved is intentionally distinct: it cannot be sized and must fail closed.
enum class WireCompactCodec : uint8_t {
	Unresolved = 0,
	None,
	PlayerPerson,
	OrganicPerson,
	Vehicle,
};

enum class WireReplicationPath : uint8_t {
	Unresolved = 0,
	None,
	Compact,
	Guided,
};

// Physical movement is independent of controller and wire codec. For example,
// a vehicle can select its compact through ai_function and its mover through a
// different move_function.
enum class MotionFamily : uint8_t {
	Unresolved = 0,
	Static,
	Person,
	GroundVehicle,
	LightVehicle,
	Watercraft,
	Aircraft,
	Guided,
};

// Retail pool membership comes from the still-unwitnessed Pool_Alloc caller
// (D-NET-97). Allocation inputs are retained without inventing a pool from BMS
// EntityKind or ItemDef.type.
enum class StoragePool : uint8_t {
	Unresolved = 0,
	Organic,
	Movable,
	Static,
	Marker,
};

enum class ItemReplicationCapability : uint32_t {
	None = 0,
	AiData = 1u << 0,
	PlayerControl = 1u << 1,
	EmplacedWeapon = 1u << 2,
	Missile = 1u << 3,
	SpawnPoint = 1u << 4,
	VehiclePhysics = 1u << 5,
	CompactReplication = 1u << 6,
	GuidedReplication = 1u << 7,
};

constexpr ItemReplicationCapability operator|(
		ItemReplicationCapability lhs, ItemReplicationCapability rhs) noexcept {
	return static_cast<ItemReplicationCapability>(
			static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
}

constexpr bool has_capability(ItemReplicationCapability set,
		ItemReplicationCapability capability) noexcept {
	return (static_cast<uint32_t>(set) & static_cast<uint32_t>(capability)) != 0;
}

// Definition-shaped input keeps catalog construction usable by parsers, tools,
// and embedders without creating another mutable resolver table.
struct ItemReplicationDefinition {
	int32_t definition_id = 0; // items.def id; wire id is definition_id - 100000
	int32_t item_type = 0;     // raw ItemDef+0x5C value
	uint32_t attrib = 0;       // raw ItemDef+0x54
	uint32_t attrib2 = 0;      // raw ItemDef+0x58
	int32_t physics = 0;       // ItemDef physics selector
	std::string ai_function;
	std::string move_function;
	std::string render_function;
	std::string disk_function;
};

struct ItemAllocationTraits {
	int32_t item_type = 0;
	uint32_t attrib = 0;
	uint32_t attrib2 = 0;
	bool ai_data = false;
	bool player_control = false;
	bool emplaced_weapon = false;
	bool spawn_point = false;
	bool vehicle_physics = false;
	StoragePool retail_pool = StoragePool::Unresolved;
};

struct ItemReplicationCallbacks {
	std::string ai_function;
	std::string move_function;
	std::string render_function;
	std::string disk_function;
	std::string effective_network_callback;
};

struct ItemReplicationProfile {
	int32_t definition_id = 0;
	std::optional<uint16_t> wire_type_id;
	WireReplicationPath wire_path = WireReplicationPath::Unresolved;
	WireCompactCodec compact_codec = WireCompactCodec::Unresolved;
	MotionFamily motion_family = MotionFamily::Unresolved;
	ItemAllocationTraits allocation;
	ItemReplicationCallbacks callbacks;
	ItemReplicationCapability capabilities = ItemReplicationCapability::None;

	// Adapter for the existing npwire record-dispatch API. EntityClass remains
	// at this boundary; None and Unresolved stay distinguishable in the profile.
	EntityClass wire_entity_class() const noexcept;
};

enum class ItemCatalogIssueCode : uint8_t {
	DuplicateDefinitionId,
	DuplicateWireTypeId,
};

struct ItemCatalogIssue {
	ItemCatalogIssueCode code = ItemCatalogIssueCode::DuplicateDefinitionId;
	int32_t definition_id = 0;
	std::optional<uint16_t> wire_type_id;
};

// Immutable, fail-closed lookup built once from loaded items.def. Definitions
// outside the 16-bit wire range remain queryable by full id. Duplicate keys are
// removed from their index so ambiguity cannot silently select a record width.
class ItemReplicationCatalog {
public:
	static constexpr int32_t kDefinitionIdOffset = 100000;

	static ItemReplicationCatalog from_definitions(
			const std::vector<ItemReplicationDefinition> &definitions);
	static ItemReplicationCatalog from_items_def(const opennova::def::DefItemsFile &items);

	const ItemReplicationProfile *by_definition_id(int32_t definition_id) const noexcept;
	const ItemReplicationProfile *by_wire_type(uint16_t wire_type_id) const noexcept;

	WireCompactCodec compact_codec_for(uint16_t wire_type_id) const noexcept;
	// Presence-aware record-width resolution. A present Unknown is a known but
	// unresolved/ambiguous items.def entry and is therefore terminal: callers
	// must fail closed instead of consulting pool or heuristic fallbacks. nullopt
	// means this catalog has no definition for the wire id.
	std::optional<EntityClass> resolve_wire_entity_class(
			uint16_t wire_type_id) const noexcept;
	EntityClass wire_entity_class_for(uint16_t wire_type_id) const noexcept;

	const std::vector<ItemReplicationProfile> &profiles() const noexcept { return profiles_; }
	const std::vector<ItemCatalogIssue> &issues() const noexcept { return issues_; }
	bool valid() const noexcept { return issues_.empty(); }

private:
	std::vector<ItemReplicationProfile> profiles_;
	std::unordered_map<int32_t, size_t> definition_index_;
	std::unordered_map<uint16_t, size_t> wire_index_;
	std::unordered_map<uint16_t, EntityClass> wire_class_resolutions_;
	std::vector<ItemCatalogIssue> issues_;
};

} // namespace opennova::replication
