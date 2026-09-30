#include "inspector_window.h"

#include <editor/graph/asset_graph.h>
#include <editor/graph/reference_queries.h>
#include <editor/session/view/findings_index.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/field_widgets.h>
#include <editor/model/field_text.h>
#include <editor/ui/inspector_layout.h>
#include <editor/ui/reference_picker.h>
#include <editor/ui/rename_dialog.h>
#include <editor/ui/ui_kit.h>

#include <algorithm>
#include <cfloat>
#include <string>
#include <vector>
#include <imgui.h>

namespace opennova::editor {
namespace {

using window_requests::edit;
using window_requests::go_to;
using window_requests::select;
using RecordChange = Document::RecordChange;

// The records one control edits: the record shown, or every selected record of one kind
// (the first the primary, whose value the control shows). A change to several is one batch,
// one undo step.
using Targets = std::vector<NodeAddress>;

// A collection whose records have at most this many fields is a table edited in place;
// one whose records hold more (a window, a part) lists them by name.
constexpr size_t kTableFields = 12;
constexpr int kTableRows = 8; // the rows a table shows before it scrolls
constexpr double kFlashSeconds = 1.2; // how long a field a request showed keeps its row lit
constexpr size_t kSavedLines = 8; // the records a changed field's tooltip names, of several

const ImVec4 kIgnored(0.95f, 0.75f, 0.35f, 1.0f);
const ImVec4 kMixed(0.60f, 0.75f, 0.95f, 1.0f);
const char *const kIgnoredTip = "The game does not read this here; the file still writes it.";
const char *const kMixedTip = "The selected records differ here; a change sets it on every one.";
const char *const kUnverifiedTip = "Whether the game reads this here is not witnessed yet.";

// A field a request asked to show (a RevealRecord event) on the record it was for: its
// section opened and the form scrolled to it once, its row lit for a moment after.
struct Reveal {
	NodeAddress record;
	std::string field;
	bool scroll = false;
	float light = 0.0f; // the row's highlight, fading to none
};

const Document *active(const SessionView &view) {
	for (const auto &document : view.documents.open)
		if (document->path() == view.documents.active) return records_of(*document);
	return nullptr;
}

// A Set on every target (`coalesce` folds a typing burst into one undo step).
void set(Workspace &workspace, const Document &document, const Targets &targets, const std::string &field, Value value,
         bool coalesce = true) {
	if (targets.size() == 1) return window_requests::set(workspace, document, targets.front(), field, std::move(value), coalesce);
	std::vector<Edit> batch;
	for (const NodeAddress &address : targets) {
		Edit change;
		change.address = address;
		change.field = field;
		change.value = value;
		change.coalesce = coalesce;
		batch.push_back(change);
	}
	window_requests::edits(workspace, document, std::move(batch));
}

// An optional field written or left out on every target that is not so already.
void set_written(Workspace &workspace, const Document &document, const Targets &targets, const std::string &field,
                 bool written) {
	if (targets.size() == 1) return window_requests::set_written(workspace, document, targets.front(), field, written);
	std::vector<Edit> batch;
	for (const NodeAddress &address : targets) {
		if (document.present(address, field) == written) continue;
		Edit change;
		change.operation = written ? EditOperation::Write : EditOperation::Clear;
		change.address = address;
		change.field = field;
		batch.push_back(change);
	}
	if (!batch.empty()) window_requests::edits(workspace, document, std::move(batch));
}

// The fields one row of the form edits: a field alone, or a group's members side by side.
using RowFields = std::vector<const FieldUse *>;

// A row's fields changed since the last save on any of the targets, and the colour the row is
// marked in: the Added green when none of the records they changed on is in the saved file,
// else the Changed amber. Both answers are the document's, cached while its revision stands.
struct FieldChange {
	bool changed = false;
	ImVec4 color;
};

FieldChange field_change(const Document &document, const Targets &targets, const RowFields &fields) {
	FieldChange out;
	bool added = true;
	for (const FieldUse *field : fields)
		for (const NodeAddress &target : targets) {
			if (!document.field_changed(target, field->schema->id)) continue;
			out.changed = true;
			added = added && document.record_change(target) == RecordChange::Added;
		}
	if (out.changed) out.color = ui_kit::change_color(added ? RecordChange::Added : RecordChange::Changed);
	return out;
}

// What the saved file holds of a changed field, for its tooltip (made only while it shows):
// its value, or that the file leaves it out or lacks the record; record by record of several.
std::string saved_words(const Document &document, const Targets &targets, const FieldUse &field) {
	std::string out;
	size_t changed = 0;
	std::vector<FieldChoice> own;
	for (const NodeAddress &target : targets) {
		if (!document.field_changed(target, field.schema->id)) continue;
		++changed;
		Value value;
		bool written = true;
		const std::string saved = !document.saved_value(target, field.schema->id, value, &written) ? "Not in the saved file"
		                          : !written ? "Left out of the saved file"
		                                     : "Saved: " + shown_value(*field.schema, document.choices_on(target, field, own), value);
		if (targets.size() == 1) return saved;
		if (changed <= kSavedLines) out += (out.empty() ? "" : "\n") + document.record_title(target) + ": " + saved;
	}
	if (changed > kSavedLines) out += "\nand " + std::to_string(changed - kSavedLines) + " more";
	return out;
}

// The same of each changed field of a row (a group's by each member's name).
std::string saved_words(const Document &document, const Targets &targets, const RowFields &fields) {
	if (fields.size() == 1) return saved_words(document, targets, *fields.front());
	std::string out;
	for (const FieldUse *field : fields) {
		const std::string saved = saved_words(document, targets, *field);
		if (!saved.empty()) out += (out.empty() ? "" : "\n") + field_title(*field->schema) + ": " + saved;
	}
	return out;
}

// A changed row's menu (a right click on its name): Revert to saved (the RevertToSaved
// request, as the editor MCP raises it), one batch that gives each target its saved value and
// presence back in each of the row's fields, one undo step.
void revert_menu(Workspace &workspace, const Document &document, const Targets &targets, const RowFields &fields) {
	if (!ImGui::BeginPopupContextItem("revert")) return;
	std::vector<std::string> revertible;
	for (const FieldUse *field : fields)
		if (std::any_of(targets.begin(), targets.end(), [&](const NodeAddress &target) {
			    return !document.revert_edits(target, field->schema->id).empty();
		    }))
			revertible.push_back(field->schema->id);
	const bool any = !revertible.empty();
	if (ImGui::MenuItem("Revert to saved", nullptr, false, any) && any)
		window_requests::revert(workspace, document, targets, revertible);
	if (!any) ui_kit::tooltip("The saved file does not have it to go back to.");
	ImGui::EndPopup();
}

// Where the next piece of a reference's tools goes: on the line (`row` null), or on a row
// that wraps under a value too narrow to share its line.
void place(ui_kit::WrapRow *row, float width) {
	if (row) row->next(width);
	else ImGui::SameLine();
}

// Where a Go to leads, in its tooltip: the file opened at the record, or shown in Files.
std::string go_to_words(const std::vector<ReferenceTarget> &targets) {
	if (targets.size() > 1) return "Go to one of the " + std::to_string(targets.size()) + " places it leads.";
	const ReferenceTarget &target = targets.front();
	return target.editable ? "Open " + target.label + "."
	                       : "Show " + target.file + " in Files (the editor does not edit its kind).";
}

// A reference's Go to, to the places the game's lookup reaches (reference_targets,
// made only while the tool is hovered, pressed or its menu open): one gone to, or several
// offered in a menu (a font through a style variable: the variable where the game reads it,
// or the file its value names). `pressed` is the tool's press; its tooltip is `tip`, then
// where it leads after `lead`.
void go_to_tool(Workspace &workspace, const FieldUse &field, const Value &value, bool pressed,
                const std::string &tip, const char *lead) {
	const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
	const SessionView &view = workspace.view();
	if (!view.findings.graph) return;
	if (pressed || hovered) {
		const std::vector<ReferenceTarget> targets =
				reference_targets(*view.findings.graph, *view.project.scan, field, value);
		if (pressed && targets.size() == 1) go_to(workspace, targets.front());
		else if (pressed && !targets.empty()) ImGui::OpenPopup("go to");
		if (hovered)
			ui_kit::tooltip(targets.empty() ? tip : tip + (tip.empty() ? "" : "\n") + lead + go_to_words(targets));
	}
	if (!ImGui::BeginPopup("go to")) return;
	for (const ReferenceTarget &target :
			reference_targets(*view.findings.graph, *view.project.scan, field, value))
		if (ImGui::MenuItem(target.label.c_str())) go_to(workspace, target);
	ImGui::EndPopup();
}

// Present / Missing / Unverified beside a reference, from the same tables the validator
// uses (compact: a coloured dot, the words in its tooltip, a click on it the Go to); a
// reference that resolves gets a "Go to" (go_to_tool) to the record that defines it, this
// document's own included, or the file it loads.
void reference_status(Workspace &workspace, const FieldUse &field, const Value &value, bool compact,
                      ui_kit::WrapRow *row) {
	std::string symbol;
	const SessionView &view = workspace.view();
	const ReferenceStatus status = view.findings.graph
			? editor::reference_status(*view.findings.graph, field, value, &symbol)
			: ReferenceStatus::Unverified;
	if (status == ReferenceStatus::NotAReference) return;
	const ImVec4 colour = ui_kit::reference_color(status);
	const char *word = ui_kit::reference_word(status);
	const bool present = status == ReferenceStatus::Present;
	std::string tip = present                              ? std::string("Found in the project.")
	                  : status == ReferenceStatus::Missing ? "No project file or record is named '" + symbol + "'."
	                                                       : std::string("The editor cannot check this kind of reference yet.");
	if (compact) {
		ImGui::SameLine();
		const float frame = ImGui::GetFrameHeight();
		const ImVec2 at = ImGui::GetCursorScreenPos();
		const bool pressed = ImGui::InvisibleButton("go to dot", ImVec2(frame * 0.5f, frame));
		ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(at.x + frame * 0.25f, at.y + frame * 0.5f), frame * 0.2f,
		                                            ImGui::GetColorU32(colour));
		tip = std::string(word) + ": " + tip;
		if (present) go_to_tool(workspace, field, value, pressed, tip, "A click: ");
		else ui_kit::tooltip(tip);
		return;
	}
	place(row, ui_kit::text_width(word));
	ImGui::TextColored(colour, "%s", word);
	ui_kit::tooltip(tip);
	if (!present) return;
	place(row, ui_kit::button_width("Go to"));
	go_to_tool(workspace, field, value, ImGui::SmallButton("Go to"), std::string(), "");
}

// A field that names something: its badge and its Go to, a number (an item id) as much as a
// text, and a text whose whole %NAME% names the stylesheet variable (value_reference). A text
// reference of the field's own is picked too (the picker's names are texts).
bool is_reference(const FieldUse &field, const Value &value) {
	return value_reference(field, value) != ReferenceKind::None;
}
bool picks_reference(const FieldUse &field) {
	return field.reference != ReferenceKind::None && field.schema->type == FieldType::Text;
}

// The width a reference's tools take beside its value: Pick (a text's), the widest word and
// Go to.
float reference_tools_width(const FieldUse &field) {
	const float gap = ImGui::GetStyle().ItemSpacing.x;
	return (picks_reference(field) ? ui_kit::button_width("Pick") + gap : 0.0f) + ui_kit::text_width("Unverified") +
	       ui_kit::button_width("Go to") + gap * 2.0f;
}

// The picker's button (a text reference's, ReferencePicker: a pick set on every target) and the
// badge after a reference's value: on its line (`beside`), or wrapping under it (a multi-line
// box, a value column too narrow to share).
void reference_tools(Workspace &workspace, ReferencePicker &picker, const Document &document, const Targets &targets,
                     const FieldUse &field, const Value &value, bool compact, bool beside) {
	ui_kit::WrapRow under;
	ui_kit::WrapRow *row = beside || compact ? nullptr : &under;
	if (picks_reference(field)) {
		if (row) row->next(ui_kit::button_width("Pick"));
		else ImGui::SameLine();
		std::string picked;
		if (picker.draw(workspace, document, targets.front(), field, value, compact, picked))
			set(workspace, document, targets, field.schema->id, picked, false);
	}
	reference_status(workspace, field, value, compact, row);
}

// A Files row dropped on a text reference's value: the file set on every target, when it is one
// the field's kind loads.
void drop_target(Workspace &workspace, const Document &document, const Targets &targets, const FieldUse &field) {
	std::string dropped;
	if (picks_reference(field) && !field.read_only && !document.blocked() &&
	    ReferencePicker::accept_file(workspace.view(), field, dropped))
		set(workspace, document, targets, field.schema->id, dropped, false);
}

// A name a field holds, as a rename takes it: a text, or a number (an item's id); "" for none.
std::string name_of(const Value &value) {
	if (const auto *text = std::get_if<std::string>(&value)) return *text;
	if (const auto *number = std::get_if<int64_t>(&value)) return *number ? std::to_string(*number) : std::string();
	return std::string();
}

// Whether a field's value is a name other records use (FieldUse::defines, as it applies to the
// record) that Rename everywhere renames: one record's, a document that takes edits.
bool renames_name(const Document &document, const Targets &targets, const FieldUse &field, const Value &value) {
	return field.defines != ReferenceKind::None && targets.size() == 1 && !document.blocked() && !name_of(value).empty();
}

// Rename everywhere on a record's name (the Inspector's Rename..., F2): the dialog opened on it.
void rename_everywhere(Workspace &workspace, const Document &document, const NodeAddress &address, const FieldUse &field,
                       const Value &value) {
	workspace.request(RenameDialog::preview(document.path(), document.locator(address), field.schema->id, name_of(value), true));
}

// A name typed over (changed since the save) that other files still use by its saved name: they
// no longer reach this record by it, whether they now reach nothing or another definition of the
// name (a stylesheet's fallback once brand.mns renames its own): how many, and Rename everywhere,
// which puts the saved name back and renames it with every use (the dialog opened on the name
// typed).
void rename_hint(Workspace &workspace, const Document &document, const NodeAddress &address, const FieldUse &field,
                 const Value &value) {
	const SessionView &view = workspace.view();
	Value saved;
	if (!view.findings.graph || !document.field_changed(address, field.schema->id) ||
	    !document.saved_value(address, field.schema->id, saved))
		return;
	const std::string old = name_of(saved), now = name_of(value);
	if (old.empty() || now.empty()) return;
	size_t others = 0;
	for (const GraphEdge *edge : view.findings.graph->referrers_of(field.defines, old, field.scope))
		if (edge->source != document.path()) ++others;
	if (!others) return;
	ImGui::PushStyleColor(ImGuiCol_Text, kIgnored);
	ImGui::TextWrapped("%zu use%s in other files still name%s '%s'.", others, others == 1 ? "" : "s", others == 1 ? "s" : "",
	                   old.c_str());
	ImGui::PopStyleColor();
	if (ImGui::SmallButton("Rename everywhere...")) {
		window_requests::revert(workspace, document, {address}, {field.schema->id});
		workspace.request(RenameDialog::preview(document.path(), document.locator(address), field.schema->id, now, true));
	}
	ui_kit::tooltip("Puts '" + old + "' back, then renames it to '" + now + "' with every use of it, in every file (F2).");
}

// The control that edits one value (the caller has set its width): the field's widget
// (field_widgets::value), each change a Set on every target. compact: a table cell (one line
// for text, the bits of a flags field in a popup). mixed: the targets differ, so a text shows
// empty with a hint and a list names no choice; a flag bit changes on each target's own bits.
void value_control(Workspace &workspace, const Document &document, const Targets &targets, const FieldUse &field,
                   const Value &value, bool compact, bool mixed = false) {
	// The choices it offers: the schema's, or the primary target's own (Document::choices_on).
	std::vector<FieldChoice> offered;
	const std::vector<FieldChoice> &choices = document.choices_on(targets.front(), field, offered);
	const FieldSchema &schema = *field.schema;
	if (schema.flags) {
		const int64_t bits = std::get<int64_t>(value);
		auto boxes = [&] {
			for (const FieldChoice &choice : choices) {
				bool checked = (bits & choice.value) != 0;
				// Its name cut to the column (whole in its tooltip), its id the name.
				const std::string &title = choice_title(choice);
				const float room = ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - ImGui::GetStyle().ItemInnerSpacing.x;
				const std::string shown = compact ? title : ui_kit::fit(title, room);
				if (ImGui::Checkbox((shown + "###" + title).c_str(), &checked)) {
					// Each target keeps its other bits.
					std::vector<Edit> batch =
					        flag_bit_edits(document, targets, schema, choice.value, checked);
					if (batch.size() == 1) window_requests::set(workspace, document, targets.front(), schema.id, batch.front().value, false);
					else window_requests::edits(workspace, document, std::move(batch));
				}
				if (!choice.label.empty() || shown != title) ui_kit::tooltip(shown != title ? title + "\n" + choice.name : choice.name);
			}
		};
		if (!compact) return boxes();
		if (ImGui::SmallButton("Bits...")) ImGui::OpenPopup("bits");
		if (ImGui::BeginPopup("bits")) {
			boxes();
			ImGui::EndPopup();
		}
		return;
	}
	Value edited = value;
	const field_widgets::Edited change = field_widgets::value(field, choices, edited, compact, mixed);
	if (change.changed) set(workspace, document, targets, schema.id, std::move(edited), change.coalesce);
	if (change.finished) window_requests::end_edit(workspace, document.path());
}

// The optional field's written / left-out tick.
void written_tick(Workspace &workspace, const Document &document, const Targets &targets, const FieldUse &field,
                  bool present) {
	bool on = present;
	if (ImGui::Checkbox("##written", &on)) set_written(workspace, document, targets, field.schema->id, on);
	ui_kit::tooltip(present ? "Written to the file. Untick to leave it out." : "Left out of the file. Tick to write it.");
}

// What the tooltip of a row's name says of its fields: the schema's words of a field alone
// (field_widgets::field_tip); a group's members by name and id, and their unit.
std::string row_tip(const RowFields &fields) {
	if (fields.size() == 1) return field_widgets::field_tip(*fields.front()->schema);
	std::string tip;
	for (const FieldUse *field : fields)
		tip += (tip.empty() ? "" : "\n") + field_title(*field->schema) + ": " + field->schema->id;
	if (!fields.front()->schema->unit.empty()) tip += "\nIn " + fields.front()->schema->unit;
	return tip;
}

// A row's name, cut to its cell, and the tags after it: (mixed) where the targets differ,
// (ignored) where the game reads none of its fields here or (?) where it may not read one
// (in a cell too narrow for them, their words move into the tooltip, which says what the
// schema says of its fields (row_tip) after `about`, what the row is, when given). A row
// whose fields changed since the last save is marked: a thin bar at the row's left edge, the
// name in the change's colour, what the saved file holds in the tooltip (made only while it
// shows), and Revert to saved on a right click.
void field_name(Workspace &workspace, const Document &document, const Targets &targets, const RowFields &fields,
                const std::string &title, const char *about, bool mixed, const FieldChange &change, float left) {
	struct Tag {
		const char *text;
		ImVec4 color;
		const char *tip;
	};
	std::vector<Tag> tags;
	if (mixed) tags.push_back({"(mixed)", kMixed, kMixedTip});
	const auto applies = [&](Applicability which) {
		return [which](const FieldUse *field) { return field->applies == which; };
	};
	if (std::all_of(fields.begin(), fields.end(), applies(Applicability::Ignored)))
		tags.push_back({"(ignored)", kIgnored, kIgnoredTip});
	else if (std::any_of(fields.begin(), fields.end(), applies(Applicability::Unverified)))
		tags.push_back({"(?)", ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), kUnverifiedTip});
	float tags_width = 0.0f;
	for (const Tag &tag : tags) tags_width += ImGui::GetStyle().ItemSpacing.x + ui_kit::text_width(tag.text);
	const float room = ImGui::GetContentRegionAvail().x;
	const bool tagged = room - tags_width >= ImGui::GetFontSize() * 3.0f;
	const std::string shown = ui_kit::fit(title, tagged ? room - tags_width : room);
	if (change.changed) ImGui::TextColored(change.color, "%s", shown.c_str());
	else ImGui::TextUnformatted(shown.c_str());
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
		std::string tip = about ? std::string(about) + "\n" : std::string();
		tip += (shown != title ? title + "\n" : std::string()) + row_tip(fields);
		if (!tagged)
			for (const Tag &tag : tags) tip += std::string("\n") + tag.tip;
		if (change.changed) tip += "\n" + saved_words(document, targets, fields);
		ui_kit::tooltip(tip);
	}
	if (change.changed) {
		const float top = ImGui::GetItemRectMin().y - ImGui::GetStyle().FramePadding.y;
		const float bar = std::max(2.0f, ImGui::GetStyle().CellPadding.x - 1.0f);
		ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(left, top), ImVec2(left + bar, top + ImGui::GetFrameHeight()),
		                                          ImGui::GetColorU32(change.color));
		revert_menu(workspace, document, targets, fields);
	}
	if (!tagged) return;
	for (const Tag &tag : tags) {
		ImGui::SameLine();
		ImGui::TextColored(tag.color, "%s", tag.text);
		ui_kit::tooltip(tag.tip);
	}
}

