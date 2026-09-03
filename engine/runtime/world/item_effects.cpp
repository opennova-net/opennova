#include <runtime/world/item_effects.h>

#include <cstdio>

#include <formats/def/def.h>
#include <runtime/world/entity.h>

namespace opennova::world {

namespace {

constexpr uint32_t kPowerupBit = DEF_ITEM_ATTRIB_POWERUP;
constexpr uint32_t kPlayerControlBit = DEF_ITEM_ATTRIB_PLAYERCONTROL;

constexpr int kKindMarker = static_cast<int>(EntityKind::Marker);
constexpr int kKindItem = static_cast<int>(EntityKind::Item);
constexpr int kKindBuilding = static_cast<int>(EntityKind::Building);

// The wire handle domain: 16-bit, 0xffff = none (a negative value = none).
constexpr int32_t kWireHandleNone = 0xffff;

void push_alias(std::vector<std::string> &out, const char *prefix, long long value) {
	char buffer[64];
	std::snprintf(buffer, sizeof(buffer), "%s:%lld", prefix, value);
	out.emplace_back(buffer);
}

} // namespace

// [orig: resolve_item_materials_and_spawn_bone_trails @ 0x522ee0 — pool 1
//  skips attrib 0x42, pools 2/3 skip attrib 0x2, pool 0 is never walked]
bool item_effect_pool_allows(int kind, uint32_t attrib) {
	if (kind == kKindItem) {
		return (attrib & (kPowerupBit | kPlayerControlBit)) == 0;
	}
	if (kind == kKindBuilding || kind == kKindMarker) {
		return (attrib & kPowerupBit) == 0;
	}
	return false;
}

bool item_effect_controller_allows(int kind, uint32_t attrib) {
	// The occupied-controller pass bypasses only PlayerControl. The
	// independent powerup exclusion remains intact.
	return kind == kKindItem &&
			(attrib & (kPowerupBit | kPlayerControlBit)) == kPlayerControlBit;
}

void item_effect_identity_aliases(int32_t net_id, int32_t bms_id,
		int64_t spawn_origin, int32_t wire_handle,
		std::vector<std::string> &out) {
	out.clear();
	const bool has_wire_identity = wire_handle >= 0 && wire_handle != kWireHandleNone;
	if (has_wire_identity) {
		push_alias(out, "wire", wire_handle);
	}
	// Synthetic items.def attachments all carry the same sentinel origin and
	// no authored BMS/net identity. Their packed runtime handle is therefore
	// the only alias that distinguishes siblings on the same carrier.
	if (has_wire_identity && bms_id == 0 &&
			(spawn_origin == -1 ||
					spawn_origin == static_cast<int64_t>(kSpawnOriginNone))) {
		return;
	}
	if (net_id > 0) {
		push_alias(out, "net", net_id);
	}
	if (bms_id > 0) {
		push_alias(out, "bms", bms_id);
	}
	if (spawn_origin > 0) {
		push_alias(out, "origin", spawn_origin);
	}
}

bool item_effect_aliases_intersect(const std::vector<std::string> &left,
		const std::vector<std::string> &right) {
	for (const std::string &alias : left) {
		for (const std::string &other : right) {
			if (alias == other) {
				return true;
			}
		}
	}
	return false;
}

// The first-16 case-insensitive scan is ONE impl in engine/formats/threedi
// [orig: ItemDef_GetBoneMaskByName @ 0x49ea40; duplicate names all set their
// bit]; a name matching nothing (or no authored name) still spawns ONE
// emitter at the entity origin — the spawn_count==0 leg
// [orig: Entity_SpawnBoneTrailEffect @ 0x43c097 -> submit_effect_descriptor
//  @ 0x43c0a4 at entity->Position].
ItemEffectAttachPlan item_effect_attach_plan(const Threedi3di3 &model,
		const char *userpoint_name) {
	ItemEffectAttachPlan plan;
	if (userpoint_name != nullptr && userpoint_name[0] != '\0') {
		plan.mask = threedi_3di3_user_point_mask(&model, userpoint_name);
		for (int i = 0; i < THREEDI_USER_POINT_SCAN_LIMIT; ++i) {
			if ((plan.mask & (1u << i)) != 0) {
				plan.user_points.push_back(i);
			}
		}
	}
	plan.origin_fallback = plan.user_points.empty();
	return plan;
}

} // namespace opennova::world
