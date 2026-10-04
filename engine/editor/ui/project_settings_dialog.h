#pragma once

#include <cstdint>
#include <string>

#include <editor/session/editor_request.h>
#include <editor/ui/expansion_fields.h>
#include <editor/ui/ui_kit.h>
#include <editor/ui/view_event_mailbox.h>
#include <editor/ui/welcome_view.h>
#include <editor/ui/workspace.h>

namespace opennova::editor {

// File > Project settings... (ADR 0046 S11d): the project's name, features (missions,
// multiplayer) and expansion (S16: what it builds on, whether and as what it builds as an
// expansion, ExpansionFields), then this computer's settings: the game install folder, the OpenNova
// runtime Play runs (read-only on a source run, where Play drives the checkout) and whether
// Play runs the game install instead. Whether it is open and what its fields hold until Apply are the
// workspace's (settings, the MCP gaps lane): opening it fills them with the settings in effect, each field
// draws them and a person's change is a set_workspace. Apply raises one ApplyProjectSettings naming every
// one of them: the session writes what differs from the settings in effect, so a retry after a partial
// failure writes what is still not in effect, whatever the dialog opened with. The dialog waits for the
// SettingsApplied view event carrying its serial: its flag clear (none failed), the session has closed it;
// set, it stays open saying what failed (the view's settings_result while no later Apply replaced it).
// Cancel changes nothing. It belongs to the project it opened in: the session closes it with the project, and a Browse...
// answered after that is dropped. The workspace draws it every frame.
class ProjectSettingsDialog {
public:
	// A SettingsApplied view event, held until the dialog draws (every frame, with the
	// workspace's modals).
	void receive(const ViewEvent &event) { events_.post(event); }
	void draw(Workspace &workspace);
	// The shell's answer to a Browse... of the dialog: taken while the dialog is open on the
	// project it asked in (`project_root`, the project open now) for the field it asked for, as the
	// field's set_workspace.
	void set_picked(Workspace &workspace, PickPurpose purpose, const std::string &path, const std::string &project_root);

private:
	void apply(Workspace &workspace);
	void close(Workspace &workspace);

	bool shown_ = false;      // open when it last drew: its waiting and its error are this opening's
	std::string root_;        // the project it is for
	ui_kit::HeldText<128> title_;
	ui_kit::HeldText<512> game_install_;
	ui_kit::HeldText<512> runtime_;
	ExpansionFields expansion_;
	PickPurpose pick_ = PickPurpose::None; // the Browse... the shell is answering
	ViewEventMailbox<> events_;
	uint64_t serial_ = 0;     // the last Apply's, and whether its answer is still to come
	uint64_t seen_ = 0;       // the highest serial an answer carried
	bool waiting_ = false;
	std::string error_;       // what the last Apply could not write
	InstallFieldCheck check_; // the install field's check, its line
};

} // namespace opennova::editor