// A form row's first cell begun: the row lit a moment while a request shows one of its fields
// (scrolled to once). Its left edge: the cell's content start less its padding.
float begin_row(Reveal *reveal, const NodeAddress &address, const RowFields &fields) {
	ImGui::TableNextRow();
	ImGui::TableNextColumn();
	if (reveal && reveal->record == address &&
	    std::any_of(fields.begin(), fields.end(), [&](const FieldUse *field) { return field->schema->id == reveal->field; })) {
		if (reveal->scroll) ImGui::SetScrollHereY(0.5f);
		reveal->scroll = false;
		if (reveal->light > 0.0f)
			ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, ImGui::GetColorU32(ImGuiCol_HeaderActive, reveal->light));
	}
	return ImGui::GetCursorScreenPos().x - ImGui::GetStyle().CellPadding.x;
}

// One field of the form: its name (the optional tick before it, marked where it changed
// since the last save) and its control, which shows the primary target's value and sets
// every target. The field a request asked to show is scrolled to once and lit a moment.
// `block_switch`: a block's own yes / no field (plan_inspector's toggle), the first row of its
// section, named "In the file" (short enough for a narrow column) and saying what its tick
// does.
void field_row(Workspace &workspace, ReferencePicker &picker, const Document &document, const Targets &targets,
               const FieldUse &field, Reveal *reveal, bool block_switch = false) {
	const FieldSchema &schema = *field.schema;
	const NodeAddress &address = targets.front();
	Value value;
	if (!document.get(address, schema.id, value)) return;
	const bool present = !schema.optional || document.present(address, schema.id);
	const bool mixed = field_mixed(document, targets, schema.id);
	const RowFields fields{&field};
	const float left = begin_row(reveal, address, fields);
	ImGui::PushID(schema.id.c_str());
	ImGui::BeginDisabled(field.read_only || document.blocked());
	if (schema.optional) {
		written_tick(workspace, document, targets, field, present);
		ImGui::SameLine();
	}
	const char *about = nullptr; // what a block's switch does, for its tooltips
	if (block_switch) {
		const auto *on = std::get_if<int64_t>(&value);
		about = on && *on != 0 ? "Untick to leave the whole block out of the file; what it holds is kept."
		                       : "Left out of the file. Tick to write the block again.";
	}
	ImGui::AlignTextToFramePadding();
	field_name(workspace, document, targets, fields, block_switch ? std::string("In the file") : field_title(schema), about,
	           mixed, field_change(document, targets, fields), left);
	ImGui::TableNextColumn();
	// A reference's tools, and a name's Rename..., share the value's line while it keeps a few
	// words' room.
	const bool renames = present && renames_name(document, targets, field, value);
	const float rename_width = renames ? ui_kit::button_width("Rename...") + ImGui::GetStyle().ItemSpacing.x : 0.0f;
	const float tools = (is_reference(field, value) ? reference_tools_width(field) : 0.0f) + rename_width;
	const bool beside = (is_reference(field, value) || renames) && !schema.multiline &&
	                    ImGui::GetContentRegionAvail().x - tools >= ImGui::GetFontSize() * 6.0f;
	ImGui::BeginDisabled(!present);
	ImGui::SetNextItemWidth(beside ? -tools : -FLT_MIN);
	value_control(workspace, document, targets, field, value, false, mixed);
	if (about) ui_kit::tooltip(about);
	if (present) drop_target(workspace, document, targets, field);
	ImGui::EndDisabled();
	if (is_reference(field, value) && present)
		reference_tools(workspace, picker, document, targets, field, value, false, beside);
	if (renames) {
		if (beside) ImGui::SameLine();
		if (ImGui::SmallButton("Rename...")) rename_everywhere(workspace, document, address, field, value);
		ui_kit::tooltip("Rename everywhere (F2): this name and every use of it, in every file.");
		rename_hint(workspace, document, address, field, value);
	}
	ImGui::EndDisabled();
	ImGui::PopID();
}

