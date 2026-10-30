#include <runtime/renderer/light_draw_frame.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

#include <base/io/fixed.h>
#include <runtime/environment/water_mirror.h>
#include <runtime/renderer/light_runtime.h>
#include <runtime/world/presentation_frame.h>

namespace opennova::renderer {

namespace {

LightSelectionOptions object_options() {
	LightSelectionOptions options;
	options.target = LightSelectionTarget::Objects;
	options.admit_owned_unscoped = false;
	return options;
}

} // namespace

LightFlickerInputs light_flicker_inputs(const env::WeatherOscillator *oscillator,
		uint32_t time_ms) {
	LightFlickerInputs flicker;
	flicker.time_ms = time_ms;
	if (oscillator != nullptr) {
		flicker.amp_ring = oscillator->amp_ring;
		flicker.amp_ring_size = sizeof(oscillator->amp_ring) / sizeof(oscillator->amp_ring[0]);
		flicker.ring_index = oscillator->ring_index;
	}
	return flicker;
}

void LightDrawFrame::clear() {
	draws_.clear();
	targets_.clear();
	selections_.clear();
	subjects_ = 0;
	any_building_ = false;
}

void LightDrawFrame::push(const LightDrawContext &draw, int32_t robj, bool per_robj) {
	draws_.push_back(draw);
	targets_.push_back(LightDrawTarget{subjects_, robj, per_robj});
}

void LightDrawFrame::add(const LightDrawSubject &subject) {
	const LightDrawContext base = entity_light_draw_context(subject.query);
	if (subject.building) {
		any_building_ = true;
		const int32_t fallback_robj = 0;
		const int32_t *robjs = subject.robj_count > 0 ? subject.robjs : &fallback_robj;
		const size_t robj_count = subject.robj_count > 0 ? subject.robj_count : 1;
		for (size_t i = 0; i < robj_count; ++i) {
			LightDrawContext draw = base;
			const SubmitOwnerGroup owner =
					submit_owner_group(subject.owner_entity, false, false, robjs[i]);
			draw.groups.owner_group_entity = owner.entity;
			draw.groups.owner_group_section = owner.section;
			draw.groups.interior_group_entity = subject.owner_entity;
			draw.groups.interior_group_section = 0;
			push(draw, robjs[i], true);
		}
		++subjects_;
		return;
	}
	LightDrawContext draw = base;
	draw.groups.interior_group_entity = subject.interior_entity;
	draw.groups.interior_group_section = subject.interior_section;
	if (!subject.skinned && subject.interior_entity != 0 && subject.interior_section == 0 &&
			subject.robj_count > 0) {
		for (size_t i = 0; i < subject.robj_count; ++i) {
			LightDrawContext part = draw;
			const SubmitOwnerGroup owner = submit_owner_group(subject.owner_entity,
					subject.person_wave, false, subject.robjs[i]);
			part.groups.owner_group_entity = owner.entity;
			part.groups.owner_group_section = owner.section;
			push(part, subject.robjs[i], true);
		}
		++subjects_;
		return;
	}
	const SubmitOwnerGroup owner = submit_owner_group(subject.owner_entity, subject.person_wave,
			subject.skinned, 0);
	draw.groups.owner_group_entity = owner.entity;
	draw.groups.owner_group_section = owner.section;
	push(draw, 0, false);
	++subjects_;
}

void LightDrawFrame::select(const LightScene &scene, const LightFrameInputs &inputs) {
	selections_.assign(draws_.size(), LightDrawSelection{});
	scene.select_for_draws(draws_.data(), draws_.size(), object_options(), inputs.ambient,
			inputs.flicker, kObjectLightD3DFill, selections_.data());
}

// --- the static rows ---------------------------------------------------------

std::array<float, 4> static_light_row_lane(bool building, int32_t robj_index,
		float light_transfer, bool contained, int32_t model_floor_q16,
		int32_t entity_bound_radius_q16) {
	const EntityLightingState lane =
			static_row_entity_lighting(building, robj_index, light_transfer, contained);
	const env::MirrorClipWave clip_wave =
			building ? env::MirrorClipWave::kSectorModel : env::MirrorClipWave::kEntity;
	return {lane.effect_scale, lane.interior_lerp ? 1.0f : 0.0f, lane.interior_daylight,
		env::water_mirror_clip_origin_offset(
				clip_wave, building ? model_floor_q16 : entity_bound_radius_q16)};
}

void StaticLightRows::set_rows(int64_t revision, std::vector<StaticLightRow> rows) {
	rows_ = std::move(rows);
	revision_ = revision;
	cache_valid_ = false;
}

int StaticLightRows::texture_rows() const {
	return rows_.empty() ? 1 : static_cast<int>(rows_.size());
}

void StaticLightRows::write_row(int atlas_row, const LightDrawSelection &selection) {
	float *row = texels_.data() + static_cast<size_t>(atlas_row) * kStaticLightRowTexels * 4;
	row[0] = static_cast<float>(selection.count);
	for (size_t light = 0; light < selection.count; ++light) {
		const SelectedLight &selected = selection.lights[light];
		float *posr = row + (1 + light * 2) * 4;
		float *color = posr + 4;
		world::presentation_from_mission(selected.position.data(), posr);
		posr[3] = selected.attenuation[2];
		color[0] = selected.color[0];
		color[1] = selected.color[1];
		color[2] = selected.color[2];
		color[3] = selected.range;
	}
}

void StaticLightRows::write_lane(int atlas_row) {
	float *texel = texels_.data() +
			(static_cast<size_t>(atlas_row) * kStaticLightRowTexels + kStaticLightRowLaneTexel) * 4;
	const std::array<float, 4> &lane = rows_[static_cast<size_t>(atlas_row)].lane;
	std::memcpy(texel, lane.data(), sizeof(float) * 4);
}

int StaticLightRows::select(const LightScene &scene, const LightFrameInputs &inputs) {
	const LightSelectionOptions options = object_options();
	const size_t row_count = rows_.size();
	const uint64_t scene_revision = scene.selection_revision();
	const bool cacheable = revision_ >= 0;
	const bool rebuild = !cacheable || !cache_valid_ || cached_scene_revision_ != scene_revision;
	std::vector<int> atlas_rows;
	std::vector<LightDrawSelection> selections;
	if (rebuild) {
		// The broadphase and the group gates: one context per active row,
		// each over its entity's cube.
		std::vector<LightDrawContext> draws;
		atlas_rows.reserve(row_count);
		draws.reserve(row_count);
		for (size_t row = 0; row < row_count; ++row) {
			if (!rows_[row].active)
				continue;
			LightDrawContext draw = entity_light_draw_context(rows_[row].query);
			draw.groups = rows_[row].groups;
			atlas_rows.push_back(static_cast<int>(row));
			draws.push_back(draw);
		}
		selections.resize(draws.size());
		scene.select_for_draws(draws.data(), draws.size(), options, inputs.ambient, inputs.flicker,
				kObjectLightD3DFill, selections.data());
		active_draws_ = static_cast<int>(draws.size());
		if (cacheable) {
			cached_.clear();
			for (size_t i = 0; i < selections.size(); ++i) {
				const LightDrawSelection &selection = selections[i];
				if (selection.count == 0)
					continue;
				Cached cached;
				cached.atlas_row = atlas_rows[i];
				cached.groups = draws[i].groups;
				cached.count = selection.count;
				cached.last_count = selection.count;
				for (size_t light = 0; light < selection.count; ++light) {
					cached.handles[light] = selection.lights[light].handle;
					cached.animated = cached.animated ||
							scene.slot_gen_active(selection.lights[light].handle);
				}
				cached_.push_back(cached);
			}
			cache_valid_ = true;
			cached_scene_revision_ = scene_revision;
			cached_color_revision_ = scene.color_revision();
			cached_ambient_ = inputs.ambient;
		}
	} else {
		// Steady frames maintain the resident payload row by row: a cached row
		// re-selects only when it is gen-animated or a global colour input
		// moved (any blend write or fade ramp bumps the pool's colour revision;
		// the gain compares directly). A skipped row's inputs are all unchanged.
		const uint64_t color_revision = scene.color_revision();
		const bool recompute_all =
				cached_color_revision_ != color_revision || cached_ambient_ != inputs.ambient;
		const size_t resident_size =
				static_cast<size_t>(kStaticLightRowTexels) * static_cast<size_t>(texture_rows()) * 4;
		if (resident_ && texels_.size() == resident_size) {
			int lit = 0;
			std::array<float, static_cast<size_t>(kStaticLightRowTexels) * 4> before{};
			LightDrawSelection selection;
			for (Cached &cached : cached_) {
				if (!recompute_all && !cached.animated) {
					if (cached.last_count > 0)
						++lit;
					continue;
				}
				selection.count = scene.select(cached.handles.data(), cached.count, cached.groups,
						options, inputs.ambient, inputs.flicker, kObjectLightD3DFill,
						selection.lights);
				cached.last_count = selection.count;
				if (selection.count > 0)
					++lit;
				float *row = texels_.data() +
						static_cast<size_t>(cached.atlas_row) * kStaticLightRowTexels * 4;
				std::memcpy(before.data(), row, sizeof(before));
				std::memset(row, 0, sizeof(before));
				write_row(cached.atlas_row, selection);
				write_lane(cached.atlas_row);
				if (std::memcmp(before.data(), row, sizeof(before)) != 0)
					dirty_ = true;
			}
			if (recompute_all) {
				cached_color_revision_ = color_revision;
				cached_ambient_ = inputs.ambient;
			}
			lit_draws_ = lit;
			return lit;
		}
		// No resident payload to maintain: re-select the cached rows and
		// write the whole buffer.
		atlas_rows.reserve(cached_.size());
		selections.resize(cached_.size());
		for (size_t i = 0; i < cached_.size(); ++i) {
			Cached &cached = cached_[i];
			atlas_rows.push_back(cached.atlas_row);
			selections[i].count = scene.select(cached.handles.data(), cached.count, cached.groups,
					options, inputs.ambient, inputs.flicker, kObjectLightD3DFill,
					selections[i].lights);
			cached.last_count = selections[i].count;
		}
	}

	// The whole payload: written fresh, kept only when it differs from the
	// resident one (with no flickering or moving light in range the rows
	// resolve to the same bytes every frame).
	std::vector<float> previous;
	previous.swap(texels_);
	texels_.assign(static_cast<size_t>(kStaticLightRowTexels) * static_cast<size_t>(texture_rows()) * 4,
			0.0f);
	int lit = 0;
	for (size_t i = 0; i < selections.size(); ++i) {
		write_row(atlas_rows[i], selections[i]);
		if (selections[i].count > 0)
			++lit;
	}
	// Every active row carries its entity lighting lane, lit or not.
	for (size_t row = 0; row < row_count; ++row) {
		if (rows_[row].active)
			write_lane(static_cast<int>(row));
	}
	if (previous != texels_)
		dirty_ = true;
	resident_ = cacheable;
	lit_draws_ = lit;
	return lit;
}

bool StaticLightRows::take_dirty() {
	const bool dirty = dirty_;
	dirty_ = false;
	return dirty;
}

void StaticLightRows::clear_output() {
	// The zeroed payload no longer mirrors the cached rows, so the next frame
	// rewrites every active row instead of maintaining the zeros in place (a
	// camera-loss frame keeps the rows, and a reload of the same mission
	// rebuilds them at the same revision and count).
	if (!texels_.empty()) {
		std::fill(texels_.begin(), texels_.end(), 0.0f);
		dirty_ = true;
	}
	resident_ = false;
	lit_draws_ = 0;
}

void StaticLightRows::reset() {
	rows_.clear();
	revision_ = -1;
	cached_.clear();
	cache_valid_ = false;
	clear_output();
	active_draws_ = 0;
}

// --- the corona walk's inputs ------------------------------------------------

LightCoronaFrameInputs light_corona_frame_inputs(const std::array<int32_t, 3> &camera_fixed,
		const std::array<float, 3> &forward_mission, const LightFrameInputs &inputs,
		uint32_t frame_index, const LightCoronaFog &fog,
		const std::vector<LightCoronaOwnerMask> &owner_masks) {
	LightCoronaFrameInputs out;
	out.owner_masks = owner_masks.data();
	out.owner_mask_count = owner_masks.size();
	// The fog-to-black fold: the primary fog's parameters, colour forced black
	// by the walk itself (light_scene.h).
	out.fog_enabled = fog.enabled;
	out.fog_type = fog.type;
	out.fog_start = fog.start;
	out.fog_end = fog.end;
	out.camera_fixed = camera_fixed;
	// The camera depth plane in mission space: depth grows in front of the
	// camera, zero at the camera origin (the batch-sort plane retail feeds the
	// fade [orig: EffectWorld_RenderLightCoronas @0x5aaf40 (the plane reads
	// @0x5ab2f8..0x5ab33c)]; the walk also tests the light centre against the
	// viewport near depth on this axis).
	const float length = std::sqrt(forward_mission[0] * forward_mission[0] +
			forward_mission[1] * forward_mission[1] + forward_mission[2] * forward_mission[2]);
	std::array<float, 3> normal = forward_mission;
	if (length > 0.0f) {
		for (float &component : normal)
			component /= length;
	}
	out.depth_plane_normal = normal;
	out.depth_plane_w = -(normal[0] * static_cast<float>(camera_fixed[0]) / io::kFp16One +
			normal[1] * static_cast<float>(camera_fixed[1]) / io::kFp16One +
			normal[2] * static_cast<float>(camera_fixed[2]) / io::kFp16One);
	out.ambient_scale = inputs.ambient;
	out.frame_index = frame_index;
	out.flicker = inputs.flicker;
	return out;
}

} // namespace opennova::renderer
