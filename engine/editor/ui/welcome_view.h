#pragma once

#include <string>

#include <editor/assets/install_check.h>
#include <editor/session/editor_request.h>
#include <editor/session/view/workspace_view.h>
#include <editor/ui/ui_kit.h>
#include <editor/ui/workspace.h>

namespace opennova::editor {

struct NovaWorldView;
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

// The line under an install field, cut to the line: its check's `words` in green for an install the editor
// imports from (`ok`), a refusal for a folder named that holds none, muted while none is `named` or the check
// waits ("" for "Checking the folder...").
void draw_install_line(const std::string &words, bool ok, bool named);

// What Create raises of the new-project form `form` over NovaWorld's lists (`novaworld`; `install`: the game install
// it names, else the editor's): a game you own's project (new_project's all, every file of its game imported as it
// opens) of a retail game, or of a retail expansion on its base game's project (base_key: found or made first); a
// mod, an expansion of the NovaWorld game it changes (base_key), the install only where its base is made from it; a
// new game, a standalone project of its title. Set up for a NovaWorld project, it names it (project_key).
EditorRequest new_project_request(const WorkspaceView::NewProject &form, const NovaWorldView &novaworld,
                                  const std::string &install);

// The new-project form, which asks what is being made (the editor import round: three choices, the raw
// expansion fields no longer the author's to fill in), each of NovaWorld's games (ADR 0074; signed out, it says to
// sign in, its Sign in... the NovaWorld window's). A game you own: which of NovaWorld's retail games or expansions
// (an expansion made on its base game's project, found or made first), and the folder; its name is its game's. A
// mod: the NovaWorld game it changes (A mod of), its name and short name (the expansion it builds as, checked as
// the game's rule takes it), the folder. A new game: the title it runs as (A game for), its name and the folder.
// The game install (prefilled with the one the editor last chose, checked as it is named, CheckInstall) is asked
// only where the game needs one, a game you own and a mod of a retail game: the editor's own, where it is that
// game's, said in a line with Change..., else the field, Browse... and what it holds said under it. Set up for a
// NovaWorld project (the projects list's Set up..., ADR 0069), it says so first, with a clear (x), and asks only
// what the project does not say: the folder, and the install by the same rule. Signed in, a mod or a new game may be
// put on NovaWorld as it is made (Put it on NovaWorld, checked: its key, made from its name until one is typed, and
// who gets it, as Put on NovaWorld... asks them). Create raises new_project_request.
// What it holds is the workspace's (new_project, the MCP gaps lane): each field draws it and a person's change is a
// set_workspace, so the editor MCP fills it as a person does; the welcome page and File > New project... draw the
// one form.
class NewProjectForm {
public:
	// True when Create raised the request.
	bool draw(Workspace &workspace);

private:
	ui_kit::HeldText<kWorkspaceText> title_;
	ui_kit::HeldText<kWorkspacePath> folder_;
	ui_kit::HeldText<kWorkspacePath> install_;
	ui_kit::HeldText<kWorkspaceExpansion> name_;
	ui_kit::HeldText<kWorkspaceText> put_key_; // Put it on NovaWorld's key ("" made from the name)
	InstallFieldCheck check_;
	bool changing_install_ = false; // Change... asked: the field in place of the editor's install's line
	bool short_typed_ = false;      // a short name typed: no longer offered from the name
};

// The Document window with no project open, which then has the whole workspace (the other windows stand
// aside: the UX round's project lane): the editor's name and what it is for; the recent projects, each by
// its title with its game, its expansion and its folder (a click opens one, Forget drops it from the list,
// one whose folder no longer holds a project said so); Open a project folder...; NovaWorld's projects, yours and
// everyone's (draw_novaworld_projects); the new-project form; then what last happened.
void draw_welcome(Workspace &workspace, NewProjectForm &form);

// Whether a workspace window (Files, the Inspector, the Preview, Problems, Output) stands aside for the
// welcome page: no project is open and the author has not asked to see it (`asked`, set by its Windows
// menu tick, show_anyway; a project opening forgets it).
bool aside_for_welcome(const SessionView &view, bool &asked);

} // namespace opennova::editor