// A group's fields on one row (field_widgets::group): its name the group's, marked, reverted
// and revealed as one field's is (any member), then the members side by side, each change a
// Set on every target; a swatch's pick of every channel one batch, one undo step.
void group_row(Workspace &workspace, const Document &document, const Targets &targets, const RowFields &fields,
               Reveal *reveal) {
	const NodeAddress &address = targets.front();
	std::vector<FieldUse> members;
	std::vector<Value> values(fields.size());
	std::vector<bool> mixed(fields.size());
	// Each member's choices as they apply to the record (its own list where it has one).
	std::vector<std::vector<FieldChoice>> own(fields.size());
	std::vector<const std::vector<FieldChoice> *> choices(fields.size());
	for (size_t i = 0; i < fields.size(); ++i) {
		if (!document.get(address, fields[i]->schema->id, values[i])) return;
		members.push_back(*fields[i]);
		choices[i] = &document.choices_on(address, *fields[i], own[i]);
		mixed[i] = field_mixed(document, targets, fields[i]->schema->id);
	}
	const float left = begin_row(reveal, address, fields);
	const std::string &title = fields.front()->schema->group;
	ImGui::PushID(title.c_str());
	ImGui::BeginDisabled(document.blocked());
	ImGui::AlignTextToFramePadding();
	field_name(workspace, document, targets, fields, title, nullptr, std::find(mixed.begin(), mixed.end(), true) != mixed.end(),
	           field_change(document, targets, fields), left);
	ImGui::TableNextColumn();
	ImGui::SetNextItemWidth(-FLT_MIN);
	size_t changed = SIZE_MAX;
	const field_widgets::Edited change =
			field_widgets::group(members, choices, values, changed, mixed);
	std::vector<Edit> batch;
	for (size_t i = 0; change.changed && i < fields.size(); ++i)
		for (const NodeAddress &target : targets) {
			if (changed != SIZE_MAX && changed != i) continue;
			Edit edit;
			edit.address = target;
			edit.field = fields[i]->schema->id;
			edit.value = values[i];
			edit.coalesce = change.coalesce;
			batch.push_back(std::move(edit));
		}
	if (batch.size() == 1) window_requests::set(workspace, document, targets.front(), batch.front().field, batch.front().value, change.coalesce);
	else if (!batch.empty()) window_requests::edits(workspace, document, std::move(batch));
	if (change.finished) window_requests::end_edit(workspace, document.path());
	ImGui::EndDisabled();
	ImGui::PopID();
}

