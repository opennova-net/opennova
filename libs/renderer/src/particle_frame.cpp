#include "renderer/particle_frame.h"

// [orig: CParticleEmitter_BuildBillboardQuads @ 0x5e6d60;
// CParticleEmitter_ComputeViewDepths @ 0x5e7580;
// CParticleManager_RecursiveSortAndRender @ 0x5ec980]

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace renderer {
namespace {

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

// Give malformed NaNs a deterministic final position without violating the
// stable-sort comparator's strict weak ordering.
bool farther_first(float a, float b) {
	const bool a_nan = std::isnan(a);
	const bool b_nan = std::isnan(b);
	if (a_nan || b_nan)
		return !a_nan && b_nan;
	return a > b;
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
	const float v[4] = {v0, v0, v1, v1};

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

class ParticleFrameCompiler::Impl {
public:
	struct DepthIndex {
		std::size_t index = 0;
		float depth = 0.0f;
	};

	struct ParticleDepthIndex {
		std::size_t emitter_index = 0;
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

	ParticleDrawPacket packet;
	std::vector<DepthIndex> emitter_order;
	std::vector<ParticleDepthIndex> particle_order;
	std::uint64_t compile_count = 0;
	std::uint64_t lifetime_capacity_growths = 0;
};

ParticleFrameCompiler::ParticleFrameCompiler() : impl_(new Impl) {}
ParticleFrameCompiler::~ParticleFrameCompiler() = default;
ParticleFrameCompiler::ParticleFrameCompiler(ParticleFrameCompiler &&) noexcept = default;
ParticleFrameCompiler &ParticleFrameCompiler::operator=(
		ParticleFrameCompiler &&) noexcept = default;

const ParticleDrawPacket &ParticleFrameCompiler::compile(
		const ParticleFrameSnapshot &snapshot,
		const ParticleViewInput &view) {
	Impl &impl = *impl_;
	ParticleDrawPacket &packet = impl.packet;
	packet.frame_id = snapshot.frame_id;
	packet.domain = view.domain;
	packet.vertices.clear();
	packet.commands.clear();
	packet.emitter_bounds.clear();
	packet.debug = {};
	ParticleFrameDebugCounters &debug = packet.debug;
	debug.compile_index = ++impl.compile_count;
	debug.input_emitters = snapshot.emitters.size();

	impl.emitter_order.clear();
	impl.reserve(impl.emitter_order, snapshot.emitters.size(), debug);

	std::size_t selected_particles = 0;
	for (std::size_t emitter_index = 0;
			emitter_index < snapshot.emitters.size(); ++emitter_index) {
		const ParticleEmitterSnapshot &emitter = snapshot.emitters[emitter_index];
		debug.input_particles += emitter.particles.size();
		if (emitter.domain != view.domain) {
			debug.domain_filtered_particles += emitter.particles.size();
			continue;
		}

		++debug.selected_emitters;
		selected_particles += emitter.particles.size();

		ParticleAabb sort_bounds = emitter.bounds;
		if (!sort_bounds.valid) {
			// Transitional fallback for producers that do not carry manager
			// bounds yet. Exact emitted-vertex bounds are still reported.
			for (const ParticleQuadSnapshot &particle : emitter.particles) {
				if (particle.visible)
					include(sort_bounds, rendered_center(particle, view));
			}
		}
		const float depth = sort_bounds.valid
				? view_depth(aabb_center(sort_bounds), view)
				: -std::numeric_limits<float>::infinity();
		impl.emitter_order.push_back({emitter_index, depth});
	}

	std::sort(impl.emitter_order.begin(), impl.emitter_order.end(),
			[](const Impl::DepthIndex &a, const Impl::DepthIndex &b) {
				if (farther_first(a.depth, b.depth))
					return true;
				if (farther_first(b.depth, a.depth))
					return false;
				return a.index < b.index;
			});

	const std::size_t max_quads = std::min(
			static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()),
			std::numeric_limits<std::size_t>::max() / 4u);
	const std::size_t retained_quads = std::min(selected_particles, max_quads);
	impl.reserve(packet.vertices, retained_quads * 4u, debug);
	impl.reserve(packet.commands, retained_quads, debug);
	impl.reserve(packet.emitter_bounds, impl.emitter_order.size(), debug);
	impl.particle_order.clear();
	impl.reserve(impl.particle_order, retained_quads, debug);

	for (const Impl::DepthIndex &emitter_entry : impl.emitter_order) {
		const ParticleEmitterSnapshot &emitter =
				snapshot.emitters[emitter_entry.index];
		ParticleEmitterDrawBounds output_bounds;
		output_bounds.emitter_id = emitter.emitter_id;
		const std::size_t bounds_index = packet.emitter_bounds.size();
		packet.emitter_bounds.push_back(output_bounds);

		for (std::size_t particle_index = 0;
				particle_index < emitter.particles.size(); ++particle_index) {
			const ParticleQuadSnapshot &particle = emitter.particles[particle_index];
			if (!particle.visible) {
				++debug.invisible_particles;
				continue;
			}
			impl.particle_order.push_back({
				emitter_entry.index,
				particle_index,
				bounds_index,
				view_depth(rendered_center(particle, view), view),
			});
		}
	}

	std::sort(impl.particle_order.begin(), impl.particle_order.end(),
			[](const Impl::ParticleDepthIndex &a,
					const Impl::ParticleDepthIndex &b) {
				if (farther_first(a.depth, b.depth))
					return true;
				if (farther_first(b.depth, a.depth))
					return false;
				if (a.emitter_index != b.emitter_index)
					return a.emitter_index < b.emitter_index;
				return a.particle_index < b.particle_index;
			});

	for (const Impl::ParticleDepthIndex &particle_entry : impl.particle_order) {
		if (debug.emitted_quads == max_quads)
			break;
		const ParticleEmitterSnapshot &emitter =
				snapshot.emitters[particle_entry.emitter_index];
		const ParticleQuadSnapshot &particle =
				emitter.particles[particle_entry.particle_index];
		ParticleEmitterDrawBounds &output_bounds =
				packet.emitter_bounds[particle_entry.bounds_index];
		if (output_bounds.quad_count == 0) {
			output_bounds.first_quad =
					static_cast<std::uint32_t>(debug.emitted_quads);
		}
		ParticleVertex quad[4];
		build_quad(particle, view, quad);
		for (const ParticleVertex &vertex : quad) {
			packet.vertices.push_back(vertex);
			include(output_bounds.bounds, {vertex.x, vertex.y, vertex.z});
		}

		const std::uint32_t quad_index =
				static_cast<std::uint32_t>(debug.emitted_quads);
		if (!packet.commands.empty() &&
				same_state(packet.commands.back(), particle.state, view.domain)) {
			++packet.commands.back().quad_count;
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
			packet.commands.push_back(command);
		}
		++debug.emitted_quads;
		++output_bounds.quad_count;
	}

	debug.draw_commands = packet.commands.size();
	debug.truncated_particles =
			selected_particles - debug.invisible_particles - debug.emitted_quads;
	debug.lifetime_capacity_growths = impl.lifetime_capacity_growths;
	debug.vertex_capacity = packet.vertices.capacity();
	debug.command_capacity = packet.commands.capacity();
	debug.emitter_bounds_capacity = packet.emitter_bounds.capacity();
	debug.emitter_sort_capacity = impl.emitter_order.capacity();
	debug.particle_sort_capacity = impl.particle_order.capacity();
	return packet;
}

} // namespace renderer
