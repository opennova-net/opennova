#include <renderer/light_scene.h>

#include <renderer/light_scene_internal.h>
#include <renderer/material_eval.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace renderer {

namespace {

using detail::apply_rgb_gen;
using detail::kHandleFlag;
using detail::kHandleIndexMask;

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

uint32_t corona_texture_argb(int x, int y) {
	// [orig: Lighting_InitTextures @ 0x5a973a..0x5a97ff].
	if (x <= 0 || x >= kCoronaTextureSize - 1 || y <= 0 ||
			y >= kCoronaTextureSize - 1) {
		return 0u;  // the border store @ 0x5a97a8
	}
	const double dx = static_cast<double>(std::abs(x - 64)) * (1.0 / 64.0);
	const double dy = static_cast<double>(std::abs(y - 64)) * (1.0 / 64.0);
	const double d = std::sqrt(dx * dx + dy * dy);
	int intensity = static_cast<int>((0.4 - d * 0.45) * 255.0);
	if (intensity < 0) {
		intensity = 0;  // @ 0x5a9793
	}
	return 0xFF000000u | (0x010101u * static_cast<uint32_t>(intensity));
}

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
	++selection_revision_;
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
	const bool hidden = amount < 0.001f;
	if (slot->hidden != hidden) {
		++selection_revision_;
	}
	slot->blend = amount;
	slot->hidden = hidden;
}

void LightScene::set_owner(LightHandle handle, uint64_t owner_entity,
		int32_t owner_section) {
	Slot *slot = slot_for(handle);
	if (slot == nullptr) {
		return;
	}
	if (slot->params.owner_entity == owner_entity &&
			slot->params.owner_section == owner_section) {
		return;
	}
	slot->params.owner_entity = owner_entity;
	slot->params.owner_section = owner_section;
	++selection_revision_;
}

