#include "authoring/menu_preview.h"

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include <editor/session/view/session_view.h>

namespace godot {

MenuPreview::MenuPreview(Node &owner) :
		owner_(owner) {
	viewport_ = memnew(SubViewport);
	// It renders on the frames the window draws it (draw() asks for one update).
	viewport_->set_update_mode(SubViewport::UPDATE_DISABLED);
	viewport_->set_size(Vector2i(opennova::menu::kMenuDesignWidth, opennova::menu::kMenuDesignHeight));
	frame_ = memnew(MenuFrame);
	// The window picks through the compiler; the frame takes no input of its own.
	frame_->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	frame_->set_size(Vector2(float(opennova::menu::kMenuDesignWidth), float(opennova::menu::kMenuDesignHeight)));
	viewport_->add_child(frame_);
	owner_.add_child(viewport_);
}

MenuPreview::~MenuPreview() {
	if (viewport_ != nullptr && viewport_->is_inside_tree()) {
		viewport_->queue_free();
	}
}

void MenuPreview::refresh(const opennova::editor::SessionView &view) {
	const opennova::editor::MenuPreviewAction action = model_.follow(view);
	if (action == opennova::editor::MenuPreviewAction::Clear) {
		frame_->clear_screen();
		assets_.reset();
		return;
	}
	if (action != opennova::editor::MenuPreviewAction::Configure) {
		return;
	}
	// The frame borrows the file source until its next configure.
	assets_ = view.findings.assets;
	// Every configure is a first load of its textures: no earlier menu's first load fixes
	// a band height here.
	frame_->reset_loads();
	frame_->configure_screen(model_.image(), model_.screen(), *assets_, model_.style_vars(), nullptr);
	model_.configured(frame_->native_assets());
	apply_options_();
}

void MenuPreview::set_options(const opennova::editor::MenuPreviewOptions &options) {
	model_.set_options(options);
}

// The options on the configured screen: the device size (until the window draws it at
// its own), then the portable rule's frame state (every window shown, the forced window
// held as the options say).
void MenuPreview::apply_options_() {
	const opennova::editor::MenuPreviewOptions &options = model_.options();
	if (width_ == 0 && height_ == 0 && options.width > 0 && options.height > 0) {
		viewport_->set_size(Vector2i(options.width, options.height));
		frame_->set_size(Vector2(float(options.width), float(options.height)));
	}
	opennova::menu::MenuFrameState state = frame_->native_state();
	opennova::editor::apply_menu_preview_options(options, model_.forced_index(), frame_->native_compiler(), state);
	frame_->set_native_state(state);
}

opennova::editor::MenuPreviewSnapshot MenuPreview::snapshot(const opennova::editor::SessionView &view) const {
	return opennova::editor::menu_preview_snapshot(view, model_, &frame_->native_compiler(), &frame_->native_state());
}

opennova::editor::MenuPreviewStatus MenuPreview::status(std::string *detail) const {
	if (detail != nullptr) {
		*detail = model_.detail();
	}
	return model_.status();
}

void MenuPreview::draw(int device_width, int device_height) {
	if (viewport_ == nullptr || model_.status() != opennova::editor::MenuPreviewStatus::Ready) {
		return;
	}
	if (device_width != width_ || device_height != height_) {
		width_ = device_width;
		height_ = device_height;
		viewport_->set_size(Vector2i(width_, height_));
		frame_->set_size(Vector2(float(width_), float(height_)));
	}
	viewport_->set_update_mode(SubViewport::UPDATE_ONCE);
	Engine *engine = Engine::get_singleton();
	if (engine->has_singleton("ImGuiGD")) {
		engine->get_singleton("ImGuiGD")->call("SubViewport", viewport_);
	}
}

const opennova::menu::MenuFrameCompiler *MenuPreview::compiler() const {
	return model_.status() == opennova::editor::MenuPreviewStatus::Ready && frame_->is_configured()
			? &frame_->native_compiler()
			: nullptr;
}

const opennova::menu::MenuFrameState *MenuPreview::frame_state() const {
	return compiler() != nullptr ? &frame_->native_state() : nullptr;
}

} // namespace godot