// A section's fields, row by row: a group's neighbours (none left out of the file: each keeps
// its own written tick) on one row, the others alone; the field `block_switch` names drawn as
// its block's switch.
void field_rows(Workspace &workspace, ReferencePicker &picker, const Document &document, const Targets &targets,
                const std::vector<FieldUse> &fields, Reveal *reveal, const std::string &block_switch = std::string()) {
	auto grouped = [](const FieldUse &field) { return !field.schema->group.empty() && !field.schema->optional; };
	for (size_t i = 0; i < fields.size();) {
		size_t end = i + 1;
		if (grouped(fields[i]))
			while (end < fields.size() && grouped(fields[end]) && fields[end].schema->group == fields[i].schema->group) ++end;
		if (end - i > 1) {
			RowFields row;
			for (size_t j = i; j < end; ++j) row.push_back(&fields[j]);
			group_row(workspace, document, targets, row, reveal);
		} else {
			field_row(workspace, picker, document, targets, fields[i], reveal,
			          !block_switch.empty() && fields[i].schema->id == block_switch);
		}
		i = end;
	}
}

// One cell of a collection's table: the field as it applies to that record, "-" where the
// game does not read it and the file leaves it out.
void field_cell(Workspace &workspace, ReferencePicker &picker, const Document &document, const NodeAddress &address,
                const FieldSchema &schema) {
	Value value;
	if (!document.get(address, schema.id, value)) {
		ImGui::TextDisabled("-");
		return;
	}
	const FieldUse field = document.field_on(address, schema);
	const bool present = !schema.optional || document.present(address, schema.id);
	const bool ignored = field.applies == Applicability::Ignored;
	if (ignored && !written(document, address, schema)) {
		ImGui::TextDisabled("-");
		ui_kit::tooltip("The game does not read this for this record.");
		return;
	}
	ImGui::PushID(schema.id.c_str());
	ImGui::BeginDisabled(field.read_only || document.blocked());
	if (schema.optional) {
		written_tick(workspace, document, {address}, field, present);
		ImGui::SameLine(0.0f, 2.0f);
	}
	const ImGuiStyle &style = ImGui::GetStyle();
	float reserve = 0.0f;
	if (is_reference(field, value))
		reserve += (picks_reference(field) ? ui_kit::button_width("...") + style.ItemSpacing.x : 0.0f) +
		           ImGui::GetFrameHeight() * 0.5f + style.ItemSpacing.x;
	if (ignored) reserve += ui_kit::text_width("!") + style.ItemSpacing.x;
	ImGui::BeginDisabled(!present);
	ImGui::SetNextItemWidth(reserve > 0.0f ? -reserve : -FLT_MIN);
	value_control(workspace, document, {address}, field, value, true);
	if (present) drop_target(workspace, document, {address}, field);
	ImGui::EndDisabled();
	if (is_reference(field, value) && present)
		reference_tools(workspace, picker, document, {address}, field, value, true, true);
	if (ignored) {
		ImGui::SameLine();
		ImGui::TextColored(kIgnored, "!");
		ui_kit::tooltip(kIgnoredTip);
	}
	ImGui::EndDisabled();
	ImGui::PopID();
}

