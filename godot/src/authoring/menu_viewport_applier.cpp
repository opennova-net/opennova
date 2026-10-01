#include "authoring/menu_viewport_applier.h"

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/vector2.hpp>

#include <algorithm>

#include <editor/preview/menu_viewport.h>
#include <editor/preview/viewport_device.h>
#include <editor/session/view/session_view.h>

namespace godot {

namespace {

opennova::editor::OperationProgress units_done(uint64_t units) {
	opennova::editor::OperationProgress progress;
	progress.done = units;
	progress.total = units;
	progress.unit = opennova::editor::OperationUnit::Steps;
	progress.label = "configure";
	return progress;
}

} // namespace

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
	// A configure in flight dropped (what it loaded ahead stays kept: the next configure finds it).
	build_.reset();
	const auto &menu = static_cast<const opennova::editor::MenuViewport &>(model);
	if (!menu.image() || !menu.screen() || !view.findings.assets) {
		clear();
		return;
	}
	// What the screen names that the frame does not keep: the viewport's own compile of the same
	// screen, over the same files, interned the same names.
	const opennova::FileSource &files = *view.findings.assets;
	const opennova::menu::MenuFrameAssets &kept = frame_->native_assets();
	const opennova::menu::MenuFrameCompiler &compiled = menu.render().compiler();
	auto build = std::make_unique<Build>();
	for (const std::string &name : compiled.texture_names())
		if (!kept.texture_kept(name, files)) build->textures.push_back(name);
	if (build->textures.empty()) {
		// Everything kept: configured now, as it is taken.
		configure_(model, view.findings.assets);
		done_ = units_done(1);
		return;
	}
	build->assets = view.findings.assets;
	build_ = std::move(build);
}

ApplierStep MenuViewportApplier::step(const opennova::editor::ViewportModel &model,
		const opennova::editor::PreviewClock &, std::string &) {
	Build &build = *build_;
	const size_t unit = build.next++;
	if (unit < build.textures.size()) {
		frame_->load_texture_ahead(build.textures[unit], *build.assets);
		return ApplierStep::More;
	}
	// The configure, which decodes nothing it loaded ahead.
	const std::shared_ptr<const opennova::editor::ProjectAssetSource> assets = build.assets;
	done_ = units_done(build.units());
	build_.reset();
	configure_(model, assets);
	return ApplierStep::Built;
}

opennova::editor::OperationProgress MenuViewportApplier::progress() const {
	if (!build_) return done_;
	opennova::editor::OperationProgress progress;
	progress.done = build_->next;
	progress.total = build_->units();
	progress.unit = opennova::editor::OperationUnit::Steps;
	progress.label = build_->next < build_->textures.size() ? "textures" : "configure";
	return progress;
}

void MenuViewportApplier::configure_(const opennova::editor::ViewportModel &model,
		const std::shared_ptr<const opennova::editor::ProjectAssetSource> &assets) {
	const auto &menu = static_cast<const opennova::editor::MenuViewport &>(model);
	if (!menu.image() || !menu.screen() || !assets) {
		clear();
		return;
	}
	// Every configure is a first load of its textures: no earlier menu's first load fixes a band
	// height here. The frame borrows the image's screen and the file source until its next
	// configure: both held here as long.
	frame_->reset_loads();
	frame_->configure_screen(menu.image().get(), menu.screen(), *assets, menu.style_vars(), nullptr);
	image_ = menu.image();
	assets_ = assets;
	apply_options_(model);
}

void MenuViewportApplier::update(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &) {
	apply_options_(model);
}

void MenuViewportApplier::clear() {
	build_.reset();
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

void MenuViewportApplier::apply(const opennova::editor::ViewportModel &, const opennova::editor::PreviewClock &,
		opennova::editor::ViewportDeviceReport &report) {
	// While a configure runs over the frames the frame holds the last screen: nothing of it reported.
	if (build_ || !frame_->is_configured()) return;
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

void MenuViewportApplier::resize(int width, int height) {
	frame_->set_size(Vector2(float(width), float(height)));
}

} // namespace godot
