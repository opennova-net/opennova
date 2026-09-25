#include <runtime/renderer/particle_frame.h>

// [orig: CParticleEmitter_BuildBillboardQuads @ 0x5e6d60;
// CParticleEmitter_ComputeViewDepths @ 0x5e7580;
// CParticleManager_RecursiveSortAndRender @ 0x5ec980]

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace opennova::renderer {
namespace {

bool water_subset_selects(const ParticleEmitterSnapshot &emitter,
		const ParticleViewInput &view) {
	if (view.water_subset == ParticleWaterSubset::All)
		return true;
	// A class-7 child (its def's first graphic is Distort) draws on the
	// distortion pass (flag 4) and on no other; every other child skips it
	// [orig: CParticleGroup_RenderChildren @ 0x5E58D2 -> the `& 4` arm
	// @ 0x5E58DB..0x5E58EE; the flag-4 pass is EffectWorld_DrawParticles(4),
	// the tail @ 0x5F72F7..0x5F72FF of the misnamed CNapiSession_SetViewMatrix,
	// called only from the FrameFX distortion row @ 0x5838F8].
	if (view.water_subset == ParticleWaterSubset::Distortion)
		return emitter.distortion_class;
	if (emitter.distortion_class)
		return false;
	// CParticleGroup_RenderChildren tests the emitter origin at +28 against
	// the active split plane: render flag 1 accepts y < plane, while flag 2
	// accepts y >= plane [orig: the `& 1` arm @ 0x5e5903, compare
	// @ 0x5e5934; the `& 2` arm @ 0x5e5940, compare @ 0x5e5971]. The plane is
	// read through the manager's split pointer, 0.0 while it is null
	// [orig: the null guards @ 0x5e5925 / @ 0x5e5962 over the fldz]; a word
	// with no flag draws every non-class-7 child [orig: @ 0x5e58f3, the draw
	// @ 0x5e58ff] and one with neither water bit draws none [orig: @ 0x5e5943].
	// The manager runs the two scene passes as two walks of the live effect
	// list [orig: CParticleManager_BeginFrame @ 0x5ecfc0, the calls @ 0x5ed034
	// and @ 0x5ed07c]. A NaN follows the hardware compare into the
	// above-inclusive subset because the strict-below test is false.
	const bool below = emitter.position.y < view.water_height;
	return view.water_subset == ParticleWaterSubset::Below ? below : !below;
}

ParticleVec3 add(const ParticleVec3 &a, const ParticleVec3 &b) {
	return {a.x + b.x, a.y + b.y, a.z + b.z};
}

ParticleVec3 subtract(const ParticleVec3 &a, const ParticleVec3 &b) {
	return {a.x - b.x, a.y - b.y, a.z - b.z};
}

ParticleVec3 multiply(const ParticleVec3 &v, float scalar) {
	return {v.x * scalar, v.y * scalar, v.z * scalar};
}

float dot(const ParticleVec3 &a, const ParticleVec3 &b) {
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

float view_depth(const ParticleVec3 &point, const ParticleViewInput &view) {
	return dot(subtract(point, view.position), view.forward);
}

ParticleVec3 aabb_center(const ParticleAabb &bounds) {
	return {
		(bounds.min.x + bounds.max.x) * 0.5f,
		(bounds.min.y + bounds.max.y) * 0.5f,
		(bounds.min.z + bounds.max.z) * 0.5f,
	};
}

void include(ParticleAabb &bounds, const ParticleVec3 &point) {
	if (!bounds.valid) {
		bounds.min = point;
		bounds.max = point;
		bounds.valid = true;
		return;
	}
	bounds.min.x = std::min(bounds.min.x, point.x);
	bounds.min.y = std::min(bounds.min.y, point.y);
	bounds.min.z = std::min(bounds.min.z, point.z);
	bounds.max.x = std::max(bounds.max.x, point.x);
	bounds.max.y = std::max(bounds.max.y, point.y);
	bounds.max.z = std::max(bounds.max.z, point.z);
}

ParticleVec3 rendered_center(const ParticleQuadSnapshot &particle,
		const ParticleViewInput &view) {
	// Positive pull is camera-ward; forward points away from the camera.
	return subtract(particle.center, multiply(view.forward, particle.camera_pull));
}

float component(const ParticleVec3 &value, int axis) {
	return axis == 0 ? value.x : (axis == 1 ? value.y : value.z);
}

float aabb_sphere_radius(const ParticleAabb &bounds) {
	const float x = bounds.max.x - bounds.min.x;
	const float y = bounds.max.y - bounds.min.y;
	const float z = bounds.max.z - bounds.min.z;
	return 0.5f * std::sqrt(x * x + y * y + z * z);
}

bool project_sort_point(const ParticleVec3 &point,
		const ParticleViewInput &view, ParticleVec3 &projected) {
	if (!view.projection_valid)
		return false;
	const ParticleVec3 relative = subtract(point, view.position);
	const float camera[4] = {
		dot(relative, view.right),
		dot(relative, view.up),
		-view_depth(point, view),
		1.0f,
	};
	float clip[4]{};
	for (int row = 0; row < 4; ++row) {
		for (int column = 0; column < 4; ++column)
			clip[row] += view.projection[column * 4 + row] * camera[column];
	}
	if (!std::isfinite(clip[3]) || clip[3] == 0.0f)
		return false;
	const float inverse_w = 1.0f / clip[3];
	projected = {
		clip[0] * inverse_w,
		-clip[1] * inverse_w,
		clip[2] * inverse_w,
	};
	if (!view.projection_near_is_one)
		projected.z = 1.0f - projected.z;
	return std::isfinite(projected.x) && std::isfinite(projected.y) &&
			std::isfinite(projected.z);
}

struct ParticleSortBounds {
	ParticleVec3 center{};
	ParticleVec3 extent{};
};

ParticleSortBounds emitter_sort_bounds(const ParticleEmitterSnapshot &emitter,
		const ParticleViewInput &view) {
	const ParticleVec3 center = emitter.bounds.valid ?
			aabb_center(emitter.bounds) : emitter.position;
	const float radius = emitter.bounds.valid ?
			aabb_sphere_radius(emitter.bounds) : 0.0f;
	ParticleVec3 projected_center;
	ParticleVec3 right_min;
	ParticleVec3 right_max;
	ParticleVec3 depth_min;
	ParticleVec3 depth_max;
	if (project_sort_point(center, view, projected_center) &&
			project_sort_point(subtract(center, multiply(view.right, radius)),
					view, right_min) &&
			project_sort_point(add(center, multiply(view.right, radius)),
					view, right_max) &&
			project_sort_point(subtract(center, multiply(view.forward, radius)),
					view, depth_min) &&
			project_sort_point(add(center, multiply(view.forward, radius)),
					view, depth_max)) {
		// Literal manager transform: the projected right-diameter supplies both
		// screen axes; the projected forward-diameter supplies depth extent
		// [orig: CParticleManager_TransformToViewSpace @ 0x5ecd10..0x5eceac].
		const float screen_extent =
				std::fabs(right_max.x - right_min.x) * 0.5f;
		return {projected_center,
				{screen_extent, screen_extent,
						std::fabs(depth_max.z - depth_min.z) * 0.5f}};
	}

	// A missing embedding projection is a headless/tool fallback. It preserves
	// the same near=1/far=0 orientation and sphere intervals in camera axes;
	// every live Camera3D path supplies the projection above.
	const ParticleVec3 relative = subtract(center, view.position);
	return {{dot(relative, view.right), -dot(relative, view.up),
			-view_depth(center, view)}, {radius, radius, radius}};
}

int retail_float_compare(float a, float b) {
	// Exact comparator branch shape used for both emitter minima and particle
	// depths. In particular, unordered x87 compares fall through to -1.
	if (static_cast<double>(b) < static_cast<double>(a))
		return 1;
	if (static_cast<double>(b) <= static_cast<double>(a))
		return 0;
	return -1;
}

using SortRange = std::pair<int, int>;

// Pending ranges are pairwise disjoint, disjoint from the working range, and
// hold at least two elements each, so a sort of n elements never stacks more
// than n / 2 of them. Callers reserve this bound once per compile.
std::size_t sort_stack_bound(std::size_t element_count) {
	return element_count / 2u + 1u;
}

template <typename T, typename Compare>
void retail_quick_sort(std::vector<T> &values, int low, int high,
		const Compare &compare, std::vector<SortRange> &pending) {
	// Utility_QuickSortWithAux @ 0x53d470 shuttles the first pivot between the
	// two scans. Reproducing it (instead of std::sort) preserves retail's equal-
	// key swaps and therefore its emitter/particle tie order.
	//
	// The original's left-partition recursion is carried on an explicit stack
	// here. The two partitions a pass produces -- [saved_low, pivot-1] and
	// [pivot+1, saved_high] -- are disjoint and neither reads the other, so
	// deferring the left one leaves the final ordering identical while keeping
	// a degenerate input (this is a first-element pivot with no median-of-3,
	// so an already-ordered range recurses once per element) off the C++ call
	// stack. Comparison count is unchanged: still the original's worst case.
	// The stack is caller-retained scratch: it is empty on entry and on exit.
	pending.clear();
	for (;;) {
		while (low < high) {
			const int saved_low = low;
			const int saved_high = high;
			int pivot = low;
			bool partition_done = false;
			while (high > low && !partition_done) {
				while (compare(values[low], values[high]) < 0) {
					--high;
					if (high <= low) {
						partition_done = true;
						break;
					}
				}
				if (partition_done || low >= high)
					break;
				std::swap(values[high], values[low]);
				++low;
				pivot = high;
				if (high <= low)
					break;
				while (compare(values[high], values[low]) > 0) {
					++low;
					if (high <= low) {
						partition_done = true;
						break;
					}
				}
				if (partition_done || low >= high)
					break;
				std::swap(values[high], values[low]);
				--high;
				pivot = low;
			}
			if (saved_low < pivot - 1)
				pending.emplace_back(saved_low, pivot - 1);
			low = pivot + 1;
			high = saved_high;
			if (saved_high <= low)
				break;
		}
		if (pending.empty())
			break;
		low = pending.back().first;
		high = pending.back().second;
		pending.pop_back();
	}
}

bool same_state(const ParticleDrawCommand &command,
		const ParticleDrawState &state,
		ParticleRenderDomain domain) {
	return command.domain == domain &&
			command.pass == state.pass &&
			command.pipeline == state.pipeline &&
			command.atlas_page == state.atlas.page &&
			command.atlas_type == state.atlas.type &&
			command.variant == state.variant;
}

void build_quad(const ParticleQuadSnapshot &particle,
		const ParticleViewInput &view,
		ParticleVertex (&vertices)[4]) {
	const ParticleVec3 center = rendered_center(particle, view);
	const float local_x[4] = {
		-particle.half_width, particle.half_width,
		-particle.half_width, particle.half_width,
	};
	const float local_y[4] = {
		particle.half_height, particle.half_height,
		-particle.half_height, -particle.half_height,
	};

	ParticleVec3 positions[4];
	if (particle.alignment == ParticleAlignment::WorldOriented) {
		// RotationYawPitchRoll: roll around Z, then pitch X, then yaw Y.
		const float cr = std::cos(particle.roll);
		const float sr = std::sin(particle.roll);
		const float cp = std::cos(particle.pitch);
		const float sp = std::sin(particle.pitch);
		const float cy = std::cos(particle.yaw);
		const float sy = std::sin(particle.yaw);
		for (std::size_t i = 0; i < 4; ++i) {
			const float roll_x = local_x[i] * cr - local_y[i] * sr;
			const float roll_y = local_x[i] * sr + local_y[i] * cr;
			const float pitch_y = roll_y * cp;
			const float pitch_z = roll_y * sp;
			const ParticleVec3 offset{
				roll_x * cy + pitch_z * sy,
				pitch_y,
				-roll_x * sy + pitch_z * cy,
			};
			positions[i] = add(center, offset);
		}
	} else {
		const float c = std::cos(particle.roll);
		const float s = std::sin(particle.roll);
		for (std::size_t i = 0; i < 4; ++i) {
			const float rotated_x = local_x[i] * c - local_y[i] * s;
			const float rotated_y = local_x[i] * s + local_y[i] * c;
			positions[i] = add(center,
					add(multiply(view.right, rotated_x),
							multiply(view.up, rotated_y)));
		}
	}

	const ParticleAtlasRegion &atlas = particle.state.atlas;
	const float u0 = atlas.rect.u_min + atlas.inset_u;
	const float v0 = atlas.rect.v_min + atlas.inset_v;
	const float u1 = atlas.rect.u_max - atlas.inset_u;
	const float v1 = atlas.rect.v_max - atlas.inset_v;
	const float u[4] = {u0, u1, u0, u1};
	// Retail's first corner is (-half, -half) with (u_min, v_min), and
	// its positive-Y corners use v_max. Our strip stores the top row first,
	// so V runs oppositely to the vertex row order. Reversing this displays
	// asymmetric dirt splashes upside down, with their dense base at the top.
	// [orig: BuildBillboardQuads @0x5e7213 / @0x5e732f..0x5e7374;
	//  RenderTopAlignedBillboards @0x5f5c99 / @0x5f5ddc..0x5f5e21]
	const float v[4] = {v1, v1, v0, v0};

	for (std::size_t i = 0; i < 4; ++i) {
		vertices[i].x = positions[i].x;
		vertices[i].y = positions[i].y;
		vertices[i].z = positions[i].z;
		vertices[i].primary_color = particle.primary_color;
		vertices[i].secondary_color = particle.secondary_color;
		vertices[i].u = u[i];
		vertices[i].v = v[i];
	}
}

} // namespace

ParticleWaterSubset particle_water_subset_for_side(bool camera_above_water,
		bool camera_side) {
	// Terrain_RenderSceneWithReflection swaps EffectWorld's mode-1/mode-2
	// submissions when the render eye crosses Env_WaterHeightFixed
	// [orig: @ 0x5c93a0 -> EffectWorld_RenderParticlePass @ 0x5f7240].
	const bool select_above = camera_above_water == camera_side;
	return select_above ? ParticleWaterSubset::Above :
			ParticleWaterSubset::Below;
}

ParticleThermalMaterial particle_thermal_material(ParticlePipeline pipeline, bool thermal) {
	if (!thermal)
		return ParticleThermalMaterial::Primary;
	switch (pipeline) {
		case ParticlePipeline::Blend:
			return ParticleThermalMaterial::InvertedBlend;
		case ParticlePipeline::Additive:
		case ParticlePipeline::Premult:
			return ParticleThermalMaterial::DarkeningModulate;
		default:
			return ParticleThermalMaterial::Primary;
	}
}

class ParticleFrameCompiler::Impl {
public:
	static constexpr std::size_t kNoBoundsEntry =
			std::numeric_limits<std::size_t>::max();

	struct EmitterSortEntry {
		std::size_t index = 0;
		// The emitter's clamped run inside snapshot.particles.
		std::size_t first_particle = 0;
		std::size_t particle_count = 0;
		ParticleSortBounds sort{};
		// Retail's per-render-object "already rendered" stamp; see render_batch.
		bool rendered = false;
		// One draw-list bounds entry per emitter, even if RenderBatch fills the
		// emitter again after an empty batch left it unstamped.
		std::size_t bounds_index = kNoBoundsEntry;
	};

	struct ParticleDepthIndex {
		// Flat index into snapshot.particles.
		std::size_t particle_index = 0;
		std::size_t bounds_index = 0;
		float depth = 0.0f;
	};

	template <typename T>
	void reserve(std::vector<T> &values, std::size_t required,
			ParticleFrameDebugCounters &debug) {
		if (required <= values.capacity())
			return;
		values.reserve(required);
		++debug.capacity_growths_this_compile;
		++lifetime_capacity_growths;
	}

	ParticleDrawList draw_list;
	std::vector<EmitterSortEntry> emitter_order;
	std::vector<ParticleDepthIndex> particle_order;
	std::vector<SortRange> sort_stack;
	std::uint64_t compile_count = 0;
	std::uint64_t lifetime_capacity_growths = 0;
};

ParticleFrameCompiler::ParticleFrameCompiler() : impl_(new Impl) {}
ParticleFrameCompiler::~ParticleFrameCompiler() = default;
ParticleFrameCompiler::ParticleFrameCompiler(ParticleFrameCompiler &&) noexcept = default;
ParticleFrameCompiler &ParticleFrameCompiler::operator=(
		ParticleFrameCompiler &&) noexcept = default;

const ParticleDrawList &ParticleFrameCompiler::draw_list() const {
	return impl_->draw_list;
}

const ParticleDrawList &ParticleFrameCompiler::compile(
		const ParticleFrameSnapshot &snapshot,
		const ParticleViewInput &view) {
	Impl &impl = *impl_;
	ParticleDrawList &draw_list = impl.draw_list;
	draw_list.frame_id = snapshot.frame_id;
	draw_list.domain = view.domain;
	draw_list.vertices.clear();
	draw_list.commands.clear();
	draw_list.emitter_bounds.clear();
	draw_list.debug = {};
	ParticleFrameDebugCounters &debug = draw_list.debug;
	debug.compile_index = ++impl.compile_count;
	debug.input_emitters = snapshot.emitters.size();

	impl.emitter_order.clear();
	impl.reserve(impl.emitter_order, snapshot.emitters.size(), debug);

	std::size_t selected_particles = 0;
	for (std::size_t emitter_index = 0;
			emitter_index < snapshot.emitters.size(); ++emitter_index) {
		const ParticleEmitterSnapshot &emitter = snapshot.emitters[emitter_index];
		const std::size_t first_particle = std::min(emitter.first_particle,
				snapshot.particles.size());
		const std::size_t particle_count = std::min(emitter.particle_count,
				snapshot.particles.size() - first_particle);
		debug.input_particles += particle_count;
		if (emitter.domain != view.domain) {
			debug.domain_filtered_particles += particle_count;
			continue;
		}
		// A group the section gate hides draws none of its children on any
		// pass [orig: CParticleGroup_RenderChildren @ 0x5E5893..0x5E5897].
		if (!emitter.group_visible) {
			++debug.section_hidden_emitters;
			continue;
		}
		if (!water_subset_selects(emitter, view)) {
			++debug.water_filtered_emitters;
			debug.water_filtered_particles += particle_count;
			continue;
		}

		++debug.selected_emitters;
		selected_particles += particle_count;
		Impl::EmitterSortEntry entry;
		entry.index = emitter_index;
		entry.first_particle = first_particle;
		entry.particle_count = particle_count;
		entry.sort = emitter_sort_bounds(emitter, view);
		impl.emitter_order.push_back(entry);
	}

	const std::size_t max_quads = std::min(
			static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()),
			std::numeric_limits<std::size_t>::max() / 4u);
	const std::size_t retained_quads = std::min(selected_particles, max_quads);
	impl.reserve(draw_list.vertices, retained_quads * 4u, debug);
	impl.reserve(draw_list.commands, retained_quads, debug);
	impl.reserve(draw_list.emitter_bounds, impl.emitter_order.size(), debug);
	impl.particle_order.clear();
	impl.reserve(impl.particle_order, retained_quads, debug);
	impl.reserve(impl.sort_stack, sort_stack_bound(
			std::max(retained_quads, impl.emitter_order.size())), debug);

	auto render_batch = [&](int begin, int count) {
		if (count <= 0)
			return;
		++debug.render_batch_leaves;
		impl.particle_order.clear();
		// RenderBatch fills the shared depth buffer from every emitter of the
		// batch whose render-object stamp is still clear [orig:
		// CParticleManager_RenderBatch @ 0x5e98d8 -> ComputeViewDepths @ 0x5e7580].
		std::size_t filled_particles = 0;
		for (int entry_offset = 0; entry_offset < count; ++entry_offset) {
			Impl::EmitterSortEntry &emitter_entry =
					impl.emitter_order[static_cast<std::size_t>(begin + entry_offset)];
			if (emitter_entry.rendered)
				continue;
			if (emitter_entry.bounds_index == Impl::kNoBoundsEntry) {
				emitter_entry.bounds_index = draw_list.emitter_bounds.size();
				ParticleEmitterDrawBounds output_bounds;
				output_bounds.emitter_id =
						snapshot.emitters[emitter_entry.index].emitter_id;
				draw_list.emitter_bounds.push_back(output_bounds);
			}
			filled_particles += emitter_entry.particle_count;
			const std::size_t run_end =
					emitter_entry.first_particle + emitter_entry.particle_count;
			for (std::size_t particle_index = emitter_entry.first_particle;
					particle_index < run_end; ++particle_index) {
				const ParticleQuadSnapshot &particle =
						snapshot.particles[particle_index];
				if (!particle.visible) {
					++debug.invisible_particles;
					continue;
				}
				// ComputeViewDepths stores the negative camera-forward dot of the
				// particle position before BuildBillboardQuads applies z_offset.
				impl.particle_order.push_back({particle_index,
						emitter_entry.bounds_index,
						-view_depth(particle.center, view)});
			}
		}
		// The stamp is batch-wide and conditional: only a batch that filled at
		// least one vertex marks every one of its render objects rendered, so
		// the emitters of an empty batch stay eligible for a later, wider batch
		// [orig: CParticleManager_RenderBatch @ 0x5e9a78..0x5e9a90].
		if (filled_particles != 0) {
			for (int entry_offset = 0; entry_offset < count; ++entry_offset) {
				impl.emitter_order[static_cast<std::size_t>(begin + entry_offset)]
						.rendered = true;
			}
		}

		if (impl.particle_order.size() > 1) {
			retail_quick_sort(impl.particle_order, 0,
					static_cast<int>(impl.particle_order.size()) - 1,
					[](const Impl::ParticleDepthIndex &a,
							const Impl::ParticleDepthIndex &b) {
						return retail_float_compare(a.depth, b.depth);
					}, impl.sort_stack);
		}
		for (const Impl::ParticleDepthIndex &particle_entry :
				impl.particle_order) {
			if (debug.emitted_quads == max_quads)
				break;
			const ParticleQuadSnapshot &particle =
					snapshot.particles[particle_entry.particle_index];
			ParticleEmitterDrawBounds &output_bounds =
					draw_list.emitter_bounds[particle_entry.bounds_index];
			if (output_bounds.quad_count == 0) {
				output_bounds.first_quad =
						static_cast<std::uint32_t>(debug.emitted_quads);
			}
			ParticleVertex quad[4];
			build_quad(particle, view, quad);
			for (const ParticleVertex &vertex : quad) {
				draw_list.vertices.push_back(vertex);
				include(output_bounds.bounds, {vertex.x, vertex.y, vertex.z});
			}

			const std::uint32_t quad_index =
					static_cast<std::uint32_t>(debug.emitted_quads);
			if (!draw_list.commands.empty() &&
					same_state(draw_list.commands.back(), particle.state,
							view.domain)) {
				++draw_list.commands.back().quad_count;
				++debug.adjacent_state_merges;
			} else {
				ParticleDrawCommand command;
				command.domain = view.domain;
				command.pass = particle.state.pass;
				command.pipeline = particle.state.pipeline;
				command.atlas_page = particle.state.atlas.page;
				command.atlas_type = particle.state.atlas.type;
				command.variant = particle.state.variant;
				command.first_quad = quad_index;
				command.quad_count = 1;
				draw_list.commands.push_back(command);
			}
			++debug.emitted_quads;
			++output_bounds.quad_count;
		}
	};

	// [orig: CParticleManager_RecursiveSortAndRender @0x5ec980 — the Z->X->Y axis cycle
	//  ((2*mask) % 7), inclusive interval split, Utility_QuickSortWithAux @0x53d470 pivot]
	auto recursive_sort_and_render = [&](auto &&self, int begin, int count,
			std::uint32_t axis_mask, std::uint32_t tried_axes) -> void {
		if (count <= 0)
			return;
		++debug.recursive_partition_calls;
		const int axis = static_cast<int>(axis_mask >> 1u);
		const std::uint32_t next_axis_mask = (2u * axis_mask) % 7u;
		if (count > 1) {
			retail_quick_sort(impl.emitter_order, begin, begin + count - 1,
					[axis](const Impl::EmitterSortEntry &a,
							const Impl::EmitterSortEntry &b) {
						const float a_min = component(a.sort.center, axis) -
								component(a.sort.extent, axis);
						const float b_min = component(b.sort.center, axis) -
								component(b.sort.extent, axis);
						return retail_float_compare(a_min, b_min);
					}, impl.sort_stack);
		}
		const int end = begin + count;
		if (count == 1) {
			render_batch(begin, 1);
			return;
		}

		bool has_overlap = false;
		int group_start = begin;
		int rendered_singletons = 0;
		float max_extent = component(impl.emitter_order[begin].sort.center, axis) +
				component(impl.emitter_order[begin].sort.extent, axis);
		for (int current = begin + 1; current < end; ++current) {
			const Impl::EmitterSortEntry &entry = impl.emitter_order[current];
			const float current_min = component(entry.sort.center, axis) -
					component(entry.sort.extent, axis);
			const float current_max = component(entry.sort.center, axis) +
					component(entry.sort.extent, axis);
			if (current_min >= max_extent) {
				if (has_overlap) {
					self(self, group_start, current - group_start,
							next_axis_mask, axis_mask);
				} else {
					render_batch(current - 1, 1);
					++rendered_singletons;
				}
				max_extent = current_max;
				has_overlap = false;
				group_start = current;
			} else {
				has_overlap = true;
				if (current_max > max_extent)
					max_extent = current_max;
			}
		}

		if (!has_overlap) {
			render_batch(end - 1, 1);
			return;
		}
		if (rendered_singletons != 0) {
			self(self, group_start, end - group_start,
					next_axis_mask, axis_mask);
			return;
		}
		const std::uint32_t accumulated_axes = tried_axes | axis_mask;
		if (accumulated_axes == 7u) {
			render_batch(begin, count);
			return;
		}
		self(self, begin, count, next_axis_mask, accumulated_axes);
	};

	if (!impl.emitter_order.empty()) {
		recursive_sort_and_render(recursive_sort_and_render, 0,
				static_cast<int>(impl.emitter_order.size()), 4u, 0u);
	}

	debug.draw_commands = draw_list.commands.size();
	debug.truncated_particles =
			selected_particles - debug.invisible_particles - debug.emitted_quads;
	debug.lifetime_capacity_growths = impl.lifetime_capacity_growths;
	debug.vertex_capacity = draw_list.vertices.capacity();
	debug.command_capacity = draw_list.commands.capacity();
	debug.emitter_bounds_capacity = draw_list.emitter_bounds.capacity();
	debug.sort_stack_capacity = impl.sort_stack.capacity();
	return draw_list;
}

}  // namespace opennova::renderer