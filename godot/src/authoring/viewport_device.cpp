#include "authoring/viewport_device.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include <algorithm>

#include <editor/preview/viewport_model.h>

namespace godot {

namespace {

int64_t now_us() {
	return int64_t(Time::get_singleton()->get_ticks_usec());
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
	// its picture is held (a build runs, or the last one failed) the texture is the last picture, drawn
	// as it was: not rendered (half built), and not sized again once it holds one (its texture would be
	// made anew, empty).
	drawn_ = 2;
	canvas_sized_ = picture.canvas_sized;
	if (!held_()) {
		size_(picture.width, picture.height);
		viewport_->set_update_mode(SubViewport::UPDATE_ONCE);
		rendered_ = true;
	} else if (!rendered_) {
		size_(picture.width, picture.height);
	}
	Engine *engine = Engine::get_singleton();
	if (engine->has_singleton("ImGuiGD")) {
		engine->get_singleton("ImGuiGD")->call("SubViewport", viewport_);
	}
}

bool ViewportDevice::surface_at(float x, float y, float point[3]) const {
	// Only over a picture it built: none while a build runs or after one failed.
	return !held_() && applier_->surface_at(x, y, point);
}

void ViewportDevice::take(opennova::editor::ViewportAction action, const opennova::editor::ViewportModel &model,
		const opennova::editor::SessionView &view, const opennova::editor::PreviewClock &clock,
		opennova::editor::ViewportDeviceReport &report) {
	// A whole frame no canvas drew it, the size its viewport's state says (the MCP's, a headless
	// run's, its window hidden), unless it holds a picture a build or a failure keeps.
	if (drawn_ == 0 && !(held_() && rendered_)) size_(model.state().width, model.state().height);
	switch (action) {
	case opennova::editor::ViewportAction::Rebuild: {
		// The build of the viewport's newest generation: one in flight dropped (the applier's rebuild
		// discards its partial work), begun anew; a picture made in one step is made whole here.
		build_ = opennova::editor::ViewportBuildReport();
		build_.generation = model.builds();
		const int64_t start = now_us();
		applier_->rebuild(model, view);
		build_.progress = applier_->progress();
		build_.loading = applier_->building();
		if (!build_.loading) {
			// Made whole as it was taken: one unit on one frame.
			build_.frames = 1;
			build_.frame_us = build_.unit_us = build_.total_us = now_us() - start;
		}
		break;
	}
	case opennova::editor::ViewportAction::Update:
		// The viewport folds an Update that comes while a build runs into it (the build applies the
		// state as it ends): one reaching a held picture has nothing of its generation to apply to.
		if (!held_()) applier_->update(model);
		break;
	case opennova::editor::ViewportAction::Clear:
		applier_->clear();
		build_.loading = false;
		build_.failed = false;
		build_.message.clear();
		break;
	case opennova::editor::ViewportAction::Keep: break;
	}
	// A render a canvas's draw asked for earlier this frame does not happen while the picture is held:
	// the build this take began runs its units after it, and the frame's render would show them.
	if (held_()) viewport_->set_update_mode(SubViewport::UPDATE_DISABLED);
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
