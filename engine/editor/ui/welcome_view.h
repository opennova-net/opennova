#pragma once

#include <string>

#include <editor/assets/install_check.h>
#include <editor/ui/expansion_fields.h>
#include <editor/ui/workspace.h>

namespace opennova::editor {

struct SessionView;

// One install field's check (the New project form's, Project settings'): the folder named asked of the
// session once it is not being typed (CheckInstall), the answer kept by the field when the view's one slot
// holds it, so another field's check over that slot leaves this one's line as it was; asked again when the
// slot was taken before the field read its answer.
class InstallFieldCheck {
public:
	// What the field's line says of `path`: the kept answer's words (`ok` true for an install the editor
	// imports from), "" while it waits; nothing named, what a project made then takes (`fallback`, the
	// editor's install a new project takes when none is named; "" for none).
	std::string words(Workspace &workspace, const std::string &path, bool typing, const std::string &fallback, bool &ok);
	// The kept answer for `path` (null while there is none).
	const InstallCheck *answer(const std::string &path) const;
	void forget();

private:
	std::string asked_;           // the folder asked, as named
	std::string slot_when_asked_; // the folder the slot held then
	InstallCheck kept_;           // the answer for it
	int retries_ = 0;             // asks again after the slot was taken (two at most for a folder)
};

// The new-project form: a name, a folder (Browse... asks the shell for one), the game install the project
// imports from and plays in (the UX round's project lane: prefilled with the one the editor last chose,
// Browse..., and checked as it is named, CheckInstall, what it holds said under it), what it builds on and
// whether it builds as an expansion (ADR 0046 S16: ExpansionFields, over the expansions of the install
// named), and Create, which raises NewProject with the install. What it holds is the workspace's
// (new_project, the MCP gaps lane): each field draws it and a person's change is a set_workspace, so the
// editor MCP fills it as a person does; the welcome page and File > New project... draw the one form.
class NewProjectForm {
public:
	// True when Create raised the request.
	bool draw(Workspace &workspace);

private:
	ui_kit::HeldText<kWorkspaceText> title_;
	ui_kit::HeldText<kWorkspacePath> folder_;
	ui_kit::HeldText<kWorkspacePath> install_;
	InstallFieldCheck check_;
	ExpansionFields expansion_;
};

// The Document window with no project open, which then has the whole workspace (the other windows stand
// aside: the UX round's project lane): the editor's name and what it is for; the recent projects, each by
// its title with its game, its expansion and its folder (a click opens one, Forget drops it from the list,
// one whose folder no longer holds a project said so); Open a project folder...; the new-project form;
// then what last happened.
void draw_welcome(Workspace &workspace, NewProjectForm &form);

// Whether a workspace window (Files, the Inspector, the Preview, Problems, Output) stands aside for the
// welcome page: no project is open and the author has not asked to see it (`asked`, set by its Windows
// menu tick, show_anyway; a project opening forgets it).
bool aside_for_welcome(const SessionView &view, bool &asked);

} // namespace opennova::editor