// A column's first width, in the font's size: a switch two frames wide, a list's name, a
// text, a number; a written tick, a colour's swatch and a reference's tools on top.
float column_width(const FieldSchema &field) {
	const float frame = ImGui::GetFrameHeight();
	const float em = ImGui::GetFontSize();
	if (is_yes_no(field)) return frame * 2.0f;
	float width = field.flags || !field.choices.empty() ? em * 11.5f : field.type == FieldType::Text ? em * 13.0f : em * 7.0f;
	if (field.optional) width += frame;
	if (field.color == FieldColor::HexArgb || field.color == FieldColor::PackedRgb) width += field_widgets::swatch_width();
	if (field.reference != ReferenceKind::None) width += em * 3.0f;
	return width;
}

// A row's context menu: the collection's structural edits without selecting the row.
void row_menu(Workspace &workspace, const Document &document, const Document::CollectionSpec &spec,
              const NodeAddress &address, size_t index, size_t count) {
	if (spec.fixed || document.blocked() || !ImGui::BeginPopupContextItem("row")) return;
	if (ImGui::MenuItem("Duplicate", nullptr, false, !spec.max || count < spec.max))
		edit(workspace, document, EditOperation::Duplicate, address, index + 1);
	if (ImGui::MenuItem("Remove")) edit(workspace, document, EditOperation::Remove, address);
	if (ImGui::MenuItem("Move up", nullptr, false, index > 0)) edit(workspace, document, EditOperation::Move, address, index - 1);
	if (ImGui::MenuItem("Move down", nullptr, false, index + 1 < count))
		edit(workspace, document, EditOperation::Move, address, index + 1);
	ImGui::EndPopup();
}

void select_row(Workspace &workspace, const Document &document, const NodeAddress &address) {
	select(workspace, document, address, ImGui::GetIO().KeyCtrl ? SelectMode::Toggle : SelectMode::Replace);
}

// A record's tooltip in a collection: its name (and the token its type words, when it does),
// and whether it changed since the last save.
std::string record_tip(const Document &document, const NodeAddress &address) {
	const std::string title = document.record_title(address), name = document.record_name(address);
	const char *change = ui_kit::change_words(document.record_change(address));
	return title + (name != title ? "\n" + name : std::string()) + (*change ? std::string("\n") + change : std::string());
}

