#include "authoring/menu_viewport_applier.h"

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/vector2.hpp>

#include <algorithm>

#include <editor/preview/menu_viewport.h>
#include <editor/preview/viewport_device.h>
#include <editor/session/view/session_view.h>

namespace godot {

MenuViewportApplier::MenuViewportApplier(SubViewport &viewport) {
	frame_ = memnew(MenuFrame);
	// The canvas picks through the viewport's compile; the frame takes no input of its own.
	frame_->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	frame_->set_size(Vector2(float(opennova::menu::kMenuDesignWidth), float(opennova::menu::kMenuDesignHeight)));
	viewport.add_child(frame_);
	frame_id_ = frame_->get_instance_id();
}

MenuViewportApplier::~MenuViewportApplier() {
	if (MenuFrame *frame = Object::cast_to<MenuFrame>(ObjectDB::get_instance(frame_id_))) frame->clear_screen();
}

void MenuViewportApplier::rebuild(const opennova::editor::ViewportModel &model, const opennova::editor::SessionView &view) {
	const auto &menu = static_cast<const opennova::editor::MenuViewport &>(model);
	if (!menu.image() || !menu.screen() || !view.findings.assets) {
		clear();
		return;
	}
	// Every configure is a first load of its textures: no earlier menu's first load fixes a band
	// height here. The frame borrows the image's screen and the file source until its next
	// configure: both held here as long.
	frame_->reset_loads();
	frame_->configure_screen(menu.image().get(), menu.screen(), *view.findings.assets, menu.style_vars(), nullptr);
	image_ = menu.image();
	assets_ = view.findings.assets;
	apply_options_(model);
}

void MenuViewportApplier::update(const opennova::editor::ViewportModel &model) {
	apply_options_(model);
}

void MenuViewportApplier::clear() {
	frame_->clear_screen();
	image_.reset();
	assets_.reset();
}

void MenuViewportApplier::apply_options_(const opennova::editor::ViewportModel &model) {
	const auto &menu = static_cast<const opennova::editor::MenuViewport &>(model);
	opennova::menu::MenuFrameState state = frame_->native_state();
	opennova::editor::apply_menu_options(menu.options(), menu.forced_index(), frame_->native_compiler(), state);
	frame_->set_native_state(state);
}

void MenuViewportApplier::step(const opennova::editor::ViewportModel &, const opennova::editor::PreviewClock &,
		opennova::editor::ViewportDeviceReport &report) {
	if (!frame_->is_configured()) return;
	// What the frame read, and where its compile placed each widget (design units).
	for (const opennova::menu::MenuDependency &dependency : frame_->native_assets().dependencies())
		report.files.note(dependency.name, dependency.stamp);
	const opennova::menu::MenuFrameCompiler &compiler = frame_->native_compiler();
	const opennova::menu::MenuFrameState &state = frame_->native_state();
	report.rects.resize(size_t(std::max(compiler.widget_count(), 0)));
	for (int index = 0; index < compiler.widget_count(); ++index) {
		opennova::mnu::RectEdges rect{};
		opennova::editor::ViewportDeviceReport::Rect &placed = report.rects[size_t(index)];
		placed.placed = compiler.widget_rect(index, state, &rect);
		placed.left = rect.left;
		placed.top = rect.top;
		placed.right = rect.right;
		placed.bottom = rect.bottom;
	}
}

void MenuViewportApplier::tick(const opennova::editor::ViewportModel &model,
		const opennova::editor::PreviewClock &clock) {
	if (!frame_->is_configured()) return;
	const auto &menu = static_cast<const opennova::editor::MenuViewport &>(model);
	uint32_t time = 0;
	if (opennova::editor::menu_frame_clock(menu, frame_->native_state().time_ms, clock, time))
		frame_->set_time_ms(time);
}

void MenuViewportApplier::resize(int width, int height) {
	frame_->set_size(Vector2(float(width), float(height)));
}

} // namespace godot
