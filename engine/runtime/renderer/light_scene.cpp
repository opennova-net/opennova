#include <renderer/light_scene.h>

#include <renderer/material_eval.h>

#include <algorithm>
#include <cstdlib>
#include <limits>

namespace renderer {

namespace {

constexpr uint16_t kHandleFlag = 0x8000; // [orig: @ 0x5a8e94]
constexpr uint16_t kHandleIndexMask = 0x7FFF;

uint64_t saturating_add(uint64_t lhs, uint64_t rhs) {
	if (rhs > std::numeric_limits<uint64_t>::max() - lhs) {
		return std::numeric_limits<uint64_t>::max();
	}
	return lhs + rhs;
}

uint64_t axis_distance_term(int64_t delta) {
	// [orig: collect_nearby_zones_by_aabb @ 0x5aa37a —
	// ((d * d + 0x8000) >> 16) per axis, 16.16 squared distance in world^2].
	const uint64_t magnitude = delta < 0
			? static_cast<uint64_t>(-delta)
			: static_cast<uint64_t>(delta);
	constexpr uint64_t kRound = 0x8000;
	if (magnitude != 0 &&
			magnitude >
					(std::numeric_limits<uint64_t>::max() - kRound) / magnitude) {
		return std::numeric_limits<uint64_t>::max() >> 16;
	}
	return (magnitude * magnitude + kRound) >> 16;
}

int32_t clamp_i32(int64_t value) {
	return static_cast<int32_t>(std::clamp<int64_t>(value,
			std::numeric_limits<int32_t>::min(),
			std::numeric_limits<int32_t>::max()));
}

} // namespace

LightHandle LightScene::spawn(const LightSpawnParams &params) {
	// First-free linear scan, then high-water growth, capacity 4096
	// [orig: LightPool_SpawnGlowEffect @ 0x5a8d83..0x5a8dc0].
	size_t slot_index = slots_.size();
	for (size_t i = 0; i < slots_.size(); ++i) {
		if (!slots_[i].live) {
			slot_index = i;
			break;
		}
	}
	if (slot_index == slots_.size()) {
		if (slots_.size() >= kCapacity) {
			return LightHandle{};
		}
		slots_.emplace_back();
	}
	Slot &slot = slots_[slot_index];
	slot = Slot{};
	slot.live = true;
	const uint32_t generation = next_generation_;
	++next_generation_;
	if (next_generation_ == 0) {
		// Zero is reserved for null.
		next_generation_ = 1;
	}
	slot.generation = generation;
	slot.hidden = false;
	slot.params = params;
	// f14 = 1.0 at spawn [orig: @ 0x5a8e1d]; the configurable initial
	// intensity is the presenter's SetBlendAmount-after-spawn fold.
	slot.blend = params.intensity;
	// d16 = d17 = the spawn duration [orig: @ 0x5a8e63/@ 0x5a8e68].
	slot.fade_counter = params.fade_duration;
	slot.fade_initial = params.fade_duration;
	for (int axis = 0; axis < 3; ++axis) {
		// The spawn AABB is position +/- radius [orig: @ 0x5a8df8..0x5a8e2a].
		slot.aabb_min[axis] = clamp_i32(
				static_cast<int64_t>(params.position_fixed[axis]) -
				params.radius_fixed);
		slot.aabb_max[axis] = clamp_i32(
				static_cast<int64_t>(params.position_fixed[axis]) +
				params.radius_fixed);
	}
	return LightHandle{
			static_cast<uint16_t>(slot_index | kHandleFlag), generation};
}

void LightScene::set_fade(LightHandle handle, int32_t mode, int32_t duration) {
	Slot *slot = slot_for(handle);
	if (slot == nullptr) {
		return;
	}
	// [orig: LightInstance_SetFadeModeAndDuration @ 0x5a8f80 — d15 = mode,
	// then d17 and d16 = duration]
	slot->params.fade_mode = mode;
	slot->fade_counter = duration;
	slot->fade_initial = duration;
}

void LightScene::set_blend(LightHandle handle, float amount) {
	Slot *slot = slot_for(handle);
	if (slot == nullptr) {
		return;
	}
	// [orig: CEffectInstance_SetBlendAmount @ 0x5a8ee0 — f14 = amount; < 0.001 sets the
	// hidden bit, >= 0.001 clears it (the mode-5 re-show path)]
	slot->blend = amount;
	slot->hidden = amount < 0.001f;
}

void LightScene::set_owner(LightHandle handle, uint64_t owner_entity,
		int32_t owner_section) {
	Slot *slot = slot_for(handle);
	if (slot == nullptr) {
		return;
	}
	slot->params.owner_entity = owner_entity;
	slot->params.owner_section = owner_section;
}

void LightScene::tick() {
	// [orig: EffectWorld_TickInstancesAndLightScale @ 0x5aa170]
	for (Slot &slot : slots_) {
		if (!slot.live) {
			continue;
		}
		const int32_t counter = slot.fade_counter;
		if (counter > 0) {
			slot.fade_counter = counter - 1;
			if (counter == 1) {
				if (slot.params.fade_mode == 5) {
					slot.hidden = true; // [orig: flags |= 2 @ 0x5aa1c3]
				} else {
					slot = Slot{}; // [orig: memset(entry, 0, 0xB0) @ 0x5aa1b9]
					continue;
				}
			}
		}
		const int32_t mode = slot.params.fade_mode;
		if (mode != 1 && (mode == 2 || mode == 5) && slot.fade_initial != 0) {
			// [orig: f14 = d16 / d17 @ 0x5aa1de]
			slot.blend = static_cast<float>(slot.fade_counter) /
					static_cast<float>(slot.fade_initial);
		}
	}
}

void LightScene::despawn(LightHandle handle) {
	Slot *slot = slot_for(handle);
	if (slot != nullptr) {
		slot->live = false;
	}
}

void LightScene::set_position(LightHandle handle,
		const std::array<int32_t, 3> &position_fixed) {
	Slot *slot = slot_for(handle);
	if (slot == nullptr) {
		return;
	}
	slot->params.position_fixed = position_fixed;
	for (int axis = 0; axis < 3; ++axis) {
		slot->aabb_min[axis] = clamp_i32(
				static_cast<int64_t>(position_fixed[axis]) -
				slot->params.radius_fixed);
		slot->aabb_max[axis] = clamp_i32(
				static_cast<int64_t>(position_fixed[axis]) +
				slot->params.radius_fixed);
	}
}

bool LightScene::alive(LightHandle handle) const {
	return slot_for(handle) != nullptr;
}

void LightScene::clear() {
	slots_.clear();
	report_ = {};
}

const LightScene::Slot *LightScene::slot_for(LightHandle handle) const {
	if (handle.is_null() || (handle.retail_value & kHandleFlag) == 0) {
		return nullptr;
	}
	const size_t index = handle.retail_value & kHandleIndexMask;
	if (index >= slots_.size() || !slots_[index].live ||
			slots_[index].generation != handle.generation) {
		return nullptr;
	}
	return &slots_[index];
}

LightScene::Slot *LightScene::slot_for(LightHandle handle) {
	return const_cast<Slot *>(
			static_cast<const LightScene *>(this)->slot_for(handle));
}

size_t LightScene::query(const std::array<int32_t, 3> &query_min_fixed,
		const std::array<int32_t, 3> &query_max_fixed,
		std::array<LightHandle, kQueryLimit> &out_handles) const {
	return query_impl(
			query_min_fixed, query_max_fixed, false, out_handles);
}

size_t LightScene::query_camera_global(
		const std::array<int32_t, 3> &query_min_fixed,
		const std::array<int32_t, 3> &query_max_fixed,
		std::array<LightHandle, kQueryLimit> &out_handles) const {
	return query_impl(query_min_fixed, query_max_fixed, true, out_handles);
}

size_t LightScene::query_impl(
		const std::array<int32_t, 3> &query_min_fixed,
		const std::array<int32_t, 3> &query_max_fixed,
		bool collect_all_before_cap,
		std::array<LightHandle, kQueryLimit> &out_handles) const {
	// [orig: collect_nearby_zones_by_aabb @ 0x5aa250 — AABB overlap, the
	// per-axis 16.16 distance metric from the query center, nearest-first
	// (retail bubble sort == stable ascending order), at most 64].
	struct Candidate {
		LightHandle handle;
		uint64_t distance;
	};
	std::vector<Candidate> candidates;
	candidates.reserve(collect_all_before_cap
			? slots_.size()
			: std::min(slots_.size(), kQueryLimit));
	std::array<int32_t, 3> center{};
	for (int axis = 0; axis < 3; ++axis) {
		center[axis] = static_cast<int32_t>(
				(static_cast<int64_t>(query_min_fixed[axis]) +
						query_max_fixed[axis]) >> 1);
	}
	for (size_t i = 0; i < slots_.size(); ++i) {
		const Slot &slot = slots_[i];
		// [orig: @ 0x5aa2a4 — the collection skips flag bit 2 (hidden)]
		if (!slot.live || slot.hidden) {
			continue;
		}
		bool overlaps = true;
		for (int axis = 0; axis < 3; ++axis) {
			if (slot.aabb_min[axis] > query_max_fixed[axis] ||
					slot.aabb_max[axis] < query_min_fixed[axis]) {
				overlaps = false;
				break;
			}
		}
		if (!overlaps) {
			continue;
		}
		uint64_t distance = 0;
		for (int axis = 0; axis < 3; ++axis) {
			distance = saturating_add(distance, axis_distance_term(
					static_cast<int64_t>(slot.params.position_fixed[axis]) -
					center[axis]));
		}
		candidates.push_back(Candidate{
				LightHandle{static_cast<uint16_t>(i | kHandleFlag),
						slot.generation},
				distance});
		if (!collect_all_before_cap && candidates.size() >= kQueryLimit) {
			break; // [orig: @ 0x5aa384 — the scan stops at 64 candidates]
		}
	}
	std::stable_sort(candidates.begin(), candidates.end(),
			[](const Candidate &a, const Candidate &b) {
				return a.distance < b.distance;
			});
	const size_t count = std::min(candidates.size(), kQueryLimit);
	for (size_t i = 0; i < count; ++i) {
		out_handles[i] = candidates[i].handle;
	}
	report_.last_query = count;
	return count;
}

size_t LightScene::select(const LightHandle *handles, size_t handle_count,
		const LightActiveGroups &groups,
		const LightSelectionOptions &options,
		const std::array<float, 3> &ambient_scale,
		const LightFlickerInputs &flicker,
		bool d3d_light_path,
		std::array<SelectedLight, kSelectLimit> &out) const {
	size_t selected = 0;
	for (size_t i = 0; i < handle_count && selected < kSelectLimit; ++i) {
		const Slot *slot = slot_for(handles[i]);
		if (slot == nullptr) {
			continue;
		}
		const LightSpawnParams &params = slot->params;
		const bool disabled_for_target =
				options.target == LightSelectionTarget::Objects
				? params.disable_objects
				: params.disable_terrain;
		if (disabled_for_target) {
			continue;
		}
		// Group gate [orig: update_light_slots @ 0x5abc90..0x5abd23]: an
		// owned light passes only for the active interior group (section
		// matched against the interior section, falling back to the owner
		// section) or the active owner group.
		if (params.owner_entity != 0 && !options.admit_owned_unscoped) {
			bool passes = false;
			if (params.owner_entity == groups.interior_group_entity) {
				const int32_t wanted = groups.interior_group_section != 0
						? groups.interior_group_section
						: groups.owner_group_section;
				passes = params.owner_section == wanted;
			} else if (params.owner_entity == groups.owner_group_entity) {
				passes = true;
			}
			if (!passes) {
				continue;
			}
		}
		SelectedLight &light = out[selected];
		for (int axis = 0; axis < 3; ++axis) {
			light.position[axis] =
					static_cast<float>(params.position_fixed[axis]) / 65536.0f;
		}
		light.position_w = params.radius_fixed != 0
				? 65536.0f / static_cast<float>(params.radius_fixed)
				: 0.0f; // [orig: @ 0x5a91d4]
		// Record bytes were stored /256 at spawn [orig: @ 0x5a8e51].
		std::array<float, 3> rgb = {
			static_cast<float>(params.rgb[0]) / 256.0f,
			static_cast<float>(params.rgb[1]) / 256.0f,
			static_cast<float>(params.rgb[2]) / 256.0f,
		};
		if (params.has_gen && params.gen.style != 0) {
			// [orig: Light_GetPointLightParams @ 0x5a9211..0x5a9243 —
			// tick the FLICKER register from the wave ring, then the RGB-gen
			// multiply].
			const int32_t ctrl =
					light_flicker_value(params.position_fixed, flicker);
			const LightRuntime gen = eval_light_runtime(params.gen.style,
					params.gen.phase, params.gen.rate, params.gen.color_start,
					params.gen.color_end, flicker.time_ms, ctrl);
			rgb[0] *= gen.r * gen.intensity;
			rgb[1] *= gen.g * gen.intensity;
			rgb[2] *= gen.b * gen.intensity;
		}
		// The intensity term is the LIVE blend (record f14) — spawn seeds it
		// and the fade tick / SetBlendAmount mutate it [orig: @ 0x5a9207].
		light.color = point_light_color(rgb, slot->blend, ambient_scale,
				d3d_light_path);
		light.attenuation = point_light_attenuation(params.radius_fixed);
		light.range = static_cast<float>(params.radius_fixed) * 1.25f / 65536.0f;
		light.lights_terrain = !params.disable_terrain;
		light.lights_objects = !params.disable_objects;
		light.handle = handles[i];
		++selected;
	}
	report_.last_selected = selected;
	return selected;
}

void LightScene::select_for_draws(const LightDrawContext *draws,
		size_t draw_count,
		const LightSelectionOptions &options,
		const std::array<float, 3> &ambient_scale,
		const LightFlickerInputs &flicker,
		bool d3d_light_path,
		LightDrawSelection *out) const {
	static_assert(std::tuple_size<decltype(LightDrawSelection::lights)>::value ==
			kSelectLimit, "LightDrawSelection carries the witnessed 4-cap");
	if (draws == nullptr || out == nullptr || draw_count == 0) {
		return;
	}
	// One pass snapshots the collection inputs in SLOT ORDER — the order the
	// witnessed first-64 cap depends on [orig: collect_nearby_zones_by_aabb
	// @ 0x5aa250 scans the table forward and breaks at 64 @ 0x5aa384;
	// hidden flag bit 2 skipped @ 0x5aa2a4].
	struct CompactSlot {
		LightHandle handle;
		std::array<int32_t, 3> aabb_min;
		std::array<int32_t, 3> aabb_max;
		std::array<int32_t, 3> position;
	};
	std::vector<CompactSlot> compact;
	compact.reserve(slots_.size());
	for (size_t i = 0; i < slots_.size(); ++i) {
		const Slot &slot = slots_[i];
		if (!slot.live || slot.hidden) {
			continue;
		}
		compact.push_back(CompactSlot{
				LightHandle{static_cast<uint16_t>(i | kHandleFlag),
						slot.generation},
				slot.aabb_min, slot.aabb_max, slot.params.position_fixed});
	}
	struct Candidate {
		LightHandle handle;
		uint64_t distance;
	};
	std::array<Candidate, kQueryLimit> candidates;
	std::array<LightHandle, kQueryLimit> handles;
	for (size_t d = 0; d < draw_count; ++d) {
		const LightDrawContext &draw = draws[d];
		std::array<int32_t, 3> center{};
		for (int axis = 0; axis < 3; ++axis) {
			center[axis] = static_cast<int32_t>(
					(static_cast<int64_t>(draw.aabb_min_fixed[axis]) +
							draw.aabb_max_fixed[axis]) >> 1);
		}
		size_t count = 0;
		for (const CompactSlot &slot : compact) {
			bool overlaps = true;
			for (int axis = 0; axis < 3; ++axis) {
				if (slot.aabb_min[axis] > draw.aabb_max_fixed[axis] ||
						slot.aabb_max[axis] < draw.aabb_min_fixed[axis]) {
					overlaps = false;
					break;
				}
			}
			if (!overlaps) {
				continue;
			}
			uint64_t distance = 0;
			for (int axis = 0; axis < 3; ++axis) {
				distance = saturating_add(distance, axis_distance_term(
						static_cast<int64_t>(slot.position[axis]) -
						center[axis]));
			}
			candidates[count] = Candidate{slot.handle, distance};
			++count;
			if (count >= kQueryLimit) {
				break; // [orig: @ 0x5aa384]
			}
		}
		// Retail bubble sort == stable ascending order [orig: @ 0x5aa3a8].
		std::stable_sort(candidates.begin(),
				candidates.begin() + static_cast<ptrdiff_t>(count),
				[](const Candidate &a, const Candidate &b) {
					return a.distance < b.distance;
				});
		for (size_t i = 0; i < count; ++i) {
			handles[i] = candidates[i].handle;
		}
		out[d].count = select(handles.data(), count, draw.groups, options,
				ambient_scale, flicker, d3d_light_path, out[d].lights);
	}
}

LightSceneReport LightScene::inspect() const {
	LightSceneReport report = report_;
	report.high_water = slots_.size();
	report.live = 0;
	for (const Slot &slot : slots_) {
		if (slot.live) {
			++report.live;
		}
	}
	return report;
}

int32_t light_flicker_value(const std::array<int32_t, 3> &position_fixed,
		const LightFlickerInputs &flicker) {
	// [orig: Light_TickGenBlock @ 0x5a8ae0 — index = (z >> 15) + (y >> 14) +
	// (x >> 14) + Env_WaveRingIndex, byte-wrapped; the ring value is written
	// to the global FLICKER ctrl slot (0x83FD00 = 0x83FCE8 + 8 * 3)].
	if (flicker.amp_ring == nullptr || flicker.amp_ring_size == 0) {
		return 0;
	}
	const uint8_t index = static_cast<uint8_t>(
			(position_fixed[2] >> 15) + (position_fixed[1] >> 14) +
			(position_fixed[0] >> 14) + flicker.ring_index);
	return flicker.amp_ring[index % flicker.amp_ring_size];
}

} // namespace renderer