// The records as a table: a numbered row each (a click selects it; marked when it changed
// since the last save), a column per field, every cell edited in place. The columns size
// to the font, resize, and scroll sideways past the table's width.
void records_table(Workspace &workspace, ReferencePicker &picker, const Document &document, const NodeAddress &owner,
                   const Document::Collection &records, const std::vector<FieldSchema> &fields) {
	const ImGuiStyle &style = ImGui::GetStyle();
	const float row = ImGui::GetFrameHeight() + style.CellPadding.y * 2.0f;
	const int count = static_cast<int>(records.ids.size());
	const float height = row * float(std::min(count, kTableRows) + 1) + style.ScrollbarSize + style.CellPadding.y * 2.0f;
	const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit |
	                              ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
	if (!ImGui::BeginTable("records", static_cast<int>(fields.size()) + 1, flags, ImVec2(0.0f, height))) return;
	ImGui::TableSetupScrollFreeze(1, 1);
	ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed,
	                        ui_kit::text_width(ui_kit::kChangeRoom) + ui_kit::text_width("000") + style.FramePadding.x * 2.0f);
	for (const FieldSchema &field : fields)
		ImGui::TableSetupColumn(field_widgets::column_header(field).c_str(), ImGuiTableColumnFlags_WidthFixed,
		                        column_width(field));
	ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
	// Each heading under its column's id, as TableHeadersRow scopes them: two fields may share a
	// heading's words.
	for (int column = 0; column <= static_cast<int>(fields.size()); ++column) {
		ImGui::TableSetColumnIndex(column);
		ImGui::PushID(column);
		ImGui::TableHeader(ImGui::TableGetColumnName(column));
		const auto tip = [&] { return field_widgets::field_tip(fields[size_t(column) - 1]); };
		if (column) ui_kit::tooltip_lazy(tip);
		ImGui::PopID();
	}
	ImGuiListClipper clipper;
	clipper.Begin(count, row);
	while (clipper.Step()) {
		for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
			const NodeAddress address{owner.row, records.spec.kind, records.ids[size_t(i)]};
			ImGui::TableNextRow(ImGuiTableRowFlags_None, row);
			ImGui::PushID(static_cast<int>(address.child));
			ImGui::TableNextColumn();
			const bool on = holds(workspace.view().documents.selected, address);
			if (on) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, ImGui::GetColorU32(ImGuiCol_Header));
			const float x = ImGui::GetCursorScreenPos().x;
			const std::string number = ui_kit::kChangeRoom + std::to_string(i + 1);
			if (ImGui::Selectable(number.c_str(), on, ImGuiSelectableFlags_None, ImVec2(0.0f, ImGui::GetFrameHeight())))
				select_row(workspace, document, address);
			ui_kit::change_dot(document.record_change(address), x);
			ui_kit::tooltip_lazy([&] { return record_tip(document, address); });
			row_menu(workspace, document, records.spec, address, size_t(i), records.ids.size());
			for (const FieldSchema &field : fields) {
				ImGui::TableNextColumn();
				field_cell(workspace, picker, document, address, field);
			}
			ImGui::PopID();
		}
	}
	ImGui::EndTable();
}

// The records by name (a window, a part, a table row), each marked when it changed since the
// last save: a click selects one, whose own fields and lists the inspector then shows.
void records_list(Workspace &workspace, const Document &document, const NodeAddress &owner,
                  const Document::Collection &records) {
	for (size_t i = 0; i < records.ids.size(); ++i) {
		const NodeAddress address{owner.row, records.spec.kind, records.ids[i]};
		ImGui::PushID(static_cast<int>(address.child));
		const float x = ImGui::GetCursorScreenPos().x;
		const std::string name =
		        ui_kit::fit(ui_kit::kChangeRoom + std::to_string(i + 1) + ". " + document.record_title(address),
		                    ImGui::GetContentRegionAvail().x);
		if (ImGui::Selectable((name + "###record").c_str(), holds(workspace.view().documents.selected, address))) select_row(workspace, document, address);
		ui_kit::change_dot(document.record_change(address), x);
		ui_kit::tooltip_lazy([&] { return record_tip(document, address); });
		row_menu(workspace, document, records.spec, address, i, records.ids.size());
		ImGui::PopID();
	}
}

// One collection of `owner`: its tools (Add, and Duplicate / Remove / Up / Down for its
// selected record), then its records. `titled`: under a heading of its own inside a group.
void collection_block(Workspace &workspace, ReferencePicker &picker, const Document &document, const NodeAddress &owner,
                      const Document::Collection &records, bool titled) {
	const Document::CollectionSpec &spec = records.spec;
	const std::vector<NodeId> &ids = records.ids;
	const char *token = document.kind_token(spec.kind);
	ImGui::PushID(*token ? token : spec.label);
	if (titled) {
		const std::string heading = std::string(spec.label) + " (" + std::to_string(ids.size()) + ")";
		const float padding = ImGui::GetStyle().SeparatorTextPadding.x * 2.0f;
		ImGui::SeparatorText(ui_kit::fit(heading, ImGui::GetContentRegionAvail().x - padding).c_str());
		ui_kit::tooltip(heading + "\n" + token);
	}
	if (spec.applies == Applicability::Ignored) {
		ImGui::PushStyleColor(ImGuiCol_Text, kIgnored);
		ImGui::TextWrapped("The game does not read these here.");
		ImGui::PopStyleColor();
	}
	size_t selected = SIZE_MAX;
	for (size_t i = 0; i < ids.size(); ++i)
		if (workspace.view().documents.selection.kind == spec.kind && workspace.view().documents.selection.child == ids[i]) selected = i;
	if (!spec.fixed) {
		ImGui::BeginDisabled(document.blocked());
		ui_kit::WrapRow row;
		ui_kit::RowTools tools;
		tools.count = ids.size();
		tools.selected = selected;
		tools.max = spec.max;
		tools.pick = "Select a row (its number) first.";
		tools.small = true;
		const NodeAddress address = selected == SIZE_MAX ? NodeAddress() : NodeAddress{owner.row, spec.kind, ids[selected]};
		switch (ui_kit::row_tools(row, tools)) {
		case ui_kit::RowTool::Add: edit(workspace, document, EditOperation::Add, {owner.row, spec.kind, 0}, SIZE_MAX, owner.child); break;
		case ui_kit::RowTool::Duplicate: edit(workspace, document, EditOperation::Duplicate, address, selected + 1); break;
		case ui_kit::RowTool::Remove: edit(workspace, document, EditOperation::Remove, address); break;
		case ui_kit::RowTool::Up: edit(workspace, document, EditOperation::Move, address, selected - 1); break;
		case ui_kit::RowTool::Down: edit(workspace, document, EditOperation::Move, address, selected + 1); break;
		case ui_kit::RowTool::None: break;
		}
		ImGui::EndDisabled();
	}
	if (ids.empty()) {
		ui_kit::empty_state("None yet.");
	} else {
		const std::vector<FieldSchema> &fields = document.fields(spec.kind);
		if (!fields.empty() && fields.size() <= kTableFields) records_table(workspace, picker, document, owner, records, fields);
		else records_list(workspace, document, owner, records);
	}
	ImGui::PopID();
}

bool holds_field(const InspectorSection &section, const std::string &field) {
	return std::any_of(section.fields.begin(), section.fields.end(), [&](const FieldUse &f) { return f.schema->id == field; }) ||
	       (section.has_toggle && section.toggle.schema->id == field);
}

// Whether a field of the section (its block's switch among them) changed since the last save
// on any of the targets: such a section starts open as a written one does, so a block just
// left out keeps its switch, marked, in sight.
bool section_changed(const Document &document, const Targets &targets, const InspectorSection &section) {
	for (const NodeAddress &target : targets) {
		if (section.has_toggle && document.field_changed(target, section.toggle.schema->id)) return true;
		for (const FieldUse &field : section.fields)
			if (document.field_changed(target, field.schema->id)) return true;
	}
	return false;
}

