#include <editor/ui/viewport_views.h>

#include <iterator>

#include <editor/ui/menu_viewport_view.h>
#include <editor/ui/model_viewport_view.h>

namespace opennova::editor {

namespace {

std::unique_ptr<ViewportView> make_menu_view() { return std::make_unique<MenuViewportView>(); }
std::unique_ptr<ViewportView> make_model_view() { return std::make_unique<ModelViewportView>(); }

constexpr ViewportViewRow kViews[] = {
	{ ViewportKind::Menu, make_menu_view },
	{ ViewportKind::Model, make_model_view },
};

constexpr bool views_in_order() {
	for (size_t i = 0; i < kViewportKindCount; ++i)
		if (kViews[i].kind != static_cast<ViewportKind>(i) || !kViews[i].make) return false;
	return true;
}

static_assert(std::size(kViews) == kViewportKindCount, "every ViewportKind has exactly one view");
static_assert(views_in_order(), "the viewport views follow ViewportKind's order, each with its make");

} // namespace

const ViewportViewRow *viewport_view_row(ViewportKind kind) {
	const size_t index = static_cast<size_t>(kind);
	return index < kViewportKindCount ? &kViews[index] : nullptr;
}

std::unique_ptr<ViewportView> make_viewport_view(ViewportKind kind) {
	const ViewportViewRow *row = viewport_view_row(kind);
	return row ? row->make() : nullptr;
}

} // namespace opennova::editor
