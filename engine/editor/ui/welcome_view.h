#pragma once

#include <string>

#include <editor/ui/expansion_fields.h>
#include <editor/ui/workspace.h>

namespace opennova::editor {

// The new-project form: a name, a folder (Browse... asks the shell for one), what it builds on and
// whether it builds as an expansion (ADR 0046 S16: ExpansionFields), and Create, which raises
// NewProject. The welcome view and File > New project... draw the one form, so the shell's folder
// pick lands in it wherever it shows.
class NewProjectForm {
public:
	// True when Create raised the request.
	bool draw(Workspace &workspace);
	void set_folder(const std::string &path);
	const char *title() const { return title_; }
	const char *folder() const { return folder_; }

private:
	char title_[128] = "My Game";
	char folder_[512] = "";
	ExpansionFields expansion_;
};

// The Document window with no project open: the new-project form, Open... and the recent
// projects (a click opens one, Forget drops it from the list), then what last happened.
void draw_welcome(Workspace &workspace, NewProjectForm &form);

} // namespace opennova::editor
