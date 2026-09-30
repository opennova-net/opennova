#pragma once
#include <string>
#include <vector>

#include <editor/model/document.h>
#include <editor/session/findings_index.h>
#include <editor/ui/editor_host.h>
#include <editor/ui/reference_picker.h>
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
// to show (the view's reveal_field: a Problems row's) is scrolled to and lit a moment, each
// time it is asked (reveal_serial). With several records of one kind selected (S9k2) it
// shows the fields they share instead, each marked where they differ, and a change sets
// every one of them in one undo step.
class InspectorWindow : public devtools::Window {
public:
	explicit InspectorWindow(EditorHost &host) : host_(host) { open = true; }
	const char *title() const override { return "Inspector"; }
	devtools::InitialDockPlacement initial_dock_placement() const override { return devtools::InitialDockPlacement::Right; }
	devtools::MenuGroup menu_group() const override { return devtools::MenuGroup::Workspace; }
	void draw(devtools::ImGuiPass &, uint64_t) override;

private:
	void draw_together(const Document &document, const std::vector<NodeAddress> &records);

	EditorHost &host_;
	ReferencePicker picker_;
	FindingsIndex findings_; // the record's Problems rows, found without a scan of every finding
	char filter_[128]{};
	// The field the view last asked to show, on its record: the ask's serial and the document
	// it was in (another ask of either shows it again); whether the form still has to scroll to
	// it, and when it was asked (its row's light fades from then).
	uint64_t reveal_serial_ = 0;
	uint64_t reveal_document_ = 0;
	NodeAddress reveal_record_;
	std::string reveal_field_;
	bool reveal_scroll_ = false;
	double reveal_time_ = 0.0;
};

} // namespace opennova::editor
