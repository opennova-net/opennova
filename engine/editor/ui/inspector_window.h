#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <editor/model/document.h>
#include <editor/session/view/findings_index.h>
#include <editor/session/view/view_revisions.h>
#include <editor/ui/workspace.h>
#include <editor/ui/reference_picker.h>
#include <editor/ui/view_event_mailbox.h>
#include <runtime/devtools/imgui_pass.h>

namespace opennova::editor {

// The generic inspector (ADR 0046 d9, S9h2): renders the selected record of the active
// document from its field schema, whatever the document type and however deep the record
// sits. A breadcrumb (a row that wraps) leads back through the records that hold it, each
// by the name its type shows (Document::record_title). The fields come in groups by the
// first step of their dotted ids (plan_inspector), a block's own yes / no field first, the
// row "In the file"; each shows its readable name (its id in a tooltip) and the
// control its type asks for: a tick box for yes / no, a list of the choices' names, a
// multi-line box for prose, a written / left-out tick for an optional field. A field the
// game ignores on the record is hidden while the file leaves it out and flagged when it
// writes it. A field changed since the last save (S11e), a block's switch among them, is
// marked: a thin bar at its row's left and its name in the change's colour, what the saved
// file holds in its tooltip, Revert to saved on a right click (one undoable batch); its
// section starts open, as one holding something written does.
// References carry a Present / Missing badge, the reference picker (ReferencePicker: style
// variables too where the value may name one; each field's popup its own filter) and "Go to";
// a text reference's value takes a Files row dropped on it when the file fits the field. A field
// whose value is a name other records use (a weapon's, a string's key, a menu's NAME) has
// Rename... (F2), Rename everywhere; typed over while other files still use the saved name, it
// says how many and offers Rename everywhere instead. Each collection is a table, one column per field of its
// records, edited in place (Add / Duplicate / Remove / Up / Down on the selected row, and a
// row's context menu), each row marked when it was added or changed since the last save; a
// collection whose records hold too much for a table lists them by name. The record's
// Problems rows close the panel, each naming the field it is about. A field a request asks
// to show (a RevealRecord view event: a Problems row's, a Go to's) is scrolled to and lit a
// moment, each time it is asked, when the Inspector next draws the record the event names (an
// event held while it does not draw). With several records of one kind selected (S9k2) it
// shows the fields they share instead, each marked where they differ, and a change sets
// every one of them in one undo step.
class InspectorWindow : public devtools::Window {
public:
	explicit InspectorWindow(Workspace &workspace) : workspace_(workspace) { open = true; }
	const char *title() const override { return "Inspector"; }
	devtools::InitialDockPlacement initial_dock_placement() const override { return devtools::InitialDockPlacement::Right; }
	devtools::MenuGroup menu_group() const override { return devtools::MenuGroup::Workspace; }
	void draw(devtools::ImGuiPass &, uint64_t) override;
	// A RevealRecord held until the Inspector draws, with what it was sent against: the
	// selection's revision and the identity of the document it names (Document::identity).
	// Taken, it shows nothing once the selection has moved since (off its record and back too:
	// the view's one reveal went with the first move) or its document was read again, its
	// records numbered anew (closed and opened, reloaded).
	struct HeldReveal {
		ViewEvent event;
		uint64_t selection = 0;
		uint64_t document = 0;
	};
	// A RevealRecord event, held until the Inspector draws.
	void receive(const ViewEvent &event);
	// The events it holds until it draws.
	const ViewEventMailbox<HeldReveal> &events() const { return events_; }

	// How many times "Referenced by" made its lines: once per change of what it reads, never for
	// a frame, a line of Output or a build's step.
	size_t users_made() const { return users_made_; }

private:
	void draw_together(const Document &document, const std::vector<NodeAddress> &records);
	// `others_only`: its own document's uses are listed by its type's part (S15), so the other files'
	// alone.
	void referenced_by(const Document &document, const NodeAddress &record, bool others_only = false);

	Workspace &workspace_;
	ReferencePicker picker_;
	std::string typed_; // what an open list of choices' box holds (field_widgets: one open at a time)
public:
	// The words box of a string id's field (the plain-words lane): what it holds while it is edited, for
	// which field (its document, record and field), and whether it is; one edited at a time.
	struct WordsBox {
		std::string text, key;
		bool editing = false;
	};

private:
	WordsBox words_;
	// "Referenced by": each use of what the selected record defines, its edge and its line, and
	// what they were made from.
	struct Use {
		const GraphEdge *edge = nullptr;
		std::string line;
	};
	struct UsersKey {
		uint64_t document = 0, load = 0, revision = 0;
		NodeAddress record;
		const AssetGraph *graph = nullptr;
		uint64_t generation = 0;
		RevisionKey files;
		bool others_only = false;
		bool operator==(const UsersKey &other) const {
			return document == other.document && load == other.load && revision == other.revision &&
			       record == other.record && graph == other.graph && generation == other.generation &&
			       files == other.files && others_only == other.others_only;
		}
	};
	std::vector<Use> users_;
	UsersKey users_key_;
	size_t users_made_ = 0;
	FindingsIndex findings_; // the record's Problems rows, found without a scan of every finding
	char filter_[128]{};
	// The RevealRecord events held until it draws, then the field the last one asked to show, on
	// its record and in the document it was in (the selection moving off either lets it go);
	// whether the form still has to scroll to it, and when it was asked (its row's light fades
	// from then).
	ViewEventMailbox<HeldReveal> events_;
	uint64_t reveal_document_ = 0;
	NodeAddress reveal_record_;
	std::string reveal_field_;
	bool reveal_scroll_ = false;
	double reveal_time_ = 0.0;
};

} // namespace opennova::editor