// One section of the plan: a heading (none for the general fields), then the form: the
// block's own switch first, its fields after; then the collections it claims. A section
// that holds something written, or a field changed since the last save, starts open; one
// holding the field a request asks to show opens.
void draw_section(Workspace &workspace, ReferencePicker &picker, const Document &document, const NodeAddress &record,
                  const NodeAddress &owner, const InspectorSection &section, Reveal *reveal) {
	if (!section.key.empty()) {
		std::string heading = section.title;
		if (!section.has_toggle && section.fields.empty() && section.collections.size() == 1)
			heading += " (" + std::to_string(section.collections.front().ids.size()) + ")";
		heading += "###" + section.key;
		if (reveal && reveal->scroll && reveal->record == record && holds_field(section, reveal->field))
			ImGui::SetNextItemOpen(true);
		const bool shown = section.written || section_changed(document, {record}, section);
		const bool open = ImGui::CollapsingHeader(heading.c_str(), shown ? ImGuiTreeNodeFlags_DefaultOpen : 0);
		ui_kit::tooltip(section.key);
		if (!open) return;
	}
	ImGui::PushID(section.key.c_str());
	// A part's switch waits until the record holds the part (its collection's Add makes one).
	bool part_missing = false;
	for (const Document::Collection &collection : section.collections)
		part_missing = part_missing || (section.key == document.kind_token(collection.spec.kind) &&
		                                collection.ids.empty());
	const bool block_switch = section.has_toggle && !part_missing;
	if ((block_switch || !section.fields.empty()) && ImGui::BeginTable("fields", 2, ImGuiTableFlags_SizingStretchProp)) {
		ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 0.4f);
		ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.6f);
		// The switch is a field like the others: marked, reverted and shown as they are.
		if (block_switch) field_row(workspace, picker, document, {record}, section.toggle, reveal, true);
		field_rows(workspace, picker, document, {record}, section.fields, reveal);
		ImGui::EndTable();
	}
	for (const Document::Collection &collection : section.collections)
		collection_block(workspace, picker, document, owner, collection,
		                 section.key != document.kind_token(collection.spec.kind));
	ImGui::PopID();
}

// Who names this record: the graph's referrers of every symbol it defines (a row's, or a
// nested record's, a menu window's NAME), a line each cut to the window: the file, the
// record and the field's name (the whole of it, and the field's id, in its tooltip); a click
// goes to the use (graph/reference_queries' usage_target).
void referenced_by(Workspace &workspace, const Document &document, const NodeAddress &record) {
	const SessionView &view = workspace.view();
	if (!view.findings.graph) return;
	std::vector<const GraphEdge *> users;
	for (const GraphSymbol *symbol : view.findings.graph->symbols_of(document.path(), document.record_path(record))) {
		if (symbol->inert) continue; // not what the game reads: nothing names this one
		if (symbol->address.row && symbol->address != record) continue; // another record of the same path
		for (const GraphEdge *edge : view.findings.graph->referrers_of(symbol->kind, symbol->name, symbol->scope)) users.push_back(edge);
	}
	if (users.empty()) return;
	ImGui::Separator();
	ImGui::Text("Referenced by %zu field(s)", users.size());
	for (size_t i = 0; i < users.size(); ++i) {
		const GraphEdge &edge = *users[i];
		ImGui::PushID(static_cast<int>(i));
		const std::string field = edge_field_title(view, edge);
		const std::string line = edge.source + ": " + (edge.record.empty() ? field : edge.record + " - " + field);
		const std::string shown = ui_kit::fit(line, ImGui::GetContentRegionAvail().x);
		const bool pressed = ImGui::Selectable((shown + "###use").c_str());
		if (pressed || ImGui::IsItemHovered()) {
			const ReferenceTarget target = usage_target(*view.project.scan, edge);
			if (pressed) go_to(workspace, target);
			ui_kit::tooltip(line + "\n" + edge.field + "\n" + go_to_words({target}));
		}
		ImGui::PopID();
	}
}

// The row and every record that holds the selection, each one click away, on a row that
// wraps; a long name cut to the window (whole in its tooltip, with the token its type words).
void breadcrumb(Workspace &workspace, const Document &document, const NodeAddress &selection) {
	std::vector<NodeAddress> chain = document.ancestors(selection);
	chain.push_back(selection);
	const float room = ImGui::GetContentRegionAvail().x;
	ui_kit::WrapRow row;
	for (size_t i = 0; i < chain.size(); ++i) {
		ImGui::PushID(int(i));
		if (i) {
			row.next(ui_kit::text_width("/"));
			ImGui::TextDisabled("/");
		}
		const std::string title = document.record_title(chain[i]), name = document.record_name(chain[i]);
		const bool last = i + 1 == chain.size();
		const std::string shown = ui_kit::fit(title, room - (last ? 0.0f : ImGui::GetStyle().FramePadding.x * 2.0f));
		row.next(last ? ui_kit::text_width(shown.c_str()) : ui_kit::button_width(shown.c_str()));
		if (last) ImGui::TextUnformatted(shown.c_str());
		else if (ImGui::SmallButton((shown + "###crumb").c_str())) select(workspace, document, chain[i]);
		std::string tip = document.kind_label(chain[i].kind);
		if (shown != title) tip += "\n" + title;
		if (name != title) tip += "\n" + name;
		ui_kit::tooltip(tip);
		ImGui::PopID();
	}
}

// The record's findings (the view's index of them, not a scan of every one), each a line: its
// severity, and its message after the name of the field it is about when it names one.
void findings(const SessionView &view, const FindingsIndex &index, const Document &document,
              const Node &row, const NodeAddress &selection) {
	for (const size_t i : index.of_record(document.path(), row.id, selection.child)) {
		const Diagnostic &d = view.findings.diagnostics[i];
		ui_kit::severity_marker(d.severity);
		ImGui::SameLine();
		ImGui::AlignTextToFramePadding();
		const NodeKind kind = d.record_kind ? d.record_kind : selection.kind;
		ImGui::TextWrapped("%s", d.field.empty() ? d.message.c_str()
		                                         : (field_title(document, kind, d.field) + ": " + d.message).c_str());
		if (!d.field.empty()) ui_kit::tooltip(d.field);
	}
}

} // namespace

void InspectorWindow::receive(const ViewEvent &event) {
	const SessionView &view = workspace_.view();
	HeldReveal held;
	held.event = event;
	held.selection = view.revisions.of(ViewConcern::Selection);
	for (const std::shared_ptr<const DocumentBase> &document : view.documents.open)
		if (document->path() == event.path) held.document = document->identity();
	events_.post(std::move(held));
}

