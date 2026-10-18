#pragma once

// The placer's retained static entity facts. Device resources are stable,
// embedder-owned asset ids; transforms are three basis rows followed by the presentation origin.
// [orig: Terrain_CollectAndRenderTileModels pool scans/admission
// @0x60D421..0x60D450; Render_CollectRenderObjectsForBatch @0x5d8ff7]

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace opennova::mission {

using StaticSourceTransform = std::array<float, 12>;
inline constexpr StaticSourceTransform kStaticSourceIdentity{
	1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};

// An entity's mission-plane position in 16.16 (x east = Godot x, y north = Godot -z): the words the
// static shadow collector's tile test and its ground probe read off the entity [orig:
// Terrain_CollectAndRenderTileModels @ 0x60D50A (X) and 0x60D531 (Y), entity+4 / +8; the ground's
// pushes @ 0x60D8D3..0x60D8DA]. A placement carries the .bms record's own x and y.
using StaticSourceOrigin = std::array<int32_t, 2>;

// The origin of a pose that came without 16.16 words (a Godot transform: a moved model's, a
// script's): its x and -z rounded to nearest (io::float_to_fp16_16_round_sat).
StaticSourceOrigin static_source_origin_of(const StaticSourceTransform &transform);

struct StaticEffectSource {
	int kind = -1;
	int entity_index = -1;
	int bms_id = 0;
	int item_id = 0;
	int source_index = -1;
	int32_t entity_bound_radius_q16 = 0;
	// The graphic's bound-block floor (world::model_bound_floor_q16): a
	// building row's mirror clip extent.
	int32_t model_floor_q16 = 0;
	std::string graphic;
	StaticSourceTransform world_transform = kStaticSourceIdentity;
	uint64_t asset_id = 0;
};

struct StaticLightDrawSource {
	int atlas_row = -1;
	int source_index = -1;
	int kind = -1;
	int entity_index = -1;
	int bms_id = 0;
	int item_id = 0;
	int robj_index = 0;
	std::array<float, 3> bounds_position{};
	std::array<float, 3> bounds_size{};
	bool active = true;
	// The item's ItemDef+0x218 daylight (items.def light_transfer / 100): a
	// building row's interior aux (renderer::static_row_entity_lighting).
	float light_transfer = 0.0f;
};

struct StaticTerrainShadowSource {
	int bms_id = 0;
	int item_id = 0;
	int entity_kind = -1;
	int entity_index = -1;
	int team = 0;
	uint32_t entity_attrib = 0;
	uint32_t item_attrib = 0;
	uint32_t item_attrib2 = 0;
	std::string graphic;
	StaticSourceTransform world_transform = kStaticSourceIdentity;
	StaticSourceOrigin origin_q16{};
	uint64_t asset_id = 0;
	bool active = true;
};

struct StaticInstance {
	int bms_id = 0;
	std::string graphic;
	std::string batch_key;
	int index = -1;
	StaticSourceTransform xform = kStaticSourceIdentity;
	StaticSourceOrigin origin_q16{};
	bool casts_static_shadow = false;
	bool mirror_reflected = false;
	int lod_instance = -1;
};

// One registry for effects, light draws, shadow providers and static carves.
// Snapshots retain placement order; manual instances retain insertion order.
// A transform can change while its rejected/inactive shadow stays unobservable,
// so revision changes belong here alongside the replacement and carve rules.
class StaticSources {
public:
	using ResolveAsset = std::function<uint64_t(const std::string &)>;

	void clear();
	void invalidate_shadow_assets();
	int append_effect(StaticEffectSource source);
	int append_light_draw(StaticLightDrawSource source);
	bool record_shadow(StaticTerrainShadowSource source);
	const std::vector<StaticEffectSource> &effect_sources() const { return effects_; }
	std::vector<StaticLightDrawSource> light_draw_sources() const;
	std::vector<StaticTerrainShadowSource> shadow_sources(const ResolveAsset &resolve) const;
	uint64_t light_revision() const { return light_revision_; }
	uint64_t shadow_revision() const { return shadow_revision_; }
	void bump_shadow_revision();

	// Placement already publishes a shadow row; manual registration must
	// publish a revision itself. Re-registering retains insertion order.
	void register_instance(StaticInstance instance, bool publish_shadow_revision);
	const StaticInstance *instance(int bms_id) const;
	bool is_hidden(int bms_id) const { return hidden_.count(bms_id) != 0; }
	bool hide_instance(int bms_id);
	bool show_instance(int bms_id);
	// A pose moves a row's origin only on an axis whose float moved: the float is the word's image
	// (bms::Entity::get_x), so an axis that stands keeps the word it was placed with. A caller
	// that holds the words (the editor's document) hands them as `origin`, which the rows take.
	bool update_shadow_transform(int kind, int index, const StaticSourceTransform &transform,
			const std::optional<StaticSourceOrigin> &origin = std::nullopt);
	bool accepts_replacement(int bms_id, const std::string &graphic) const;
	// A replacement swaps the graphic an entity draws, not the entity: its row keeps the entity's
	// origin.
	bool set_shadow_replacement(int bms_id, const std::string &graphic,
			const StaticSourceTransform &transform, bool active, uint64_t asset_id);
	bool clear_shadow_replacement(int bms_id);

private:
	struct ShadowPolicy {
		bool represented = false;
		bool admitted = false;
		bool base_active = false;
	};
	ShadowPolicy shadow_policy(int bms_id) const;
	ShadowPolicy effective_policy(int bms_id) const;
	StaticInstance *mutable_instance(int bms_id);
	std::vector<StaticEffectSource> effects_;
	std::vector<StaticLightDrawSource> light_draws_;
	std::vector<StaticTerrainShadowSource> shadows_;
	std::unordered_map<uint64_t, std::vector<std::size_t>> shadow_rows_;
	std::unordered_map<int, std::vector<std::size_t>> shadow_rows_by_bms_;
	std::vector<StaticInstance> instances_;
	std::unordered_map<int, std::size_t> instance_rows_;
	std::unordered_set<int> hidden_;
	std::unordered_map<int, StaticTerrainShadowSource> replacements_;
	uint64_t light_revision_ = 1;
	uint64_t shadow_revision_ = 0;
};

} // namespace opennova::mission
