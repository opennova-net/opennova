#include <editor/ui/viewport_views.h>

#include <iterator>

#include <editor/session/row_table.h>
#include <editor/ui/definition_viewport_view.h>
#include <editor/ui/effect_viewport_view.h>
#include <editor/ui/environment_viewport_view.h>
#include <editor/ui/font_viewport_view.h>
#include <editor/ui/terrain_viewport_view.h>
#include <editor/ui/hud_viewport_view.h>
#include <editor/ui/menu_viewport_view.h>
#include <editor/ui/mission_map_view.h>
#include <editor/ui/mission_viewport_view.h>
#include <editor/ui/model_viewport_view.h>
#include <editor/ui/script_viewport_view.h>
#include <editor/ui/texture_viewport_view.h>

namespace opennova::editor {

namespace {

template <class V> std::unique_ptr<ViewportView> make_view() { return std::make_unique<V>(); }

constexpr ViewportViewRow kViews[] = {
	{ ViewportKind::Menu, make_view<MenuViewportView> },
	{ ViewportKind::Model, make_view<ModelViewportView> },
	{ ViewportKind::Script, make_view<ScriptViewportView> },
	{ ViewportKind::Mission, make_view<MissionViewportView> },
	{ ViewportKind::Texture, make_view<TextureViewportView> },
	{ ViewportKind::Effect, make_view<EffectViewportView> },
	{ ViewportKind::Hud, make_view<HudViewportView> },
	{ ViewportKind::Definition, make_view<DefinitionViewportView> },
	{ ViewportKind::Environment, make_view<EnvironmentViewportView> },
	{ ViewportKind::Terrain, make_view<TerrainViewportView> },
	{ ViewportKind::Font, make_view<FontViewportView> },
	{ ViewportKind::Map, make_view<MissionMapViewportView> },
};

static_assert(std::size(kViews) == kViewportKindCount, "every ViewportKind has exactly one view");
static_assert(rows_follow(kViews, &ViewportViewRow::kind), "the viewport views follow ViewportKind's order");

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
