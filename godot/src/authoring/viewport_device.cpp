#include "authoring/viewport_device.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include <algorithm>

#include <editor/preview/viewport_model.h>

#include "env/mission_environment.h"
#include "env/water.h"

namespace godot {

namespace {

int64_t now_us() {
	return int64_t(Time::get_singleton()->get_ticks_usec());
}

// The process frame now: a canvas's draw, the pump's take and the frame's steps read the same one.
uint64_t frame_now() {
	return Engine::get_singleton()->get_process_frames();
}

} // namespace

ViewportDevice::ViewportDevice(Node &owner, const String &name,
		const std::function<std::unique_ptr<ViewportApplier>(SubViewport &)> &make, Retire retire) :
		retire_(std::move(retire)) {
	viewport_ = memnew(SubViewport);
	viewport_->set_name(name);
	// It renders on the frames a canvas draws it (draw() asks for one update), in a world of its
	// own, and keeps its last picture between.
	viewport_->set_update_mode(SubViewport::UPDATE_DISABLED);
	viewport_->set_use_own_world_3d(true);
	applier_ = make(*viewport_);
	owner.add_child(viewport_);
	viewport_id_ = viewport_->get_instance_id();
}

ViewportDevice::~ViewportDevice() {
	// The applier lets go of what its nodes borrowed (a menu's frame its screen) while they live.
	applier_.reset();
	// Gone already where its owner's teardown freed its children first.
	SubViewport *viewport = Object::cast_to<SubViewport>(ObjectDB::get_instance(viewport_id_));
	if (viewport == nullptr) return;
	viewport->set_update_mode(SubViewport::UPDATE_DISABLED);
	if (retire_) retire_(viewport);
	else if (viewport->is_inside_tree()) viewport->queue_free();
}

void ViewportDevice::size_(int width, int height) {
	const Vector2i size(width > 0 ? width : 1, height > 0 ? height : 1);
	if (viewport_->get_size() == size) return;
	viewport_->set_size(size);
	applier_->resize(size.x, size.y);
}

void ViewportDevice::draw(const opennova::editor::ViewportPicture &picture) {
	// Its texture drawn through the ImGui pass as the canvas's current item: the size alone. While
	// it keeps its last picture (a build runs, or the last one failed) the texture is that picture,
	// drawn as it was: not rendered (half built), and not sized again once it holds one (its texture
	// would be made anew, empty).
	drawn_ = 2;
	canvas_sized_ = picture.canvas_sized;
	// Where the mouse is over the picture, for a picture that draws the game's pointer (DI-08).
	applier_->pointer(picture.pointer, picture.pointer_x, picture.pointer_y);
	if (!keeps_last_()) {
		size_(picture.width, picture.height);
		viewport_->set_update_mode(SubViewport::UPDATE_ONCE);
		rendered_ = true;
		render_frame_ = frame_now();
		render_asked_ = true;
	} else if (!rendered_) {
		size_(picture.width, picture.height);
	}
	Engine *engine = Engine::get_singleton();
	if (engine->has_singleton("ImGuiGD")) {
		engine->get_singleton("ImGuiGD")->call("SubViewport", viewport_);
	}
}

void ViewportDevice::publish_scene_state() {
	if (applier_->scene_state() != 0) {
		applier_->publish_scene_state();
		return;
	}
	// The shipped defaults: the retail noon the environment globals have before any mission
	// publishes, and no water.
	MissionEnvironment::publish_shipped_defaults();
	Water::publish_absent();
}

void ViewportDevice::withhold_render() {
	// The frame's render dropped: the last picture stands, and the frame is no render frame of its
	// (its units may run).
	viewport_->set_update_mode(SubViewport::UPDATE_DISABLED);
	render_asked_ = false;
	render_frame_ = UINT64_MAX;
}

void ViewportDevice::present(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock) {
	(void)model;
	// The clock's seconds since the last present (none, or a paused clock: 0).
	const double ms = double(clock.ms());
	const double dt = presented_ms_ < 0.0 || ms < presented_ms_ ? 0.0 : (ms - presented_ms_) / 1000.0;
	presented_ms_ = ms;
	render_asked_ = false;
	rendered_frame_ = frame_now();
	applier_->present(dt);
}

bool ViewportDevice::surface_at(float x, float y, float point[3]) const {
	// Only over a picture it built: none while a build runs or after one failed.
	return !keeps_last_() && applier_->surface_at(x, y, point);
}

bool ViewportDevice::surface_between(const double from[3], const double to[3], double point[3]) const {
	// The applier's to answer: the layer that holds its surface may stand while it builds another.
	return applier_->surface_between(from, to, point);
}

bool ViewportDevice::ground_at(double x, double y, double &height) const {
	return applier_->ground_at(x, y, height);
}

opennova::editor::ViewportRayHit ViewportDevice::ray_between(const double from[3], const double to[3]) const {
	// The applier's to answer, as the surface is: its records stand while it builds another layer.
	return applier_->ray_between(from, to);
}

void ViewportDevice::take(opennova::editor::ViewportAction action, const opennova::editor::ViewportModel &model,
		const opennova::editor::SessionView &view, const opennova::editor::PreviewClock &clock,
		opennova::editor::ViewportDeviceReport &report) {
	// A whole frame no canvas drew it, the size its viewport's state says (the MCP's, a headless
	// run's, its window hidden), unless it holds a picture a build or a failure keeps.
	const uint64_t frame = frame_now();
	if (drawn_ == 0 && !(keeps_last_() && rendered_)) size_(model.state().width, model.state().height);
	switch (action) {
	case opennova::editor::ViewportAction::Rebuild: {
		// The build of the viewport's newest generation: one in flight dropped (the applier's rebuild
		// discards its partial work), begun anew; a picture made in one step is made whole here.
		build_ = opennova::editor::ViewportBuildReport();
		build_.generation = model.builds();
		const int64_t start = now_us();
		applier_->rebuild(model, view, clock);
		build_.progress = applier_->progress();
		build_.loading = applier_->building();
		if (!build_.loading) {
			// Made whole as it was taken: one unit on one frame.
			build_.frames = 1;
			build_.frame_us = build_.unit_us = build_.total_us = now_us() - start;
			built_once_ = true;
		}
		break;
	}
	case opennova::editor::ViewportAction::Update:
		// The viewport folds an Update that comes while a build runs into it (the build applies the
		// state as it ends): one reaching a kept picture has nothing of its generation to apply to.
		if (!keeps_last_()) applier_->update(model, clock);
		break;
	case opennova::editor::ViewportAction::Clear:
		applier_->clear();
		build_.loading = false;
		build_.failed = false;
		build_.message.clear();
		break;
	case opennova::editor::ViewportAction::Keep: break;
	}
	// Nothing renders while it keeps its last picture, but for the render this frame's draw asked for
	// before this take began a build: the scene is still the last complete one (a Rebuild plans the
	// units, none has run), so the frame renders that, and no unit runs in it (step()). A canvas resized
	// this frame gets its picture, and a build that ended last frame is shown before the next begins.
	if (keeps_last_() && render_frame_ != frame) viewport_->set_update_mode(SubViewport::UPDATE_DISABLED);
	applier_->apply(model, clock, report);
	const Vector2i size = viewport_->get_size();
	report.width = size.x;
	report.height = size.y;
	report.canvas_sized = drawn_ > 0 && canvas_sized_;
}

void ViewportDevice::tick(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock) {
	// A new frame: its units counted from here (the Shell steps the builds after the tick).
	frame_us_ = -1;
	applier_->tick(model, clock);
	// The frame is over: a canvas that drew it this frame sizes it until the next one's layout pass.
	if (drawn_ > 0) --drawn_;
}

bool ViewportDevice::step(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock) {
	if (!build_.loading || !applier_->building()) return false;
	// The frame renders the last complete picture its draw asked for as it ends: its units wait for
	// the next frame.
	if (render_frame_ == frame_now()) return false;
	const int64_t start = now_us();
	std::string failure;
	const ApplierStep result = applier_->step(model, clock, failure);
	const int64_t spent = now_us() - start;
	// The frame's first unit counts the frame (frame_us_ is -1 from the tick until one runs).
	if (frame_us_ < 0) {
		frame_us_ = 0;
		++build_.frames;
	}
	frame_us_ += spent;
	build_.frame_us = std::max(build_.frame_us, frame_us_);
	build_.unit_us = std::max(build_.unit_us, spent);
	build_.total_us += spent;
	switch (result) {
	case ApplierStep::More: build_.progress = applier_->progress(); break;
	case ApplierStep::Built:
		build_.loading = false;
		build_.progress.done = build_.progress.total;
		built_once_ = true;
		break;
	case ApplierStep::Failed:
		build_.loading = false;
		build_.failed = true;
		build_.message = failure;
		break;
	}
	return true;
}

} // namespace godot
