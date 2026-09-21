#pragma once
#include <editor/ui/editor_host.h>
#include <runtime/devtools/imgui_pass.h>

namespace opennova::editor {
class CatalogWindow : public devtools::Window {
public:
	explicit CatalogWindow(EditorHost &host) : host_(host) { open = true; }
	const char *title() const override { return "Catalog"; }
	devtools::InitialDockPlacement initial_dock_placement() const override { return devtools::InitialDockPlacement::Center; }
	void draw(devtools::ImGuiPass &, uint64_t) override;
private:
	EditorHost &host_;
	char filter_[128]{};
	bool sort_names_ = false;
};
class CatalogInspector : public devtools::Window {
public:
	explicit CatalogInspector(EditorHost &host) : host_(host) { open = true; }
	const char *title() const override { return "Inspector"; }
	devtools::InitialDockPlacement initial_dock_placement() const override { return devtools::InitialDockPlacement::Right; }
	void draw(devtools::ImGuiPass &, uint64_t) override;
private:
	EditorHost &host_;
	char filter_[128]{};
};
} // namespace opennova::editor