void LightScene::tick() {
	// [orig: EffectWorld_TickInstancesAndLightScale @ 0x5aa170]
	bool selection_changed = false;
	for (Slot &slot : slots_) {
		if (!slot.live) {
			continue;
		}
		const int32_t counter = slot.fade_counter;
		if (counter > 0) {
			slot.fade_counter = counter - 1;
			if (counter == 1) {
				if (slot.params.fade_mode == 5) {
					if (!slot.hidden) {
						slot.hidden = true; // [orig: flags |= 2 @ 0x5aa1c3]
						selection_changed = true;
					}
				} else {
					slot = Slot{}; // [orig: memset(entry, 0, 0xB0) @ 0x5aa1b9]
					selection_changed = true;
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
	if (selection_changed) {
		++selection_revision_;
	}
}

void LightScene::despawn(LightHandle handle) {
	Slot *slot = slot_for(handle);
	if (slot != nullptr) {
		slot->live = false;
		++selection_revision_;
	}
}

void LightScene::set_position(LightHandle handle,
		const std::array<int32_t, 3> &position_fixed) {
	Slot *slot = slot_for(handle);
	if (slot == nullptr) {
		return;
	}
	if (slot->params.position_fixed == position_fixed) {
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
	++selection_revision_;
}

bool LightScene::alive(LightHandle handle) const {
	return slot_for(handle) != nullptr;
}

void LightScene::clear() {
	if (!slots_.empty()) {
		++selection_revision_;
	}
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
		// Group gate [orig: Light_PassesActiveGroups @ 0x5a9120..0x5a916e]: an
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
		apply_rgb_gen(params, flicker, rgb);
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
			kSelectLimit, "LightDrawSelection carries the witnessed 3-cap");
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

size_t LightScene::collect_corona_quads(const LightCoronaFrameInputs &inputs,
		std::vector<LightCoronaQuad> &out) const {
	out.clear();
	// [orig: EffectWorld_RenderLightCoronas @ 0x5aaf40]. Constants decoded
	// from the binary: 1/65536 @ 0x7c3310, 0.5 @ 0x7c3b94, 1/16 @ 0x7c486c,
	// 0.1 @ 0x7c69f4, 0.66 @ 0x7d3e68, the 100-wu cull 0x640000 fixed
	// @ 0x5ab143.
	constexpr double kMaxDistanceFixed = 0x640000;   // 100 wu
	constexpr float kColorScale = 0.0625f;           // 1/16
	constexpr float kStepFactor = 0.1f;
	constexpr float kSegmentShrink = 0.66f;
	constexpr int kSegments = 3;
	// The witnessed device fog policy (the object/terrain shaders' shared
	// implementation): exp(-d * ln64/end) for type 0, linear (end - d) /
	// (end - start) with start = passed (type 1), end/2 (type 2), end/4
	// (type 3) [orig: Render_SetFogState @ 0x58a950;
	// CD3DDevice_SetFogParameters @ 0x677960].
	const auto fog_visibility = [&inputs](float dist) -> float {
		if (!inputs.fog_enabled) {
			return 1.0f;
		}
		const float safe_end = std::max(inputs.fog_end, 1.0f);
		if (inputs.fog_type == 0) {
			constexpr float kLn64 = 4.1588830833596715f;
			return std::clamp(
					std::exp(-std::max(dist, 0.0f) * (kLn64 / safe_end)),
					0.0f, 1.0f);
		}
		float start = inputs.fog_start;
		if (inputs.fog_type == 2) {
			start = safe_end * 0.5f;
		} else if (inputs.fog_type == 3) {
			start = safe_end * 0.25f;
		}
		return std::clamp((safe_end - dist) / std::max(safe_end - start, 1.0f),
				0.0f, 1.0f);
	};
	const std::array<float, 3> camera_world = {
		static_cast<float>(inputs.camera_fixed[0]) / 65536.0f,
		static_cast<float>(inputs.camera_fixed[1]) / 65536.0f,
		static_cast<float>(inputs.camera_fixed[2]) / 65536.0f,
	};
	for (const Slot &slot : slots_) {
		if (!slot.live || slot.hidden || slot.params.disable_corona) {
			continue; // [orig: flag bits 2 / 0x200 skipped @ 0x5ab027]
		}
		// The owned-light visible-section gate: an owned corona draws only
		// when its owner building's section bit is set this frame; an owner
		// absent from the table passes unconditionally (retail: a
		// non-pool-2 owner index falls outside g_BuildingSectionVisMask and
		// the test returns TRUE) [orig: the sectorFilter gate @ 0x5ab027 ->
		// Terrain_IsBuildingSectionBitSet @ 0x5c6960; both live callers
		// pass the filter enabled @ 0x5c96ab / 0x5c85fb].
		if (slot.params.owner_entity != 0) {
			bool passes = true;
			for (size_t i = 0; i < inputs.owner_mask_count; ++i) {
				const LightCoronaOwnerMask &row = inputs.owner_masks[i];
				if (row.owner_entity == slot.params.owner_entity) {
					const uint32_t section = static_cast<uint32_t>(
							slot.params.owner_section) & 31u;
					passes = (row.section_mask & (1u << section)) != 0;
					break;
				}
			}
			if (!passes) {
				continue;
			}
		}
		std::array<int32_t, 3> pos_fixed = slot.params.position_fixed;
		// The impact-flash re-center (retail render flag 0x100): the light
		// spawned radius/2 above the impact; its corona drops back down
		// [orig: @ 0x5ab037..0x5ab05c].
		if (slot.params.corona_lower_half_radius) {
			pos_fixed[2] = clamp_i32(static_cast<int64_t>(pos_fixed[2]) -
					(static_cast<int64_t>(slot.params.radius_fixed) >> 1));
		}
		// The per-frame sub-centimeter jitter: +-512 fixed on x (frames
		// 0/1) or y (frames 2/3) [orig: the frame & 3 switch @ 0x5ab06e].
		switch (inputs.frame_index & 3u) {
			case 0: pos_fixed[0] = clamp_i32(
					static_cast<int64_t>(pos_fixed[0]) + 512); break;
			case 1: pos_fixed[0] = clamp_i32(
					static_cast<int64_t>(pos_fixed[0]) - 512); break;
			case 2: pos_fixed[1] = clamp_i32(
					static_cast<int64_t>(pos_fixed[1]) + 512); break;
			default: pos_fixed[1] = clamp_i32(
					static_cast<int64_t>(pos_fixed[1]) - 512); break;
		}
		// Camera distance cull at 100 wu, in fixed units like retail's
		// float-of-fixed sqrt [orig: @ 0x5ab0b7..0x5ab143].
		double dist_sq = 0.0;
		for (int axis = 0; axis < 3; ++axis) {
			const double delta = static_cast<double>(pos_fixed[axis]) -
					static_cast<double>(inputs.camera_fixed[axis]);
			dist_sq += delta * delta;
		}
		if (std::sqrt(dist_sq) > kMaxDistanceFixed) {
			continue;
		}
		const std::array<float, 3> light_world = {
			static_cast<float>(pos_fixed[0]) / 65536.0f,
			static_cast<float>(pos_fixed[1]) / 65536.0f,
			static_cast<float>(pos_fixed[2]) / 65536.0f,
		};
		const float radius_world =
				static_cast<float>(slot.params.radius_fixed) / 65536.0f;
		const float base_half = radius_world * 0.5f;
		if (base_half <= 0.0f) {
			continue;
		}
		// Color: record rgb (bytes /256 at spawn) x live blend x ambient
		// scale x 1/16, then the RgbGen multiply — the same gen evaluation
		// the point-light select runs [orig: @ 0x5ab149..0x5ab1b8].
		std::array<float, 3> rgb = {
			static_cast<float>(slot.params.rgb[0]) / 256.0f *
					slot.blend * inputs.ambient_scale[0] * kColorScale,
			static_cast<float>(slot.params.rgb[1]) / 256.0f *
					slot.blend * inputs.ambient_scale[1] * kColorScale,
			static_cast<float>(slot.params.rgb[2]) / 256.0f *
					slot.blend * inputs.ambient_scale[2] * kColorScale,
		};
		apply_rgb_gen(slot.params, inputs.flicker, rgb);
		// The toward-camera march: step = 0.1 x radius along
		// normalize(cam - light) [orig: @ 0x5ab28b..0x5ab2c4].
		std::array<float, 3> to_camera = {
			camera_world[0] - light_world[0],
			camera_world[1] - light_world[1],
			camera_world[2] - light_world[2],
		};
		const float to_camera_len = std::sqrt(to_camera[0] * to_camera[0] +
				to_camera[1] * to_camera[1] + to_camera[2] * to_camera[2]);
		if (to_camera_len > 0.0f) {
			const float step = radius_world * kStepFactor / to_camera_len;
			to_camera[0] *= step;
			to_camera[1] *= step;
			to_camera[2] *= step;
		} else {
			to_camera = {0.0f, 0.0f, 0.0f};
		}
		float half = base_half;
		std::array<float, 3> center = light_world;
		for (int segment = 0; segment < kSegments; ++segment) {
			center[0] += to_camera[0];
			center[1] += to_camera[1];
			center[2] += to_camera[2];
			// Per-segment fade: camera-plane depth over the BASE half-size,
			// clamped 0..1; <= 0 skips the quad [orig: the fdivr 1/half
			// @ 0x5ab2cf and the plane compare @ 0x5ab2f8..0x5ab33c].
			const float depth =
					inputs.depth_plane_normal[0] * center[0] +
					inputs.depth_plane_normal[1] * center[1] +
					inputs.depth_plane_normal[2] * center[2] +
					inputs.depth_plane_w;
			const float fade = std::clamp(depth / base_half, 0.0f, 1.0f);
			if (fade > 0.0f) {
				// The fog-to-black fold: the corona pass runs the primary
				// device fog with FOGCOLOR forced black, so additive quads
				// fade OUT with distance [orig: CD3DDevice_SetFogAndBlendMode
				// (dev, 2) @ 0x5aafb6 -> case 2 @ 0x677740]. D3D fogs the
				// vertices; the segment center is the same distance to
				// within a half-size.
				const float seg_dx = center[0] - camera_world[0];
				const float seg_dy = center[1] - camera_world[1];
				const float seg_dz = center[2] - camera_world[2];
				const float fog = fog_visibility(std::sqrt(
						seg_dx * seg_dx + seg_dy * seg_dy + seg_dz * seg_dz));
				const float scale = fade * fog;
				LightCoronaQuad quad;
				quad.center = center;
				quad.half_size = half;
				quad.rgb = {rgb[0] * scale, rgb[1] * scale, rgb[2] * scale};
				out.push_back(quad);
			}
			half *= kSegmentShrink; // [orig: x0.66 @ 0x5ab71f]
		}
	}
	return out.size();
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

ModelLightOwner resolve_model_light_owner(const ModelLightOwnerInputs &inputs) {
	ModelLightOwner owner;
	// An authored attach subobject wins outright, for every spawning entity
	// kind [orig: @ 0x56c89f -> LightInstance_SetOwnerGroup(entity, bone)
	// @ 0x56c8ae].
	if (inputs.attach_bone != 0) {
		owner.entity = inputs.spawning_entity;
		owner.section = static_cast<int32_t>(inputs.attach_bone);
		return owner;
	}
	// A building never ran the query, so its own unattached records stay
	// world lights [orig: the ItemType_Building gate @ 0x56c7ec].
	if (inputs.spawner_is_building || !inputs.blink_hit) {
		return owner; // [orig: the zero-count fall-through @ 0x56c8bd]
	}
	// Inside a blink box: the containing building + that volume's section
	// [orig: @ 0x56c8c9..0x56c8db — only hit slot 0 is read].
	owner.entity = inputs.blink_owner_entity;
	owner.section = inputs.blink_section;
	return owner;
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
