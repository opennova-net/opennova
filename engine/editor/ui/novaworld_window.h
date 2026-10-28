#pragma once

#include <cstdint>
#include <string>

#include <editor/session/session_operation.h>
#include <editor/ui/novaworld_put_dialog.h>
#include <editor/ui/workspace.h>
#include <runtime/devtools/imgui_pass.h>

namespace opennova::editor {

struct NovaWorldView;

// The sign-in to NovaWorld (ADR 0069; session/novaworld_controller.h): the service's address (the one the
// view keeps, until the person types another), the account's name and its password. The password is typed here
// alone: Sign in raises novaworld_sign_in, a person's request no MCP client may send (ServedBy::SessionNeedsPerson),
// and the field is emptied as it goes.
class NovaWorldSignInForm {
public:
	// True when Sign in raised the request.
	bool draw(Workspace &workspace);

private:
	char server_[256] = {};
	char username_[128] = {};
	char password_[256] = {};
	std::string server_taken_; // the view's address the field last took
	bool server_edited_ = false;
};

// The projects NovaWorld lists (the public ones, the account's own, every one for an admin; a private one says so)
// in two groups, Yours (those the account may push) and Everyone's, each by its title with its key and its newest
// revision. One with a revision has Get..., which asks the Shell for a folder (PickPurpose::NovaWorldFolder; the
// windows keep the key until the pick comes back): the project is got into a folder of its key's name inside the
// one chosen, and opens. An empty one of yours has Set up..., which fills the New project form for it (what it
// makes, its name, the key its project file takes). Listed none yet, List asks the store (novaworld_check). The
// welcome page and the NovaWorld window draw it.
void draw_novaworld_projects(Workspace &workspace);

// Whether an operation of `kind` is NovaWorld's (a check, a working copy's setup, a pull or a get, a push, a mod's
// base game's project made), and what its Cancel leaves behind ("" for another's).
bool novaworld_operation(OperationKind kind);
std::string novaworld_cancel_words(OperationKind kind);

// What NovaWorld is doing now, where the NovaWorld window and the welcome page show it: a NovaWorld operation's
// phase in words ("Downloading 312 of 4,180 files (1.2 GiB of 2.9 GiB)"), a progress bar where it counts, and
// Cancel where it may stop (between files: its tooltip says what is left); else what NovaWorld is asked ("Putting
// MYMOD on NovaWorld..."). The line and the bar change ten times a second at most. Nothing when nothing runs.
void draw_novaworld_activity(Workspace &workspace);

// The words the status bar gives the open project's standing in NovaWorld: "ACIDGAMBIT r41", then its
// changes and the revisions it lacks once a check, a pull or a push has counted them ("ACIDGAMBIT r41, 3 changed,
// 2 incoming"); "" for a folder that is no working copy of a project there.
std::string novaworld_status_text(const NovaWorldView &novaworld);

// The NovaWorld window (ADR 0069; its window id "novaworld"), a tab beside Files: the account (who is signed in, and
// where; Sign in... and Sign out), then the open project's place on NovaWorld. A project its file names no NovaWorld
// project of says so, with Put on NovaWorld... (NovaWorldPutDialog). One whose folder is being set up as its
// working copy says so (or why the service shows it none). A working copy shows its revision and the store's
// newest, Check and Pull, its changes since its revision (what each did to the file, and Discard, which puts the
// revision's file back), what kept a file out of them, and for an account that may push it (the service's word) a
// message ("First revision" before the first) and Push, which sends every change; else a read-only line. For an
// admin, a version and Release rN, which the service builds the release of the revision from (ADR 0069 d7). A pull
// stopped on conflicts lists them, each with Keep mine and Take theirs, and Pull goes on once none is left. Its
// history lists the project's revisions, newest first. Then what the last NovaWorld job came to. With no project
// open it stands aside for the welcome page (which lists the store's projects itself); shown anyway, it lists them.
class NovaWorldWindow : public devtools::Window {
public:
	explicit NovaWorldWindow(Workspace &workspace) : workspace_(workspace) { open = true; }

	const char *title() const override { return "NovaWorld"; }
	devtools::InitialDockPlacement initial_dock_placement() const override {
		return devtools::InitialDockPlacement::Left;
	}
	devtools::MenuGroup menu_group() const override { return devtools::MenuGroup::Workspace; }
	bool stands_aside() const override;
	void show_anyway() override { welcome_asked_ = true; }
	bool focus_on_appearing() const override { return false; }
	// Build > Release...: the Release section's version field brought forward at the next draw.
	void focus_release() { focus_release_ = true; }
	void draw(devtools::ImGuiPass &pass, uint64_t frame_index) override;

private:
	void draw_account(const NovaWorldView &novaworld);
	void draw_not_on_novaworld(const NovaWorldView &novaworld);
	void draw_setting_up(const NovaWorldView &novaworld);
	void draw_standing(const NovaWorldView &novaworld);
	void draw_push(const NovaWorldView &novaworld);
	void draw_release(const NovaWorldView &novaworld);
	void draw_conflicts(const NovaWorldView &novaworld);
	void draw_history(const NovaWorldView &novaworld);

	Workspace &workspace_;
	mutable bool welcome_asked_ = false; // shown with no project open at the author's ask
	NovaWorldSignInForm sign_in_;
	NovaWorldPutDialog put_;
	char message_[2048] = {};  // the push's message
	std::string prefilled_;    // the project the message was last prefilled for ("First revision")
	char version_[33] = {};    // a release's version
	bool focus_release_ = false; // Build > Release... asked: the version field brought forward
};

} // namespace opennova::editor
