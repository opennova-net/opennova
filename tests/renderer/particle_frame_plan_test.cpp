// renderer::plan_particle_frame, the particle frame's ordered pass compiles
// (which views compile which passes: the main scene's two world passes, the
// main-scene-only distortion pass, the first-person domain, and the mirror /
// Inset / Inset-mirror pairs), the distortion row's content gate, the render
// snapshot build, and the pass environment (renderer/particle_frame_plan.h
// carries the witnesses).

#include <runtime/renderer/particle_frame_plan.h>

#include <cstdio>
#include <memory>
#include <vector>

namespace {

namespace r = opennova::renderer;
namespace p = opennova::particle;

int g_failures = 0;

void check(bool ok, const char *message) {
	if (!ok) {
		std::fprintf(stderr, "FAIL: %s\n", message);
		++g_failures;
	}
}

constexpr std::array<float, 9> kIdentityView{1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};

r::ParticleFrameStrides strides() {
	r::ParticleFrameStrides s;
	s.main_view = 1;
	s.mirror = 2;
	s.inset = 3;
	s.inset_mirror = 4;
	return s;
}

void a_main_only_frame_compiles_the_main_scene_and_retires_the_pairs() {
	r::ParticleFrameViews views;
	views.main_above_water = true;
	views.thermal = true;
	views.strides = strides();
	const r::ParticleFramePlan plan = r::plan_particle_frame(views);
	check(plan.compile_count == 4, "main only: four compiles");
	const r::ParticleDrawSlot order[] = {r::ParticleDrawSlot::WorldFarSide,
		r::ParticleDrawSlot::WorldCameraSide, r::ParticleDrawSlot::Distortion,
		r::ParticleDrawSlot::FirstPerson};
	for (std::size_t i = 0; i < 4 && i < plan.compile_count; ++i) {
		check(plan.compiles[i].slot == order[i], "main only: frame order");
		check(plan.compiles[i].view == r::ParticleFrameView::Main, "main only: the main eye");
		check(plan.compiles[i].thermal, "main only: the main scene carries the thermal byte");
		check(plan.compiles[i].lod_divisor == 1, "main only: the main stride");
		check(!plan.compiles[i].relight, "main only: no relight");
	}
	check(plan.compiles[0].water_subset == r::particle_water_subset_for_side(true, false),
			"main far side");
	check(plan.compiles[1].water_subset == r::particle_water_subset_for_side(true, true),
			"main camera side");
	check(plan.compiles[2].water_subset == r::ParticleWaterSubset::Distortion,
			"distortion subset");
	check(plan.compiles[3].domain == r::ParticleRenderDomain::FirstPerson &&
					plan.compiles[3].water_subset == r::ParticleWaterSubset::All,
			"first person: its own domain, no water partition");
	check(plan.retired_count == 6, "main only: three pairs retire");
}

void every_view_compiles_in_frame_order_with_its_own_side_and_stride() {
	r::ParticleFrameViews views;
	views.main_above_water = false;
	views.mirror = true;
	views.mirror_rendered = true;
	views.inset = true;
	views.inset_above_water = true;
	views.inset_mirror = true;
	views.inset_mirror_rendered = true;
	views.thermal = true;
	views.strides = strides();
	const r::ParticleFramePlan plan = r::plan_particle_frame(views);
	check(plan.compile_count == 10, "every view: ten compiles");
	check(plan.retired_count == 0, "every view: nothing retires");
	const r::ParticlePassCompile &mirror_far = plan.compiles[4];
	const r::ParticlePassCompile &mirror_near = plan.compiles[5];
	check(mirror_far.slot == r::ParticleDrawSlot::ReflectionFarSide &&
					mirror_near.slot == r::ParticleDrawSlot::ReflectionCameraSide,
			"the mirror pair follows the main scene's");
	check(mirror_far.relight && !mirror_near.relight, "the pair relights once, first");
	check(!mirror_far.thermal && !mirror_near.thermal, "a secondary scene passes no thermal byte");
	check(mirror_far.lod_divisor == 2, "the mirror's stride");
	check(mirror_far.water_subset == r::particle_water_subset_for_side(false, false),
			"the mirror selects by the main eye's side");
	const r::ParticlePassCompile &inset_far = plan.compiles[6];
	check(inset_far.slot == r::ParticleDrawSlot::SecondSceneFarSide &&
					inset_far.view == r::ParticleFrameView::Inset,
			"the Inset pair follows");
	check(inset_far.water_subset == r::particle_water_subset_for_side(true, false),
			"the Inset selects by its own eye's side");
	check(inset_far.lod_divisor == 3 && inset_far.relight, "the Inset's stride, relit");
	const r::ParticlePassCompile &inset_mirror = plan.compiles[8];
	check(inset_mirror.slot == r::ParticleDrawSlot::InsetMirrorFarSide &&
					inset_mirror.view == r::ParticleFrameView::InsetMirror,
			"the Inset's mirror pair is last");
	check(inset_mirror.water_subset == r::particle_water_subset_for_side(false, false),
			"the Inset's mirror selects by the main eye's side");
	check(inset_mirror.lod_divisor == 4, "the Inset mirror's stride");
}

void a_mirror_the_frame_leaves_unrendered_compiles_nothing() {
	r::ParticleFrameViews views;
	views.mirror = true;
	views.mirror_rendered = false;
	views.inset_mirror = true;
	views.inset_mirror_rendered = false;
	const r::ParticleFramePlan plan = r::plan_particle_frame(views);
	check(plan.compile_count == 4, "unrendered mirrors: main scene only");
	bool reflection_retired = false;
	for (std::size_t i = 0; i < plan.retired_count; ++i)
		reflection_retired |= plan.retired[i] == r::ParticleDrawSlot::ReflectionFarSide;
	check(reflection_retired, "an unrendered mirror's pair retires");
}

void the_distortion_row_counts_emitters_ahead_of_the_submit_tests() {
	r::ParticleFrameDebugCounters counters;
	check(!r::particle_distortion_row_present(counters), "no class-7 child: no row");
	counters.frustum_culled_emitters = 1;
	check(r::particle_distortion_row_present(counters), "a culled child counts");
	counters = {};
	counters.empty_emitters = 1;
	check(r::particle_distortion_row_present(counters), "an empty child counts");
	counters = {};
	counters.selected_emitters = 2;
	check(r::particle_distortion_row_present(counters), "a selected child counts");
}

void the_snapshot_takes_layers_passes_and_atlas_entries() {
	auto definitions = std::make_shared<std::vector<p::ParticleDef>>(1);
	p::ParticleFrameSnapshot frame;
	frame.frame_index = 7;
	frame.definitions = definitions;
	p::EffectGroupFrameSnapshot group;
	group.render_domain = p::EffectRenderDomain::FirstPerson;
	group.section_visible = false;
	frame.groups.push_back(group);
	p::EffectEmitterFrameSnapshot emitter;
	emitter.id = 42;
	emitter.group_index = 0;
	emitter.first_particle = 0;
	emitter.particle_count = 3;
	frame.emitters.push_back(emitter);
	for (int i = 0; i < 3; ++i) {
		p::Particle particle;
		particle.position = {float(i), 0.0f, 0.0f};
		particle.size = 2.0f;
		particle.graphic_layer = static_cast<std::uint8_t>(i == 0 ? 1 : (i == 1 ? 3 : 2));
		frame.particles.push_back(particle);
	}
	std::vector<r::ParticleDefinitionVisual> visuals(1);
	// Layer 1 present (Additive, one entry); layer 3 absent (falls back to
	// layer 1); layer 2 Distort with no entry (no draw).
	visuals[0].layers[1].present = true;
	visuals[0].layers[1].type = 1;
	visuals[0].layers[1].frame_entries = {0};
	visuals[0].layers[2].present = true;
	visuals[0].layers[2].type = 7;
	std::vector<r::ParticleAtlasEntry> entries(1);
	entries[0].type = 1;
	entries[0].placement.valid = true;
	entries[0].placement.page = 5;
	r::ParticleRenderInput input;
	r::build_particle_render_input(frame, visuals, entries, kIdentityView, r::ParticleQuadTints{},
			input);
	check(input.snapshot.frame_id == 7, "the frame id");
	check(input.snapshot.emitters.size() == 1 && input.snapshot.particles.size() == 3,
			"one emitter, three quads");
	if (input.snapshot.emitters.size() != 1 || input.snapshot.particles.size() != 3)
		return;
	const r::ParticleEmitterSnapshot &built = input.snapshot.emitters[0];
	check(built.emitter_id == 42, "the emitter id");
	check(built.domain == r::ParticleRenderDomain::FirstPerson, "the group's render domain");
	check(!built.group_visible, "the group's section gate");
	check(built.bounds.valid && built.bounds.min.x == -1.0f && built.bounds.max.x == 3.0f,
			"the sort box takes each base half-size");
	const auto &quads = input.snapshot.particles;
	check(quads[0].visible && quads[0].state.atlas.page == 5 &&
					quads[0].state.pipeline == r::ParticlePipeline::Additive,
			"a present layer draws its entry");
	check(quads[1].visible && quads[1].state.pipeline == r::ParticlePipeline::Additive,
			"an absent layer falls back to the first present one");
	check(!quads[2].visible && quads[2].state.pass == r::ParticleRenderPass::Distortion,
			"a Distort layer draws in the distortion pass; no entry, no draw");

	r::ParticleRenderInput mismatched;
	std::vector<r::ParticleDefinitionVisual> none;
	r::build_particle_render_input(frame, none, entries, kIdentityView, r::ParticleQuadTints{},
			mismatched);
	check(mismatched.snapshot.emitters.empty(), "visuals for other definitions build nothing");
}

void no_environment_runs_unfogged_under_neutral_tints() {
	const r::ParticlePassEnvironment env = r::particle_pass_environment(nullptr, false);
	check(env.fog.end == 0.0f && env.mirror_fog.end == 0.0f, "no fog block");
	check(env.tints.ambient[0] == 1.0f && env.tints.modulator[2] == 1.0f, "neutral tints");
}

} // namespace

int main() {
	a_main_only_frame_compiles_the_main_scene_and_retires_the_pairs();
	every_view_compiles_in_frame_order_with_its_own_side_and_stride();
	a_mirror_the_frame_leaves_unrendered_compiles_nothing();
	the_distortion_row_counts_emitters_ahead_of_the_submit_tests();
	the_snapshot_takes_layers_passes_and_atlas_entries();
	no_environment_runs_unfogged_under_neutral_tints();
	if (g_failures != 0) {
		std::fprintf(stderr, "%d check(s) failed\n", g_failures);
		return 1;
	}
	std::puts("particle_frame_plan: ok");
	return 0;
}
