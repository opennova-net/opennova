#pragma once

#include <cstdint>
#include <string>

#include <editor/session/editor_request.h>
#include <editor/ui/expansion_fields.h>
#include <editor/ui/view_event_mailbox.h>
#include <editor/ui/welcome_view.h>
#include <editor/ui/workspace.h>

namespace opennova::editor {

// File > Project settings... (ADR 0046 S11d): the project's name, features (missions,
// multiplayer) and expansion (S16: what it builds on, whether and as what it builds as an
// expansion, ExpansionFields), then this computer's settings: the game install folder, the OpenNova
// runtime Play runs (read-only on a source run, where Play drives the checkout) and whether
// Play runs the game install instead. The fields are the dialog's until Apply, which raises
// one ApplyProjectSettings naming every one of them: the session writes what differs from
// the settings in effect, so a retry after a partial failure writes what is still not in
// effect, whatever the dialog opened with. The dialog waits for the SettingsApplied view event
// carrying its serial: its flag clear (none failed), it closes; set, it stays open saying what
// failed (the view's settings_result while no later Apply replaced it).
// Cancel changes nothing. It belongs to the project it opened in: another project (or none)
// closes it, and a Browse... answered after that is dropped. The workspace draws it every
// frame.
class ProjectSettingsDialog {
public:
	// Opens on the next draw for the view's project, the settings in effect in the fields.
	void open(const SessionView &view);
	// A SettingsApplied view event, held until the dialog draws (every frame, with the
	// workspace's modals).
	void receive(const ViewEvent &event) { events_.post(event); }
	void draw(Workspace &workspace);
	// The shell's answer to a Browse... of the dialog: taken while the dialog is open on the
	// project it asked in (`project_root`, the project open now) for the field it asked for.
	void set_picked(PickPurpose purpose, const std::string &path, const std::string &project_root);

private:
	struct Fields {
		char title[128] = "";
		bool mission = false;
		bool multiplayer = false;
		char game_install[512] = "";
		char runtime[512] = "";
		bool play_in_install = false;
	};
	void apply(Workspace &workspace);
	void close();

	bool open_ = false;       // open, or opening on the next draw
	bool open_requested_ = false;
	std::string root_;        // the project it is for
	Fields fields_;
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
