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

r::ParticleEmitterSnapshot emitter(std::uint64_t id,
		r::ParticleRenderDomain domain, float sort_depth,
		std::vector<r::ParticleQuadSnapshot> particles) {
	r::ParticleEmitterSnapshot value;
	value.emitter_id = id;
	value.domain = domain;
	value.bounds.valid = true;
	value.bounds.min = {-1.0f, -1.0f, sort_depth - 1.0f};
	value.bounds.max = {1.0f, 1.0f, sort_depth + 1.0f};
	value.particles = std::move(particles);
	return value;
}

bool domain_sort_and_material_run_contract() {
	const auto shared = state(r::ParticlePipeline::Additive, 3, 1, 7);
	const auto split = state(r::ParticlePipeline::Distort, 4, 7, 8,
			r::ParticleRenderPass::Distortion);
	r::ParticleFrameSnapshot snapshot;
	snapshot.frame_id = 91;
	snapshot.emitters.push_back(emitter(10, r::ParticleRenderDomain::World, 5.0f,
			{quad(4.0f, 0x11u, shared), quad(6.0f, 0x22u, shared),
					quad(5.0f, 0x33u, split)}));
	snapshot.emitters.push_back(emitter(20, r::ParticleRenderDomain::World,
			20.0f, {quad(20.0f, 0x44u, shared)}));
	snapshot.emitters.push_back(emitter(30, r::ParticleRenderDomain::World,
			20.0f, {quad(20.0f, 0x55u, shared)}));
	snapshot.emitters.push_back(emitter(40, r::ParticleRenderDomain::FirstPerson,
			100.0f, {quad(100.0f, 0x66u, shared)}));
	r::ParticleViewInput view;
	r::ParticleFrameCompiler compiler;
	const auto &packet = compiler.compile(snapshot, view);
	if (!check(packet.frame_id == 91 &&
			packet.domain == r::ParticleRenderDomain::World &&
			packet.vertices.size() == 20,
			"world compile emits five retail-stride quads")) return false;
	if (!check(packet.vertices[0].primary_color == 0x44u &&
			packet.vertices[4].primary_color == 0x55u &&
			packet.vertices[8].primary_color == 0x22u &&
			packet.vertices[12].primary_color == 0x33u &&
			packet.vertices[16].primary_color == 0x11u,
			"emitters and particles sort far-to-near with stable ties")) return false;
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
	snapshot.emitters.push_back(emitter(101, r::ParticleRenderDomain::World,
			10.0f, {quad(10.0f, 0xa1u, shared), quad(0.0f, 0xa2u, shared)}));
	snapshot.emitters.push_back(emitter(202, r::ParticleRenderDomain::World,
			9.0f, {quad(9.0f, 0xb1u, shared), quad(8.0f, 0xb2u, shared)}));

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
	snapshot.emitters.push_back(emitter(77, r::ParticleRenderDomain::World,
			100.0f, {visible, hidden}));
	r::ParticleFrameCompiler compiler;
	r::ParticleViewInput view;
	const r::ParticleDrawPacket first = compiler.compile(snapshot, view);
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

} // namespace

int main() {
	if (!color_byte_conversion_contract()) return 1;
	if (!domain_sort_and_material_run_contract()) return 1;
	if (!overlapping_emitters_interleave_by_particle_depth_contract()) return 1;
	if (!geometry_bounds_and_reuse_contract()) return 1;
	return 0;
}
