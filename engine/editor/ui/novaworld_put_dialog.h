#pragma once

#include <string>

#include <editor/ui/workspace.h>

namespace opennova::editor {

struct NovaWorldView;
struct RemoteProject;
struct SessionView;

// Put on NovaWorld... (ADR 0069; session/novaworld_controller.h's put): a small modal over the window that
// opened it (the NovaWorld window, Project settings), for the open project whose file names no NovaWorld
// project yet. Either a new project of the account's (its name prefilled from the project's title and its key
// made from the name, under Details for one who needs another, a player's up to 11 with no "QQ" as the service takes it; who gets it,
// public first, in the website's words; the kind and base game the session finds as the novaworld_check it raises
// on opening comes back, shown, and for a mod whose base game it finds none of, NovaWorld's games to choose from),
// or an empty project the account may push that the project is (offered only where one is listed: the likeliest
// chosen first, one whose key or name is the project's, else the first). Put raises novaworld_put and the dialog
// stays open while NovaWorld answers: the session writes the key in the project file and it closes, or it says the
// refusal under its fields, which keep what was typed.
class NovaWorldPutDialog {
public:
	// Shown at the next draw, its fields filled from the view as it is then.
	void open() {
		opening_ = true;
		new_here_.clear();
	}
	// The same for the open project's own `key` on a service that shows none of it: a new project under that
	// key, the key itself nothing to ask.
	void open_new_here(const std::string &key) {
		opening_ = true;
		new_here_ = key;
	}
	// Drawn every frame by its owner, in the ID scope it is opened from.
	void draw(Workspace &workspace);

private:
	void fill(const SessionView &view);

	bool opening_ = false;
	std::string new_here_; // the project's own key, put on this service as it is ("" a put of a project with none)
	bool create_ = true;
	char title_[128] = {};
	char key_[17] = {};
	bool key_typed_ = false;
	bool public_ = true;
	std::string base_; // a mod's base game, chosen where the session found none
	std::string lobby_;
	char lobby_name_[65] = {};
	std::string empty_; // the empty project chosen
	bool asked_ = false; // Put pressed: the dialog waits on NovaWorld's answer
};

// A listed project in a person's words: its name; beside another of that name, its owner's too, and only then its
// key. The key is the identity underneath, shown in no list.
std::string novaworld_project_words(const NovaWorldView &novaworld, const RemoteProject &project);

// The key a project named `title` is offered: its letters and digits in capitals, at most 16 ("OP: Acid
// Gambit" "OPACIDGAMBIT"); a `player`'s at most 11, with no "QQ" ("OPACIDGAMBI").
std::string novaworld_key_for(const std::string &title, bool player);

} // namespace opennova::editor
