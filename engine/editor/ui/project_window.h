#pragma once

#include <string>

#include <editor/session/editor_request.h>
#include <editor/ui/editor_host.h>
#include <runtime/devtools/imgui_pass.h>

namespace opennova::editor {

// The centre window: with no project open, the new-project form, the recent list and
// Open; with one open, the project's name, game and features, the game runtime Play
// uses, and the Build / Play / Stop controls. Never closeable: it is the editor's
// home.
class ProjectWindow : public devtools::Window {
public:
	explicit ProjectWindow(EditorHost &host) : host_(host) {}

	const char *title() const override { return "Project"; }
	bool is_closeable() const override { return false; }
	devtools::InitialDockPlacement initial_dock_placement() const override {
		return devtools::InitialDockPlacement::Center;
	}
	void draw(devtools::ImGuiPass &pass, uint64_t frame_index) override;

	// A picked directory / file lands in the form field that asked for it.
	void set_picked(PickPurpose purpose, const std::string &path);

	// The new-project form, readable by a test.
	const char *new_project_title() const { return title_; }
	const char *new_project_location() const { return location_; }

private:
	void draw_home();
	void draw_open_project();

	EditorHost &host_;
	char title_[128] = "My Game";
	char location_[512] = "";
	char runtime_[512] = "";
	char edited_title_[128] = "";
	uint64_t edited_title_revision_ = ~uint64_t{0};
	uint64_t runtime_revision_ = ~uint64_t{0};
};

} // namespace opennova::editor
