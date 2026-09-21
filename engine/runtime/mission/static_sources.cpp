#include <runtime/mission/static_sources.h>
#include <runtime/mission/placement_traits.h>

#include <algorithm>
#include <utility>

namespace opennova::mission {
namespace {

uint64_t identity_key(int kind, int index) {
	return (uint64_t(uint32_t(kind)) << 32) | uint32_t(index);
}

bool admitted(const StaticTerrainShadowSource &source) {
	return item_casts_static_terrain_shadow(source.entity_kind, source.entity_attrib,
			source.item_attrib, source.item_attrib2);
}

} // namespace

void StaticSources::bump_shadow_revision() {
	if (++shadow_revision_ == 0) ++shadow_revision_;
}

void StaticSources::clear() {
	effects_.clear();
	light_draws_.clear();
	shadows_.clear();
	shadow_rows_.clear();
	shadow_rows_by_bms_.clear();
	instances_.clear();
	instance_rows_.clear();
	hidden_.clear();
	replacements_.clear();
	++light_revision_;
	bump_shadow_revision();
}

void StaticSources::invalidate_shadow_assets() {
	for (auto &source : shadows_) source.asset_id = 0;
	replacements_.clear();
	bump_shadow_revision();
}

int StaticSources::append_effect(StaticEffectSource source) {
	source.source_index = static_cast<int>(effects_.size());
	effects_.push_back(std::move(source));
	return static_cast<int>(effects_.size()) - 1;
}

int StaticSources::append_light_draw(StaticLightDrawSource source) {
	source.atlas_row = static_cast<int>(light_draws_.size());
	light_draws_.push_back(std::move(source));
	++light_revision_;
	return static_cast<int>(light_draws_.size()) - 1;
}

bool StaticSources::record_shadow(StaticTerrainShadowSource source) {
	// The retail collector walks only pool 2 then pool 1. Keep rejected rows
	// from those pools, preserving the exact portable admission inputs.
	// [orig: Terrain_CollectAndRenderTileModels @0x60d250,
	// admission @0x60d421..0x60d450; docs/terrain/terrain-re.md]
	if (source.entity_kind != kEntityKindBuilding && source.entity_kind != kEntityKindItem)
		return false;
	const auto row = shadows_.size();
	shadow_rows_[identity_key(source.entity_kind, source.entity_index)].push_back(row);
	if (source.bms_id != 0) shadow_rows_by_bms_[source.bms_id].push_back(row);
	shadows_.push_back(std::move(source));
	bump_shadow_revision();
	return true;
}

std::vector<StaticLightDrawSource> StaticSources::light_draw_sources() const {
	auto out = light_draws_;
	for (auto &source : out)
		source.active = source.bms_id == 0 || !is_hidden(source.bms_id);
	return out;
}

const StaticInstance *StaticSources::instance(int bms_id) const {
	const auto it = instance_rows_.find(bms_id);
	return it == instance_rows_.end() ? nullptr : &instances_[it->second];
}

StaticInstance *StaticSources::mutable_instance(int bms_id) {
	const auto it = instance_rows_.find(bms_id);
	return it == instance_rows_.end() ? nullptr : &instances_[it->second];
}

void StaticSources::register_instance(StaticInstance value, bool publish_shadow_revision) {
	if (auto *current = mutable_instance(value.bms_id)) {
		*current = std::move(value);
	} else {
		instance_rows_[value.bms_id] = instances_.size();
		instances_.push_back(std::move(value));
	}
	if (publish_shadow_revision) bump_shadow_revision();
}

bool StaticSources::hide_instance(int bms_id) {
	if (!instance(bms_id) || is_hidden(bms_id)) return false;
	hidden_.insert(bms_id);
	++light_revision_;
	bump_shadow_revision();
	return true;
}

bool StaticSources::show_instance(int bms_id) {
	if (!hidden_.erase(bms_id)) return false;
	++light_revision_;
	bump_shadow_revision();
	return true;
}

std::vector<StaticTerrainShadowSource> StaticSources::shadow_sources(
		const ResolveAsset &resolve) const {
	auto out = shadows_;
	const auto apply_replacement = [&](StaticTerrainShadowSource &source) {
		const auto it = replacements_.find(source.bms_id);
		if (it == replacements_.end()) return;
		const auto &replacement = it->second;
		source.graphic = replacement.graphic;
		source.world_transform = replacement.world_transform;
		source.asset_id = replacement.asset_id != 0
				? replacement.asset_id : resolve(replacement.graphic);
		source.active = replacement.active;
	};
	for (auto &source : out) {
		if (source.asset_id == 0 && !source.graphic.empty())
			source.asset_id = resolve(source.graphic);
		if (source.bms_id == 0) continue;
		if (const auto *placed = instance(source.bms_id)) source.world_transform = placed->xform;
		source.active = !is_hidden(source.bms_id);
		apply_replacement(source);
	}
	// Manual registrations merge after placed rows, in registration order.
	// casts=false maps to the authored NoShadow veto, exactly as placement.
	for (const auto &placed : instances_) {
		if (std::any_of(out.begin(), out.end(), [&](const auto &source) {
				return source.bms_id == placed.bms_id;
			})) continue;
		StaticTerrainShadowSource source;
		source.bms_id = placed.bms_id;
		source.entity_kind = kEntityKindBuilding;
		source.entity_index = placed.index;
		source.entity_attrib = placed.casts_static_shadow ? 0u : kEntityAttribNoShadow;
		source.graphic = placed.graphic;
		source.world_transform = placed.xform;
		source.asset_id = resolve(source.graphic);
		source.active = !is_hidden(placed.bms_id);
		apply_replacement(source);
		out.push_back(std::move(source));
	}
	return out;
}

bool StaticSources::update_shadow_transform(int kind, int index,
		const StaticSourceTransform &transform) {
	bool matched = false;
	bool saw_source_match = false;
	bool observable_changed = false;
	const auto rows = shadow_rows_.find(identity_key(kind, index));
	if (rows != shadow_rows_.end()) {
		for (const auto i : rows->second) {
			if (i >= shadows_.size()) continue;
			auto &source = shadows_[i];
			if (source.entity_kind != kind || source.entity_index != index) continue;
			saw_source_match = matched = true;
			bool effective_active = source.active &&
					(source.bms_id == 0 || !is_hidden(source.bms_id));
			auto replacement = replacements_.find(source.bms_id);
			const bool has_replacement = source.bms_id != 0 && replacement != replacements_.end();
			if (has_replacement) effective_active = replacement->second.active;
			bool changed = source.world_transform != transform;
			source.world_transform = transform;
			if (source.bms_id != 0) {
				if (auto *placed = mutable_instance(source.bms_id)) {
					changed |= placed->xform != transform;
					placed->xform = transform;
				}
				if (has_replacement) {
					changed |= replacement->second.world_transform != transform;
					replacement->second.world_transform = transform;
				}
			}
			observable_changed |= admitted(source) && effective_active && changed;
		}
	}
	if (!saw_source_match && kind == kEntityKindBuilding) {
		for (auto &placed : instances_) {
			if (placed.index != index) continue;
			matched = true;
			auto replacement = replacements_.find(placed.bms_id);
			const bool has_replacement = replacement != replacements_.end();
			const bool effective_active = placed.casts_static_shadow &&
					(has_replacement ? replacement->second.active : !is_hidden(placed.bms_id));
			bool changed = placed.xform != transform;
			placed.xform = transform;
			if (has_replacement) {
				changed |= replacement->second.world_transform != transform;
				replacement->second.world_transform = transform;
			}
			observable_changed |= effective_active && changed;
		}
	}
	if (observable_changed) bump_shadow_revision();
	return matched;
}

StaticSources::ShadowPolicy StaticSources::shadow_policy(int bms_id) const {
	ShadowPolicy out;
	const auto rows = shadow_rows_by_bms_.find(bms_id);
	if (rows == shadow_rows_by_bms_.end()) return out;
	for (const auto i : rows->second) {
		if (i >= shadows_.size()) continue;
		const auto &source = shadows_[i];
		if (source.bms_id != bms_id) continue;
		out.represented = true;
		const bool accepts = admitted(source);
		out.admitted |= accepts;
		out.base_active |= accepts && source.active && !is_hidden(bms_id);
	}
	return out;
}

StaticSources::ShadowPolicy StaticSources::effective_policy(int bms_id) const {
	auto policy = shadow_policy(bms_id);
	if (!policy.represented) {
		if (const auto *placed = instance(bms_id)) {
			policy.represented = true;
			policy.admitted = placed->casts_static_shadow;
			policy.base_active = policy.admitted && !is_hidden(bms_id);
		}
	}
	return policy;
}

bool StaticSources::accepts_replacement(int bms_id, const std::string &graphic) const {
	return bms_id != 0 && !graphic.empty() && effective_policy(bms_id).represented;
}

bool StaticSources::set_shadow_replacement(int bms_id, const std::string &graphic,
		const StaticSourceTransform &transform, bool active, uint64_t asset_id) {
	if (!accepts_replacement(bms_id, graphic)) return false;
	const auto policy = effective_policy(bms_id);
	StaticTerrainShadowSource replacement;
	replacement.bms_id = bms_id;
	replacement.graphic = graphic;
	replacement.world_transform = transform;
	replacement.asset_id = asset_id;
	replacement.active = active && asset_id != 0;
	const auto current = replacements_.find(bms_id);
	if (current != replacements_.end()) {
		const auto &value = current->second;
		if (value.graphic == replacement.graphic &&
				value.world_transform == replacement.world_transform &&
				value.asset_id == replacement.asset_id && value.active == replacement.active)
			return asset_id != 0;
	}
	const bool was_effective = policy.admitted &&
			(current != replacements_.end() ? current->second.active : policy.base_active);
	const bool becomes_effective = policy.admitted && replacement.active;
	replacements_[bms_id] = std::move(replacement);
	if (was_effective || becomes_effective) bump_shadow_revision();
	return asset_id != 0;
}

bool StaticSources::clear_shadow_replacement(int bms_id) {
	const auto current = replacements_.find(bms_id);
	if (current == replacements_.end()) return false;
	const auto policy = effective_policy(bms_id);
	const bool was_effective = policy.admitted && current->second.active;
	const bool becomes_effective = policy.admitted && policy.base_active;
	replacements_.erase(current);
	if (was_effective || becomes_effective) bump_shadow_revision();
	return true;
}

} // namespace opennova::mission
