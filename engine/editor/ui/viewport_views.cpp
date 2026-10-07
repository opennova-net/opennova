#include <editor/ui/viewport_views.h>

#include <iterator>

#include <editor/ui/definition_viewport_view.h>
#include <editor/ui/effect_viewport_view.h>
#include <editor/ui/environment_viewport_view.h>
#include <editor/ui/terrain_viewport_view.h>
#include <editor/ui/hud_viewport_view.h>
#include <editor/ui/menu_viewport_view.h>
#include <editor/ui/mission_viewport_view.h>
#include <editor/ui/model_viewport_view.h>
#include <editor/ui/script_viewport_view.h>
#include <editor/ui/texture_viewport_view.h>

namespace opennova::editor {

namespace {

std::unique_ptr<ViewportView> make_menu_view() { return std::make_unique<MenuViewportView>(); }
std::unique_ptr<ViewportView> make_model_view() { return std::make_unique<ModelViewportView>(); }
std::unique_ptr<ViewportView> make_script_view() { return std::make_unique<ScriptViewportView>(); }
std::unique_ptr<ViewportView> make_mission_view() { return std::make_unique<MissionViewportView>(); }
std::unique_ptr<ViewportView> make_texture_viewport_view() { return std::make_unique<TextureViewportView>(); }
std::unique_ptr<ViewportView> make_effect_view() { return std::make_unique<EffectViewportView>(); }
std::unique_ptr<ViewportView> make_hud_viewport_view() { return std::make_unique<HudViewportView>(); }
std::unique_ptr<ViewportView> make_definition_view() { return std::make_unique<DefinitionViewportView>(); }
std::unique_ptr<ViewportView> make_environment_view() { return std::make_unique<EnvironmentViewportView>(); }
std::unique_ptr<ViewportView> make_terrain_view() { return std::make_unique<TerrainViewportView>(); }

constexpr ViewportViewRow kViews[] = {
	{ ViewportKind::Menu, make_menu_view },
	{ ViewportKind::Model, make_model_view },
	{ ViewportKind::Script, make_script_view },
	{ ViewportKind::Mission, make_mission_view },
	{ ViewportKind::Texture, make_texture_viewport_view },
	{ ViewportKind::Effect, make_effect_view },
	{ ViewportKind::Hud, make_hud_viewport_view },
	{ ViewportKind::Definition, make_definition_view },
	{ ViewportKind::Environment, make_environment_view },
	{ ViewportKind::Terrain, make_terrain_view },
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
