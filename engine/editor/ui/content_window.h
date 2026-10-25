#pragma once

#include <cstdint>
#include <string>

#include <editor/ui/workspace.h>
#include <runtime/devtools/imgui_pass.h>

namespace opennova::editor {

struct ContentView;

// The sign-in to the content store (ADR 0069; session/content_controller.h): the service's address (the one the
// view keeps, until the person types another), the account's name and its password. The password is typed here
// alone: Sign in raises content_sign_in, a person's request no MCP client may send (ServedBy::SessionNeedsPerson),
// and the field is emptied as it goes.
class ContentSignInForm {
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

// The projects the content store lists (the public ones, every one for an admin; a private one says so), each by
// its title with its key and its newest revision, with Get..., which asks the Shell for a folder
// (PickPurpose::ContentFolder; the windows keep the key until the pick comes back): the project is got into a
// folder of its key's name inside the one chosen, and opens. Listed none yet, List asks the store
// (content_check). The welcome page and the Content window draw it.
void draw_content_projects(Workspace &workspace);

// The words the status bar gives the open project's standing in the content store: "ACIDGAMBIT r41", then its
// changes and the revisions it lacks once a check, a pull or a push has counted them ("ACIDGAMBIT r41, 3 changed,
// 2 incoming"); "" for a project the store does not hold.
std::string content_status_text(const ContentView &content);

// The Content window (ADR 0069), a tab beside Files: the account (who is signed in, and where; Sign in... and
// Sign out), then the open project's place in the store. A project the store does not hold says so, and an admin
// binds it to a project of the store (Bind). One it holds shows its revision and the store's newest, Check and
// Pull, its changes since its revision (what each did to the file, and Discard, which puts the revision's file
// back), what kept a file out of them, and for an admin
// a message and Push, which sends every change, and a version and Release rN, which the service builds the
// release of the revision from (ADR 0069 d7). A pull stopped on conflicts lists them, each with Keep mine and
// Take theirs, and Pull goes on once none is left. Its history lists the project's revisions, newest first. Then
// what the last content job came to. With no project open
// it stands aside for the welcome page (which lists the store's projects itself); shown anyway, it lists them.
class ContentWindow : public devtools::Window {
public:
	explicit ContentWindow(Workspace &workspace) : workspace_(workspace) { open = true; }

	const char *title() const override { return "Content"; }
	devtools::InitialDockPlacement initial_dock_placement() const override {
		return devtools::InitialDockPlacement::Left;
	}
	devtools::MenuGroup menu_group() const override { return devtools::MenuGroup::Workspace; }
	bool stands_aside() const override;
	void show_anyway() override { welcome_asked_ = true; }
	bool focus_on_appearing() const override { return false; }
	void draw(devtools::ImGuiPass &pass, uint64_t frame_index) override;

private:
	void draw_account(const ContentView &content);
	void draw_binding(const ContentView &content);
	void draw_bind(const ContentView &content);
	void draw_standing(const ContentView &content);
	void draw_conflicts(const ContentView &content);
	void draw_history(const ContentView &content);

	Workspace &workspace_;
	mutable bool welcome_asked_ = false; // shown with no project open at the author's ask
	ContentSignInForm sign_in_;
	bool signing_in_ = false; // the sign-in form shown
	char message_[2048] = {};  // the push's message
	char version_[33] = {};    // a release's version
	std::string bind_key_;     // the project Bind binds to
};

} // namespace opennova::editor
