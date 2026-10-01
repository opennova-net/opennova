#include "authoring/viewport_device.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include <editor/preview/viewport_model.h>

namespace godot {

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
	// Its texture drawn through the ImGui pass as the canvas's current item: the size alone.
	drawn_ = 2;
	canvas_sized_ = picture.canvas_sized;
	size_(picture.width, picture.height);
	viewport_->set_update_mode(SubViewport::UPDATE_ONCE);
	Engine *engine = Engine::get_singleton();
	if (engine->has_singleton("ImGuiGD")) {
		engine->get_singleton("ImGuiGD")->call("SubViewport", viewport_);
	}
}

bool ViewportDevice::surface_at(float x, float y, float point[3]) const {
	return applier_->surface_at(x, y, point);
}

void ViewportDevice::take(opennova::editor::ViewportAction action, const opennova::editor::ViewportModel &model,
		const opennova::editor::SessionView &view, const opennova::editor::PreviewClock &clock,
		opennova::editor::ViewportDeviceReport &report) {
	// A whole frame no canvas drew it, the size its viewport's state says (the MCP's, a headless
	// run's, its window hidden).
	if (drawn_ == 0) size_(model.state().width, model.state().height);
	switch (action) {
	case opennova::editor::ViewportAction::Rebuild: applier_->rebuild(model, view); break;
	case opennova::editor::ViewportAction::Update: applier_->update(model); break;
	case opennova::editor::ViewportAction::Clear: applier_->clear(); break;
	case opennova::editor::ViewportAction::Keep: break;
	}
	applier_->step(model, clock, report);
	const Vector2i size = viewport_->get_size();
	report.width = size.x;
	report.height = size.y;
	report.canvas_sized = drawn_ > 0 && canvas_sized_;
}

void ViewportDevice::tick(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock) {
	applier_->tick(model, clock);
	// The frame is over: a canvas that drew it this frame sizes it until the next one's layout pass.
	if (drawn_ > 0) --drawn_;
}

} // namespace godot
