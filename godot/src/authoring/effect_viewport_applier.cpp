#include "authoring/effect_viewport_applier.h"
#include "authoring/orbit_stage.h"
#include "authoring/preview_godot.h"

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/base_material3d.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <editor/preview/effect_viewport.h>
#include <editor/preview/preview_clock.h>
#include <editor/preview/viewport_device.h>
#include <editor/session/view/session_view.h>

#include "authoring/preview_backdrop.h"
#include "env/mission_environment.h"
#include "render/frame_fx.h"

namespace godot {

namespace {

const opennova::editor::EffectViewport &effect_of(const opennova::editor::ViewportModel &model) {
	return static_cast<const opennova::editor::EffectViewport &>(model);
}

} // namespace

EffectViewportApplier::EffectViewportApplier(SubViewport &viewport) {
	// The orbit devices' stage (authoring/orbit_stage), its grid with it.
	OrbitStage stage = make_orbit_stage(viewport, true);
	backdrop_ = stage.backdrop;
	frame_effects_ = std::move(stage.frame_effects);
	environment_ = stage.environment;
	camera_ = stage.camera;
	grid_ = stage.grid;
	effects_ = std::make_unique<PreviewEffects>(*stage.root);
	effects_->set_environment_source(environment_);
}

EffectViewportApplier::~EffectViewportApplier() = default;

void EffectViewportApplier::background(opennova::editor::PreviewBackground background) {
	set_preview_backdrop(*backdrop_, background);
	grid_->set_mesh(preview_grid_mesh(background));
}

void EffectViewportApplier::rebuild(const opennova::editor::ViewportModel &viewport,
		const opennova::editor::SessionView &view, const opennova::editor::PreviewClock &clock) {
	const opennova::editor::EffectViewport &model = effect_of(viewport);
	if (!model.playback().scene() || !view.findings.assets) {
		clear();
		return;
	}
	// The project's files the graphics are read from (mounted again where one read moved), and the scene
	// the viewport opened last; drawn now as it stands.
	effects_->mount(view.findings.assets);
	effects_->show(model.playback().scene());
	shows_ = true;
	place_(viewport);
	effects_->render(int64_t(clock.ms()));
}

void EffectViewportApplier::update(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &) {
	place_(model);
}

void EffectViewportApplier::clear() {
	effects_->clear();
	effects_->set_environment_source(environment_);
	shows_ = false;
}

void EffectViewportApplier::place_(const opennova::editor::ViewportModel &viewport) {
	const opennova::editor::EffectViewport &model = effect_of(viewport);
	place_orbit_camera(*camera_, model.camera(), false);
	grid_->set_visible(model.options().grid);
}

void EffectViewportApplier::apply(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &,
		opennova::editor::ViewportDeviceReport &report) {
	place_(model);
	if (!shows_) return;
	report.files = effects_->stamps();
	report.missing = effects_->missing();
}

void EffectViewportApplier::tick(const opennova::editor::ViewportModel &viewport, const opennova::editor::PreviewClock &clock) {
	if (!shows_) return;
	// The scene the viewport stepped, drawn as it stands (a scene opened again since the last Rebuild is the
	// next one's to take).
	(void)viewport;
	effects_->render(int64_t(clock.ms()));
	frame_effects_->present(effects_->renderer());
}

} // namespace godot
