#include <renderer/particle_frame.h>
#include <renderer/particle_color.h>

#include <cmath>
#include <cstdio>
#include <limits>
#include <utility>
#include <vector>

namespace {
namespace r = renderer;

static_assert(sizeof(r::ParticleVertex) == 28,
		"particle upload vertices retain the retail stride");

bool check(bool ok, const char *message) {
	if (!ok) {
		std::fputs(message, stderr);
		std::fputc(10, stderr);
	}
	return ok;
}

bool near(float actual, float expected) {
	return std::fabs(actual - expected) <= 0.00001f;
}

bool color_byte_conversion_contract() {
	using renderer::particle_retail_low_byte;
	using renderer::particle_unit_byte;
	const float nan = std::numeric_limits<float>::quiet_NaN();
	const float inf = std::numeric_limits<float>::infinity();
	const float huge = std::numeric_limits<float>::max();
	if (!check(particle_unit_byte(nan) == 0 &&
			particle_unit_byte(-inf) == 0 &&
			particle_unit_byte(inf) == 255 &&
			particle_unit_byte(-huge) == 0 &&
			particle_unit_byte(huge) == 255,
			"UNORM byte conversion bounds invalid floats deterministically")) {
		return false;
	}
	if (!check(particle_unit_byte(0.0f) == 0 &&
			particle_unit_byte(0.5f) == 127 &&
			particle_unit_byte(1.0f) == 255,
			"UNORM byte conversion retains finite truncation")) return false;
	if (!check(particle_retail_low_byte(nan) == 0 &&
			particle_retail_low_byte(inf) == 0 &&
			particle_retail_low_byte(-inf) == 0 &&
			particle_retail_low_byte(huge) == 0 &&
			particle_retail_low_byte(-huge) == 0,
			"retail low-byte conversion emulates x86 invalid conversion")) {
		return false;
	}
	return check(particle_retail_low_byte(-2.0f) == 129 &&
			particle_retail_low_byte(-1.0f) == 0 &&
			particle_retail_low_byte(1.0f) == 255 &&
			particle_retail_low_byte(3.0f) == 254,
			"retail low-byte conversion truncates and wraps finite values");
}

r::ParticleDrawState state(r::ParticlePipeline pipeline,
		std::uint32_t page, std::uint8_t type, std::uint16_t variant,
		r::ParticleRenderPass pass = r::ParticleRenderPass::Color) {
	r::ParticleDrawState value;
	value.pass = pass;
	value.pipeline = pipeline;
	value.atlas.page = page;
	value.atlas.type = type;
	value.atlas.rect = {0.0f, 0.0f, 1.0f, 1.0f};
	value.variant = variant;
	return value;
}

r::ParticleQuadSnapshot quad(float z, std::uint32_t color,
		const r::ParticleDrawState &draw_state) {
	r::ParticleQuadSnapshot value;
	value.center = {0.0f, 0.0f, z};
	value.half_width = 0.5f;
	value.half_height = 0.5f;
	value.primary_color = color;
	value.state = draw_state;
	return value;
}

// Appends one emitter and its contiguous particle run in producer order. The
// returned index stays valid across later appends (the vector may reallocate).
std::size_t add_emitter(r::ParticleFrameSnapshot &snapshot, std::uint64_t id,
		r::ParticleRenderDomain domain, float sort_depth,
		const std::vector<r::ParticleQuadSnapshot> &particles,
		float emitter_height = 0.0f) {
	r::ParticleEmitterSnapshot value;
	value.emitter_id = id;
	value.domain = domain;
	value.position.y = emitter_height;
	value.bounds.valid = true;
	value.bounds.min = {-1.0f, -1.0f, sort_depth - 1.0f};
	value.bounds.max = {1.0f, 1.0f, sort_depth + 1.0f};
	value.first_particle = snapshot.particles.size();
	value.particle_count = particles.size();
	snapshot.particles.insert(snapshot.particles.end(), particles.begin(),
			particles.end());
	snapshot.emitters.push_back(value);
	return snapshot.emitters.size() - 1;
}

bool water_emitter_partition_contract() {
	const auto draw_state = state(r::ParticlePipeline::Blend, 1, 0, 0);
	r::ParticleFrameSnapshot snapshot;
	add_emitter(snapshot, 1, r::ParticleRenderDomain::World, 30.0f,
			{quad(30.0f, 0x11u, draw_state)}, 9.0f);
	add_emitter(snapshot, 2, r::ParticleRenderDomain::World, 20.0f,
			{quad(20.0f, 0x22u, draw_state)}, 10.0f);
	add_emitter(snapshot, 3, r::ParticleRenderDomain::World, 10.0f,
			{quad(10.0f, 0x33u, draw_state)}, 11.0f);
	// A FirstPerson emitter at the same height proves that domain filtering is
	// accounted independently from the World water selector.
	add_emitter(snapshot, 4, r::ParticleRenderDomain::FirstPerson, 40.0f,
			{quad(40.0f, 0x44u, draw_state)}, 9.0f);

	r::ParticleFrameCompiler compiler;
	r::ParticleViewInput view;
	view.water_height = 10.0f;
	view.water_subset = r::particle_water_subset_for_side(true, false);
	const auto &above_camera_far = compiler.compile(snapshot, view);
	if (!check(above_camera_far.emitter_bounds.size() == 1 &&
			above_camera_far.emitter_bounds[0].emitter_id == 1 &&
			above_camera_far.debug.selected_emitters == 1 &&
			above_camera_far.debug.water_filtered_emitters == 2 &&
			above_camera_far.debug.water_filtered_particles == 2 &&
			above_camera_far.debug.domain_filtered_particles == 1,
			"camera-above far pass selects only strict-below emitter origins")) {
		return false;
	}

	view.water_subset = r::particle_water_subset_for_side(true, true);
	const auto &above_camera_near = compiler.compile(snapshot, view);
	if (!check(above_camera_near.emitter_bounds.size() == 2 &&
			above_camera_near.emitter_bounds[0].emitter_id == 2 &&
			above_camera_near.emitter_bounds[1].emitter_id == 3,
			"camera-above camera-side pass includes equality and above")) {
		return false;
	}

	view.water_subset = r::particle_water_subset_for_side(false, false);
	const auto &below_camera_far = compiler.compile(snapshot, view);
	if (!check(below_camera_far.emitter_bounds.size() == 2 &&
			below_camera_far.emitter_bounds[0].emitter_id == 2 &&
			below_camera_far.emitter_bounds[1].emitter_id == 3,
			"underwater far pass reverses to the above-inclusive subset")) {
		return false;
	}

	view.water_subset = r::particle_water_subset_for_side(false, true);
	const auto &below_camera_near = compiler.compile(snapshot, view);
	if (!check(below_camera_near.emitter_bounds.size() == 1 &&
			below_camera_near.emitter_bounds[0].emitter_id == 1,
			"underwater camera-side pass reverses to strict-below")) {
		return false;
	}

	view.domain = r::ParticleRenderDomain::FirstPerson;
	view.water_subset = r::ParticleWaterSubset::All;
	const auto &first_person = compiler.compile(snapshot, view);
	return check(first_person.emitter_bounds.size() == 1 &&
			first_person.emitter_bounds[0].emitter_id == 4 &&
			first_person.debug.water_filtered_emitters == 0,
			"unsplit FirstPerson compilation ignores the water plane");
}

bool domain_sort_and_material_run_contract() {
	const auto shared = state(r::ParticlePipeline::Additive, 3, 1, 7);
	const auto split = state(r::ParticlePipeline::Distort, 4, 7, 8,
			r::ParticleRenderPass::Distortion);
	r::ParticleFrameSnapshot snapshot;
	snapshot.frame_id = 91;
	add_emitter(snapshot, 10, r::ParticleRenderDomain::World, 5.0f,
			{quad(4.0f, 0x11u, shared), quad(6.0f, 0x22u, shared),
					quad(5.0f, 0x33u, split)});
	add_emitter(snapshot, 20, r::ParticleRenderDomain::World, 20.0f,
			{quad(20.0f, 0x44u, shared)});
	add_emitter(snapshot, 30, r::ParticleRenderDomain::World, 20.0f,
			{quad(20.0f, 0x55u, shared)});
	add_emitter(snapshot, 40, r::ParticleRenderDomain::FirstPerson, 100.0f,
			{quad(100.0f, 0x66u, shared)});
	r::ParticleViewInput view;
	r::ParticleFrameCompiler compiler;
	const auto &packet = compiler.compile(snapshot, view);
	if (!check(packet.frame_id == 91 &&
			packet.domain == r::ParticleRenderDomain::World &&
			packet.vertices.size() == 20,
			"world compile emits five retail-stride quads")) return false;
	if (!check(packet.vertices[0].primary_color == 0x55u &&
			packet.vertices[4].primary_color == 0x44u &&
			packet.vertices[8].primary_color == 0x22u &&
			packet.vertices[12].primary_color == 0x33u &&
			packet.vertices[16].primary_color == 0x11u,
			"retail recursive leaves sort far-to-near with literal quicksort ties")) return false;
	if (!check(packet.emitter_bounds.size() == 3 &&
			packet.emitter_bounds[0].emitter_id == 20 &&
			packet.emitter_bounds[1].emitter_id == 30 &&
			packet.emitter_bounds[2].emitter_id == 10,
			"emitter bounds retain deterministic sorted order")) return false;
	if (!check(packet.commands.size() == 3 &&
			packet.commands[0].first_quad == 0 &&
			packet.commands[0].quad_count == 3 &&
			packet.commands[1].first_quad == 3 &&
			packet.commands[1].quad_count == 1 &&
			packet.commands[2].first_quad == 4 &&
			packet.commands[2].quad_count == 1,
			"only adjacent exact material states merge")) return false;
	if (!check(packet.commands[0].pipeline == r::ParticlePipeline::Additive &&
			packet.commands[0].atlas_page == 3 &&
			packet.commands[0].atlas_type == 1 &&
			packet.commands[0].variant == 7 &&
			packet.commands[1].pass == r::ParticleRenderPass::Distortion &&
			packet.commands[1].pipeline == r::ParticlePipeline::Distort,
			"material runs preserve exact pipeline and atlas identity")) return false;
	if (!check(packet.debug.input_emitters == 4 &&
			packet.debug.selected_emitters == 3 &&
			packet.debug.input_particles == 6 &&
			packet.debug.domain_filtered_particles == 1 &&
			packet.debug.emitted_quads == 5 &&
			packet.debug.draw_commands == 3 &&
			packet.debug.adjacent_state_merges == 2,
			"world diagnostics account for filtering and merges")) return false;
	view.domain = r::ParticleRenderDomain::FirstPerson;
	const auto &first_person = compiler.compile(snapshot, view);
	return check(first_person.emitter_bounds.size() == 1 &&
			first_person.emitter_bounds[0].emitter_id == 40 &&
			first_person.vertices.size() == 4 &&
			first_person.debug.domain_filtered_particles == 5,
			"first-person compile selects only its render domain");
}

bool overlapping_emitters_interleave_by_particle_depth_contract() {
	const auto shared = state(r::ParticlePipeline::Blend, 6, 0, 4);
	r::ParticleFrameSnapshot snapshot;
	// The manager bounds put A before B. Sorting emitter blocks would produce
	// A10,A0,B9,B8, but retail's overlapping batch is globally depth sorted.
	add_emitter(snapshot, 101, r::ParticleRenderDomain::World, 10.0f,
			{quad(10.0f, 0xa1u, shared), quad(0.0f, 0xa2u, shared)});
	add_emitter(snapshot, 202, r::ParticleRenderDomain::World, 9.0f,
			{quad(9.0f, 0xb1u, shared), quad(8.0f, 0xb2u, shared)});

	r::ParticleFrameCompiler compiler;
	r::ParticleViewInput view;
	const auto &packet = compiler.compile(snapshot, view);
	if (!check(packet.vertices.size() == 16 &&
			packet.vertices[0].primary_color == 0xa1u &&
			packet.vertices[4].primary_color == 0xb1u &&
			packet.vertices[8].primary_color == 0xb2u &&
			packet.vertices[12].primary_color == 0xa2u,
			"overlapping emitters interleave far-to-near by particle depth")) {
		return false;
	}
	if (!check(packet.emitter_bounds.size() == 2 &&
			packet.emitter_bounds[0].emitter_id == 101 &&
			packet.emitter_bounds[0].first_quad == 0 &&
			packet.emitter_bounds[0].quad_count == 2 &&
			packet.emitter_bounds[1].emitter_id == 202 &&
			packet.emitter_bounds[1].first_quad == 1 &&
			packet.emitter_bounds[1].quad_count == 2,
			"interleaved emitter bounds report first occurrence and total count")) {
		return false;
	}
	return check(packet.commands.size() == 1 &&
			packet.commands[0].first_quad == 0 &&
			packet.commands[0].quad_count == 4 &&
			packet.debug.adjacent_state_merges == 3,
			"globally sorted adjacent particles still coalesce material state");
}

bool projected_recursive_leaf_partition_contract() {
	const auto shared = state(r::ParticlePipeline::Blend, 9, 0, 2);
	r::ParticleFrameSnapshot snapshot;
	const std::size_t left = add_emitter(snapshot, 301,
			r::ParticleRenderDomain::World, 10.0f,
			{quad(10.0f, 0xa1u, shared), quad(6.0f, 0xa2u, shared)});
	snapshot.emitters[left].bounds.min = {-12.0f, -1.0f, 6.0f};
	snapshot.emitters[left].bounds.max = {-8.0f, 1.0f, 10.0f};
	const std::size_t right = add_emitter(snapshot, 302,
			r::ParticleRenderDomain::World, 9.0f,
			{quad(9.0f, 0xb1u, shared), quad(8.0f, 0xb2u, shared)});
	snapshot.emitters[right].bounds.min = {8.0f, -1.0f, 7.0f};
	snapshot.emitters[right].bounds.max = {12.0f, 1.0f, 11.0f};

	r::ParticleViewInput view;
	// Minimal reverse-Z perspective: clip=(x,y,1,-z), with camera-forward
	// world +Z becoming camera -Z. Positive depth therefore maps to 1/depth.
	view.projection[0] = 1.0f;
	view.projection[5] = 1.0f;
	view.projection[11] = -1.0f;
	view.projection[14] = 1.0f;
	view.projection_valid = true;
	view.projection_near_is_one = true;

	r::ParticleFrameCompiler compiler;
	const auto &packet = compiler.compile(snapshot, view);
	if (!check(packet.vertices.size() == 16 &&
			packet.vertices[0].primary_color == 0xa1u &&
			packet.vertices[4].primary_color == 0xa2u &&
			packet.vertices[8].primary_color == 0xb1u &&
			packet.vertices[12].primary_color == 0xb2u,
			"screen-separated emitters sort particles inside distinct retail leaves")) {
		return false;
	}
	if (!check(packet.debug.recursive_partition_calls == 2 &&
			packet.debug.render_batch_leaves == 2,
			"retail z then x partition emits two non-overlapping leaves")) {
		return false;
	}
	return check(packet.commands.size() == 1 &&
			packet.debug.adjacent_state_merges == 3,
			"adjacent identical leaf output remains one equivalent GPU run");
}

bool geometry_bounds_and_reuse_contract() {
	auto draw_state = state(r::ParticlePipeline::Premult, 8, 2, 11);
	draw_state.atlas.rect = {0.1f, 0.2f, 0.9f, 0.8f};
	draw_state.atlas.inset_u = 0.05f;
	draw_state.atlas.inset_v = 0.1f;
	r::ParticleQuadSnapshot visible;
	visible.center = {10.0f, 20.0f, 30.0f};
	visible.half_width = 2.0f;
	visible.half_height = 1.0f;
	visible.camera_pull = 3.0f;
	visible.primary_color = 0x11223344u;
	visible.secondary_color = 0x55667788u;
	visible.state = draw_state;
	auto hidden = visible;
	hidden.visible = false;
	r::ParticleFrameSnapshot snapshot;
	snapshot.frame_id = 92;
	add_emitter(snapshot, 77, r::ParticleRenderDomain::World, 100.0f,
			{visible, hidden});
	r::ParticleFrameCompiler compiler;
	r::ParticleViewInput view;
	const r::ParticleDrawList first = compiler.compile(snapshot, view);
	if (!check(first.vertices.size() == 4 &&
			sizeof(first.vertices[0]) == 28,
			"one visible quad emits four 28-byte vertices")) return false;
	const auto &top_left = first.vertices[0];
	const auto &bottom_right = first.vertices[3];
	if (!check(near(top_left.x, 8.0f) && near(top_left.y, 21.0f) &&
			near(top_left.z, 27.0f) && near(top_left.u, 0.15f) &&
			near(top_left.v, 0.3f) &&
			near(bottom_right.x, 12.0f) &&
			near(bottom_right.y, 19.0f) &&
			near(bottom_right.z, 27.0f) &&
			near(bottom_right.u, 0.85f) &&
			near(bottom_right.v, 0.7f),
			"camera pull, extents, and inset UVs compile exactly")) return false;
	if (!check(top_left.primary_color == 0x11223344u &&
			top_left.secondary_color == 0x55667788u,
			"raw primary and secondary colors survive packet compilation")) return false;
	if (!check(first.commands.size() == 1 &&
			first.commands[0].atlas_page == 8 &&
			first.commands[0].atlas_type == 2 &&
			first.commands[0].variant == 11 &&
			first.commands[0].pipeline == r::ParticlePipeline::Premult,
			"draw command preserves exact material state")) return false;
	if (!check(first.emitter_bounds.size() == 1 &&
			first.emitter_bounds[0].emitter_id == 77 &&
			first.emitter_bounds[0].first_quad == 0 &&
			first.emitter_bounds[0].quad_count == 1 &&
			first.emitter_bounds[0].bounds.valid &&
			near(first.emitter_bounds[0].bounds.min.x, 8.0f) &&
			near(first.emitter_bounds[0].bounds.min.y, 19.0f) &&
			near(first.emitter_bounds[0].bounds.min.z, 27.0f) &&
			near(first.emitter_bounds[0].bounds.max.x, 12.0f) &&
			near(first.emitter_bounds[0].bounds.max.y, 21.0f) &&
			near(first.emitter_bounds[0].bounds.max.z, 27.0f),
			"packet reports exact rendered bounds, not sort bounds")) return false;
	if (!check(first.debug.input_particles == 2 &&
			first.debug.invisible_particles == 1 &&
			first.debug.emitted_quads == 1 &&
			first.debug.truncated_particles == 0 &&
			first.debug.capacity_growths_this_compile > 0,
			"diagnostics distinguish hidden, emitted, and allocated work")) return false;
	const auto &second = compiler.compile(snapshot, view);
	return check(second.debug.compile_index == 2 &&
			second.debug.capacity_growths_this_compile == 0 &&
			second.debug.lifetime_capacity_growths ==
					first.debug.lifetime_capacity_growths &&
			second.vertices.size() == first.vertices.size() &&
			near(second.vertices[0].x, first.vertices[0].x) &&
			near(second.vertices[0].u, first.vertices[0].u),
			"identical warm compile is deterministic and allocation-free");
}

bool world_oriented_and_rolled_quad_contract() {
	// Billboard roll spins the quad inside the view plane, while the
	// YAWANDPITCH path (def flag 0x100) routes (yaw, pitch, roll) through the
	// retail RotationYawPitchRoll matrix and may leave that plane
	// [orig: CParticleEmitter_RenderStaticBillboards @ 0x5f4e10 selects the
	// world-oriented path; the Euler feed @ 0x5f5068].
	constexpr float kHalfPi = 1.57079632679f;
	const auto draw_state = state(r::ParticlePipeline::Blend, 1, 0, 1);
	r::ParticleFrameCompiler compiler;
	r::ParticleViewInput view;

	r::ParticleQuadSnapshot rolled;
	rolled.center = {0.0f, 0.0f, 5.0f};
	rolled.half_width = 2.0f;
	rolled.half_height = 1.0f;
	rolled.roll = kHalfPi;
	rolled.state = draw_state;
	{
		r::ParticleFrameSnapshot snapshot;
		add_emitter(snapshot, 1, r::ParticleRenderDomain::World, 5.0f, {rolled});
		const auto &packet = compiler.compile(snapshot, view);
		if (!check(packet.vertices.size() == 4, "rolled billboard compiles"))
			return false;
		// A 90-degree roll swaps the width/height extents in the view plane and
		// keeps every corner at the quad's depth.
		if (!check(near(packet.vertices[0].x, -1.0f) &&
				near(packet.vertices[0].y, -2.0f) &&
				near(packet.vertices[3].x, 1.0f) &&
				near(packet.vertices[3].y, 2.0f),
				"billboard roll rotates extents inside the view plane")) return false;
		if (!check(near(packet.vertices[0].z, 5.0f) &&
				near(packet.vertices[1].z, 5.0f) &&
				near(packet.vertices[2].z, 5.0f) &&
				near(packet.vertices[3].z, 5.0f),
				"the billboard path never leaves the view plane")) return false;
	}

	r::ParticleQuadSnapshot tipped = rolled;
	tipped.roll = 0.0f;
	tipped.pitch = kHalfPi;
	tipped.alignment = r::ParticleAlignment::WorldOriented;
	{
		r::ParticleFrameSnapshot snapshot;
		add_emitter(snapshot, 2, r::ParticleRenderDomain::World, 5.0f, {tipped});
		const auto &packet = compiler.compile(snapshot, view);
		if (!check(packet.vertices.size() == 4, "world-oriented quad compiles"))
			return false;
		float z_span = 0.0f;
		for (std::size_t i = 0; i < 4; ++i) {
			const float dz = std::fabs(packet.vertices[i].z -
					packet.vertices[0].z);
			if (dz > z_span) z_span = dz;
		}
		if (!check(z_span > 1.9f,
				"a 90-degree pitch tips the world-oriented quad out of the view plane"))
			return false;
		if (!check(near(packet.vertices[0].y, 0.0f) &&
				near(packet.vertices[3].y, 0.0f),
				"the pitched quad's height axis leaves the view Y axis")) return false;
	}
	return true;
}

bool flat_particle_runs_contract() {
	// Emitters reference contiguous runs of one flat particle array in
	// producer order; a run that overruns the array is clamped, never read
	// past the end, and accounted only for what exists.
	const auto draw_state = state(r::ParticlePipeline::Blend, 2, 0, 3);
	r::ParticleFrameSnapshot snapshot;
	add_emitter(snapshot, 1, r::ParticleRenderDomain::World, 10.0f,
			{quad(10.0f, 0x11u, draw_state), quad(9.0f, 0x12u, draw_state)});
	add_emitter(snapshot, 2, r::ParticleRenderDomain::World, 30.0f,
			{quad(30.0f, 0x21u, draw_state)});
	// Emitter 3 claims ten particles starting inside emitter 2's run; only the
	// one existing particle is compiled. Emitter 4 starts past the end.
	add_emitter(snapshot, 3, r::ParticleRenderDomain::World, 50.0f, {});
	snapshot.emitters.back().first_particle = 2;
	snapshot.emitters.back().particle_count = 10;
	add_emitter(snapshot, 4, r::ParticleRenderDomain::World, 70.0f, {});
	snapshot.emitters.back().first_particle = 99;
	snapshot.emitters.back().particle_count = 5;

	r::ParticleFrameCompiler compiler;
	r::ParticleViewInput view;
	const auto &packet = compiler.compile(snapshot, view);
	if (!check(packet.debug.input_particles == 4 &&
			packet.debug.selected_emitters == 4 &&
			packet.debug.truncated_particles == 0,
			"overrunning runs are clamped to the flat array")) return false;
	if (!check(packet.vertices.size() == 16 &&
			packet.vertices[0].primary_color == 0x21u &&
			packet.vertices[4].primary_color == 0x21u &&
			packet.vertices[8].primary_color == 0x11u &&
			packet.vertices[12].primary_color == 0x12u,
			"the clamped run re-emits the shared particle far-to-near")) return false;
	return check(packet.emitter_bounds.size() == 4,
			"every selected emitter reports exactly one bounds entry");
}

bool retained_sort_stack_contract() {
	// A near-to-far run is the first-element-pivot quicksort's degenerate
	// input (one partition per element). The explicit recursion stack is
	// retained scratch, so the warm compile of the same frame allocates
	// nothing and reproduces the far-to-near result exactly.
	const auto draw_state = state(r::ParticlePipeline::Additive, 1, 1, 0);
	constexpr std::size_t particle_count = 257;
	std::vector<r::ParticleQuadSnapshot> ordered;
	std::vector<r::ParticleQuadSnapshot> shuffled;
	std::uint32_t lcg = 12345u;
	for (std::size_t i = 0; i < particle_count; ++i) {
		const std::uint32_t ordinal = static_cast<std::uint32_t>(i);
		ordered.push_back(quad(1.0f + static_cast<float>(i), 0x100u + ordinal,
				draw_state));
		lcg = lcg * 1664525u + 1013904223u;
		shuffled.push_back(quad(1.0f + static_cast<float>(lcg % 4096u) * 0.25f,
				0x1000u + ordinal, draw_state));
	}
	r::ParticleFrameSnapshot snapshot;
	add_emitter(snapshot, 1, r::ParticleRenderDomain::World, 128.0f, ordered);
	add_emitter(snapshot, 2, r::ParticleRenderDomain::World, 128.0f, shuffled);

	r::ParticleFrameCompiler compiler;
	r::ParticleViewInput view;
	const r::ParticleDrawList first = compiler.compile(snapshot, view);
	if (!check(first.vertices.size() == 2u * particle_count * 4u &&
			first.debug.render_batch_leaves == 1 &&
			first.debug.sort_stack_capacity > 0,
			"both runs share one overlapping leaf and a retained sort stack")) {
		return false;
	}
	for (std::size_t q = 1; q < 2u * particle_count; ++q) {
		if (!check(first.vertices[q * 4u].z <= first.vertices[(q - 1) * 4u].z,
				"the leaf emits far-to-near across both runs")) return false;
	}
	const auto &second = compiler.compile(snapshot, view);
	if (!check(second.debug.capacity_growths_this_compile == 0 &&
			second.debug.lifetime_capacity_growths ==
					first.debug.lifetime_capacity_growths,
			"the warm degenerate sort allocates nothing")) return false;
	for (std::size_t v = 0; v < first.vertices.size(); ++v) {
		if (!check(second.vertices[v].primary_color ==
						first.vertices[v].primary_color &&
				near(second.vertices[v].z, first.vertices[v].z),
				"the retained stack reproduces the first compile exactly")) {
			return false;
		}
	}
	return true;
}

bool empty_batch_leaves_emitters_unstamped_contract() {
	// Z partition: {A,B} overlap and are separated from {C,D}, which overlap
	// each other; every emitter overlaps on X and Y. Retail recurses {A,B}
	// through all three axes into one batch, then, because the trailing group
	// closed the range with no singleton rendered, re-partitions the WHOLE
	// range until the all-axes fallback batches A,B,C,D together. A and B hold
	// no particles, so their first batch filled nothing and left them
	// unstamped; the wider batch fills them again (to nothing) while C and D
	// draw once, far-to-near. The diagnostics still carry one bounds entry per
	// emitter [orig: CParticleManager_RenderBatch @ 0x5e9a78].
	const auto draw_state = state(r::ParticlePipeline::Blend, 0, 0, 0);
	r::ParticleFrameSnapshot snapshot;
	add_emitter(snapshot, 0xa, r::ParticleRenderDomain::World, 20.0f, {});
	add_emitter(snapshot, 0xb, r::ParticleRenderDomain::World, 19.5f, {});
	add_emitter(snapshot, 0xc, r::ParticleRenderDomain::World, 10.0f,
			{quad(10.0f, 0xc1u, draw_state)});
	add_emitter(snapshot, 0xd, r::ParticleRenderDomain::World, 9.5f,
			{quad(9.5f, 0xd1u, draw_state)});

	r::ParticleFrameCompiler compiler;
	r::ParticleViewInput view;
	const auto &packet = compiler.compile(snapshot, view);
	if (!check(packet.debug.recursive_partition_calls == 5 &&
			packet.debug.render_batch_leaves == 2,
			"the empty leaf is followed by the whole-range fallback batch")) {
		return false;
	}
	if (!check(packet.vertices.size() == 8 &&
			packet.vertices[0].primary_color == 0xc1u &&
			packet.vertices[4].primary_color == 0xd1u,
			"the refilled empty emitters do not disturb the far-to-near output")) {
		return false;
	}
	if (!check(packet.emitter_bounds.size() == 4,
			"re-filled emitters keep a single bounds entry")) return false;
	bool seen[4] = {false, false, false, false};
	for (const r::ParticleEmitterDrawBounds &bounds : packet.emitter_bounds) {
		if (bounds.emitter_id < 0xa || bounds.emitter_id > 0xd)
			return check(false, "bounds entries name only the four emitters");
		seen[bounds.emitter_id - 0xa] = true;
	}
	if (!check(seen[0] && seen[1] && seen[2] && seen[3],
			"every emitter appears exactly once in the bounds list")) return false;
	return check(packet.emitter_bounds[0].quad_count == 0 &&
			packet.emitter_bounds[1].quad_count == 0 &&
			packet.emitter_bounds[2].quad_count == 1 &&
			packet.emitter_bounds[3].quad_count == 1,
			"the empty leaf registers its emitters before the fallback batch");
}

} // namespace

int main() {
	if (!color_byte_conversion_contract()) return 1;
	if (!water_emitter_partition_contract()) return 1;
	if (!domain_sort_and_material_run_contract()) return 1;
	if (!overlapping_emitters_interleave_by_particle_depth_contract()) return 1;
	if (!projected_recursive_leaf_partition_contract()) return 1;
	if (!geometry_bounds_and_reuse_contract()) return 1;
	if (!world_oriented_and_rolled_quad_contract()) return 1;
	if (!flat_particle_runs_contract()) return 1;
	if (!retained_sort_stack_contract()) return 1;
	if (!empty_batch_leaves_emitters_unstamped_contract()) return 1;
	return 0;
}