void InspectorWindow::draw(devtools::ImGuiPass &, uint64_t) {
	const SessionView &view = workspace_.view();
	// The fields asked to show since the Inspector last drew, each taken now (read below).
	const std::vector<HeldReveal> reveals = events_.take();
	const Document *document = active(view);
	if (!document || !view.documents.selection.row) {
		ui_kit::empty_state("Select a record.", "Its fields and lists show here.");
		return;
	}
	const Node *row = document->row(view.documents.selection.row);
	if (!row) {
		ui_kit::empty_state("The selected record was removed.");
		return;
	}
	const NodeAddress selection = view.documents.selection;
	// A field a request asks to show (a Problems row's): each ask on the record selected now, in
	// this document, shown once, the filter cleared so nothing hides it; the same row clicked
	// again is another ask, shown again. An ask about a record no longer selected shows nothing,
	// nor one the selection moved from since it was sent (and back) or whose document was read
	// again; the selection moving off the record shown lets its field go.
	const uint64_t selected = view.revisions.of(ViewConcern::Selection);
	for (const HeldReveal &held : reveals) {
		const ViewEvent &reveal = held.event;
		if (reveal.kind != ViewEventKind::RevealRecord || reveal.path != document->path() ||
				reveal.address != selection || reveal.field.empty() ||
				held.selection != selected || held.document != document->identity())
			continue;
		reveal_document_ = document->identity();
		reveal_field_ = reveal.field;
		reveal_record_ = selection;
		reveal_scroll_ = true;
		reveal_time_ = ImGui::GetTime();
		filter_[0] = '\0';
	}
	if (reveal_document_ != document->identity() || reveal_record_ != selection)
		reveal_field_.clear();
	Reveal reveal;
	reveal.record = reveal_record_;
	reveal.field = reveal_field_;
	reveal.scroll = reveal_scroll_;
	const double since = ImGui::GetTime() - reveal_time_;
	reveal.light = reveal_field_.empty() || since >= kFlashSeconds ? 0.0f : float(1.0 - since / kFlashSeconds) * 0.8f;
	breadcrumb(workspace_, *document, selection);
	// Several records of one kind: the fields they share, each change set on every one.
	Targets together{selection};
	for (const NodeAddress &address : view.documents.selected)
		if (address != selection) together.push_back(address);
	const bool one_kind = std::all_of(together.begin(), together.end(),
	                                  [&](const NodeAddress &address) { return address.kind == selection.kind; });
	if (together.size() > 1 && one_kind) {
		draw_together(*document, together);
		return;
	}
	if (together.size() > 1) {
		const std::string note = std::to_string(together.size()) +
		                         " records of different kinds selected; the fields below are the primary one's.";
		ui_kit::empty_state(note.c_str());
	}
	ui_kit::filter_box("##filter", filter_, sizeof(filter_), "Filter fields and lists");
	// The selection's own collections, or its owner's when it holds none, so the records
	// beside it (a window's other actions) stay one click away.
	NodeAddress owner = selection;
	Document::Placement at;
	if (document->collections_of(selection).empty() && document->placement(selection, at)) owner = at.owner;
	const std::vector<InspectorSection> plan = plan_inspector(*document, selection, owner, filter_);
	if (plan.empty() && filter_[0]) ui_kit::empty_state("No field or list matches the filter.");
	for (const InspectorSection &section : plan) draw_section(workspace_, picker_, *document, selection, owner, section, &reveal);
	reveal_scroll_ = reveal.scroll;
	// F2: Rename everywhere on the record's first field that defines a name.
	if (ImGui::Shortcut(ImGuiKey_F2))
		for (const FieldSchema &schema : document->fields(selection.kind)) {
			const FieldUse field = document->field_on(selection, schema);
			Value value;
			if (!document->get(selection, schema.id, value) || !renames_name(*document, {selection}, field, value)) continue;
			rename_everywhere(workspace_, *document, selection, field, value);
			break;
		}
	referenced_by(workspace_, *document, selection);
	findings_.follow(view);
	findings(view, findings_, *document, *row, selection);
}

// Several records of one kind (ADR 0046 S9k2): which they are (a click selects one alone),
// then the fields they share (plan_shared_inspector), each showing the primary's value,
// marked where they differ and where any of them changed since the last save; a change is
// one batch over all of them, one undo step. Their lists stay with each record's own form.
void InspectorWindow::draw_together(const Document &document, const std::vector<NodeAddress> &records) {
	const std::string count = std::to_string(records.size()) + " " + document.kind_label(records.front().kind) +
	                          " records selected: a change here sets every one of them.";
	ui_kit::empty_state(count.c_str());
	const float line = ImGui::GetTextLineHeightWithSpacing();
	if (ImGui::BeginChild("together", ImVec2(0.0f, line * float(std::min<size_t>(records.size(), 4)) + 4.0f),
	                      ImGuiChildFlags_Borders)) {
		for (const NodeAddress &record : records) {
			ImGui::PushID(static_cast<int>(record.child ? record.child : record.row));
			// Its path in the names the windows show (the file's own path in its tooltip).
			std::string titles;
			for (const NodeAddress &owner : document.ancestors(record)) titles += document.record_title(owner) + "/";
			titles += document.record_title(record);
			const std::string path = document.record_path(record);
			const float x = ImGui::GetCursorScreenPos().x;
			const std::string shown = ui_kit::fit(ui_kit::kChangeRoom + titles, ImGui::GetContentRegionAvail().x);
			if (ImGui::Selectable((shown + "###record").c_str(), record == records.front())) select(workspace_, document, record);
			ui_kit::change_dot(document.record_change(record), x);
			ui_kit::tooltip_lazy([&] {
				const std::string above = titles != path ? titles + "\n" : std::string();
				return above + path + "\nSelect this one alone.";
			});
			ImGui::PopID();
		}
	}
	ImGui::EndChild();
	ui_kit::filter_box("##filter", filter_, sizeof(filter_), "Filter fields");
	const std::vector<InspectorSection> plan = plan_shared_inspector(document, records, filter_);
	if (plan.empty() && filter_[0]) ui_kit::empty_state("No field matches the filter.");
	for (const InspectorSection &section : plan) {
		if (!section.key.empty()) {
			const bool shown = section.written || section_changed(document, records, section);
			const bool open = ImGui::CollapsingHeader((section.title + "###" + section.key).c_str(),
			                                          shown ? ImGuiTreeNodeFlags_DefaultOpen : 0);
			ui_kit::tooltip(section.key);
			if (!open) continue;
		}
		ImGui::PushID(section.key.c_str());
		if (ImGui::BeginTable("fields", 2, ImGuiTableFlags_SizingStretchProp)) {
			ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 0.4f);
			ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.6f);
			// The block's own switch (named as its group, leading it) reads as it does alone.
			const bool has_switch =
			        !section.key.empty() && std::any_of(section.fields.begin(), section.fields.end(), [&](const FieldUse &field) {
				        return field.schema->id == section.key && is_yes_no(*field.schema);
			        });
			field_rows(workspace_, picker_, document, records, section.fields, nullptr, has_switch ? section.key : std::string());
			ImGui::EndTable();
		}
		ImGui::PopID();
	}
}

} // namespace opennova::editor
