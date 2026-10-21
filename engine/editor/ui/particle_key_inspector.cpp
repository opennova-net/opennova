#include <editor/ui/particle_key_inspector.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <imgui.h>

#include <base/io/cp1252.h>
#include <editor/documents/particle_keys.h>
#include <editor/graph/asset_graph.h>
#include <editor/model/document_base.h>
#include <editor/model/text_document.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/project_find.h>
#include <editor/ui/reference_fields.h>
#include <editor/ui/reference_picker.h>
#include <editor/ui/rename_dialog.h>
#include <editor/ui/ui_kit.h>

namespace opennova::editor {

namespace {

// The block each file's panel shows and the last refusal it met, by the file's path (the panel's own state), and the
// text as the panel last read it: its blocks and what the graph reads of it (extract_from_text: each name at its
// span), made again only when the document moves (its instance, its load, its revision).
struct PanelState {
	size_t block = 0;
	std::string adding; // the key an Add picked, its value not yet typed
	std::string refused;
	uint64_t identity = 0, load = 0, revision = 0;
	bool read = false;
	std::vector<ParticleKeyBlock> blocks;
	Extracted names;
};
std::map<std::string, PanelState> &panels() {
	static std::map<std::string, PanelState> state;
	return state;
}

// The picker the panel's name fields share (its popups each the document's and the name's own).
ReferencePicker &picker() {
	static ReferencePicker shared;
	return shared;
}

// The text read again where it moved since the panel last read it.
void read(PanelState &state, const TextDocument &text) {
	if (state.read && state.identity == text.identity() && state.load == text.load_generation() &&
	    state.revision == text.revision())
		return;
	state.read = true;
	state.identity = text.identity();
	state.load = text.load_generation();
	state.revision = text.revision();
	state.blocks = particle_key_blocks(text);
	state.names = Extracted();
	extract_from_text(text, state.names);
}

// Whether a span lies inside a field's value.
bool inside(const TextSpan &span, const ParticleKeyField &field) {
	return span.line && span.line == field.span.line && span.column >= field.span.column &&
	       span.column + span.length <= field.span.column + field.span.length;
}

// A value set: the edit's request, or why the reader would take it otherwise.
void set_value(Workspace &workspace, const TextDocument &text, const ParticleKeyBlock &block, const std::string &key,
		const std::string &value, PanelState &state, const ParticleKeyField *field = nullptr) {
	Edit edit;
	std::string why;
	if (!particle_key_edit(text, block, key, value, edit, why, field)) {
		state.refused = key + ": " + why + ".";
		return;
	}
	state.refused.clear();
	workspace.request(request::edit_record(text.path(), std::move(edit)));
}

// One name of a value given another (a name field's): that name's span replaced, the value checked as the reader
// takes it; the name in the text's code page.
void set_name(Workspace &workspace, const TextDocument &text, const ParticleKeyField &field, const GraphEdge &name,
		const std::string &to, PanelState &state) {
	std::string stored;
	Edit edit;
	std::string why;
	if (!utf8_to_cp1252(to, stored)) why = "the game's code page (Windows-1252) has no character of it";
	else if (particle_name_edit(field, name.span, stored, edit, why)) {
		state.refused.clear();
		workspace.request(request::edit_record(text.path(), std::move(edit)));
		return;
	}
	state.refused = field.key + ": " + why + ".";
}

// A name the value holds as its text_reference_field (the graph's edge of it there).
void name_field(Workspace &workspace, const TextDocument &text, const ParticleKeyField &field, const GraphEdge &name,
		PanelState &state) {
	TextPlace place;
	place.document = &text;
	place.record = name.record;
	place.key = name.field;
	place.span = name.span;
	place.value = name.value;
	place.kind = name.kind;
	place.scope = name.scope;
	place.loader_arg = name.loader_arg;
	place.width = name.width;
	std::string given;
	if (text_reference_field(workspace, picker(), place, given) && given != name.value)
		set_name(workspace, text, field, name, given, state);
}

// An id row's tools (the name it defines, its symbol): Rename everywhere (F2 over its box) and Find usages, after
// its box; the box's width left for them.
float id_tools_width() {
	return ui_kit::button_width("Rename...") + ui_kit::button_width("Usages") + ImGui::GetStyle().ItemSpacing.x * 2.0f;
}
void id_tools(Workspace &workspace, const TextDocument &text, const GraphSymbol &defined, bool box_keyed) {
	const bool renames = workspace.view().allows(EditorRequestKind::PreviewRename) && !text.blocked();
	const bool f2 = box_keyed && ImGui::IsKeyPressed(ImGuiKey_F2, false);
	ImGui::SameLine();
	ImGui::BeginDisabled(!renames);
	if ((ImGui::SmallButton("Rename...") || f2) && renames)
		workspace.request(RenameDialog::preview(text.path(), defined.locator, defined.field, defined.display, true));
	ImGui::EndDisabled();
	ui_kit::tooltip("Rename everywhere (F2): " + std::string(reference_row(defined.kind).label) + " " + defined.display +
	                " and every use of it, in every file.");
	ImGui::SameLine();
	if (ImGui::SmallButton("Usages")) ProjectFind::open_usages(workspace, text.path(), defined.locator);
	ui_kit::tooltip("Find usages (Shift+F12): who names " + defined.display + ", each a Go to.");
	// What F12 and Shift+F12 act on while the row is under the pointer or its box has the keyboard.
	if (box_keyed) {
		JumpSubject subject;
		subject.usages_file = text.path();
		subject.usages_locator = defined.locator;
		workspace.offer_jump(subject);
	}
}

} // namespace

void draw_particle_key_inspector(Workspace &workspace, const DocumentBase &document) {
	const TextDocument *text = text_of(document);
	if (!text) return;
	PanelState &state = panels()[document.path()];
	read(state, *text);
	const std::vector<ParticleKeyBlock> &blocks = state.blocks;
	if (blocks.empty()) {
		ui_kit::empty_state("The game's reader stops in this file.",
		                    "Its Problems row names the line; the keys show here once the reader takes the text.");
		return;
	}
	state.block = std::min(state.block, blocks.size() - 1);
	ImGui::SeparatorText("Keys");
	ImGui::SetNextItemWidth(-1.0f);
	if (ImGui::BeginCombo("##block", particle_block_title(blocks[state.block]).c_str())) {
		for (size_t i = 0; i < blocks.size(); ++i)
			if (ImGui::Selectable(particle_block_title(blocks[i]).c_str(), i == state.block)) {
				state.block = i;
				state.adding.clear();
				state.refused.clear();
			}
		ImGui::EndCombo();
	}
	// The text as read at the frame's start: a set this frame reads it again on the next.
	const ParticleKeyBlock &block = blocks[state.block];
	const Extracted &names = state.names;
	const bool editable = workspace.view().allows(EditorRequestKind::EditRecord) && !document.blocked();
	ImGui::BeginDisabled(!editable);
	const float label_width = ImGui::GetFontSize() * 9.0f;
	std::vector<const ParticleKeyField *> lacking;
	for (size_t row = 0; row < block.fields.size(); ++row) {
		const ParticleKeyField &field = block.fields[row];
		if (!field.present) {
			lacking.push_back(&field);
			continue;
		}
		ImGui::PushID(int(row)); // each line its own (a key written twice is two rows)
		// The names the value holds (each a reference the graph reads at its span), and the one it defines (an id).
		std::vector<const GraphEdge *> held;
		for (const GraphEdge &edge : names.edges)
			if (inside(edge.span, field)) held.push_back(&edge);
		const GraphSymbol *defined = nullptr;
		for (const GraphSymbol &symbol : names.symbols)
			if (!defined && inside(symbol.span, field)) defined = &symbol;
		const std::string tip = field.row ? particle_key_words(*field.row) + ".\nEnter sets it, on its own line of the text."
		                                  : std::string("A key the game's reader keeps as unknown: it reads nothing of it.");
		ImGui::AlignTextToFramePadding();
		if (field.row) ImGui::TextUnformatted(field.key.c_str());
		else ImGui::TextDisabled("%s", field.key.c_str());
		ui_kit::tooltip(tip);
		ImGui::SameLine(label_width);
		// A value that is one name alone (a child_id, a curve's table with no modifier, a graphic's texture with no
		// mode): its name field in the box's place; else the whole value's box, and each name it holds under it.
		const bool alone = held.size() == 1 && held.front()->span.column == field.span.column &&
		                   held.front()->span.length == field.span.length;
		if (alone) {
			name_field(workspace, *text, field, *held.front(), state);
		} else {
			ImGui::SetNextItemWidth(defined ? -id_tools_width() : -1.0f);
			char value[512] = {};
			std::strncpy(value, field.value.c_str(), sizeof(value) - 1);
			if (ImGui::InputText("##value", value, sizeof(value), ImGuiInputTextFlags_EnterReturnsTrue) &&
			    field.value != value && field.row)
				set_value(workspace, *text, block, field.key, value, state, &field);
			const bool keyed = ImGui::IsItemHovered() || ImGui::IsItemFocused() || ImGui::IsItemActive();
			ui_kit::tooltip(tip);
			if (defined) id_tools(workspace, *text, *defined, keyed);
			for (size_t i = 0; i < held.size(); ++i) {
				ImGui::PushID(int(i));
				ImGui::SetCursorPosX(ImGui::GetCursorPosX() + label_width);
				name_field(workspace, *text, field, *held[i], state);
				ImGui::PopID();
			}
		}
		ImGui::PopID();
	}
	// A key the block lacks, picked from the list (or a graphic layer's or a slot's key typed by its name): its value
	// typed, then its line put before the block's closing brace.
	ImGui::Spacing();
	ImGui::SetNextItemWidth(-1.0f);
	if (ImGui::BeginCombo("##add", "Add a key")) {
		for (const ParticleKeyField *field : lacking) {
			if (ImGui::Selectable(field->key.c_str())) {
				state.adding = field->key;
				state.refused.clear();
			}
			if (field->row) ui_kit::tooltip(particle_key_words(*field->row) + ".");
		}
		ImGui::EndCombo();
	}
	char named[64] = {};
	ImGui::SetNextItemWidth(-1.0f);
	if (ImGui::InputTextWithHint("##named", "or a key by its name (g1_alpha, collide_sound3...)", named, sizeof(named),
	                             ImGuiInputTextFlags_EnterReturnsTrue) &&
	    named[0]) {
		if (particle::key_row(block.kind, named)) {
			state.adding = named;
			state.refused.clear();
		} else {
			state.refused = std::string(named) + ": no key of this block the game reads.";
		}
	}
	if (!state.adding.empty()) {
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(state.adding.c_str());
		ImGui::SameLine(label_width);
		ImGui::SetNextItemWidth(-1.0f);
		char value[512] = {};
		if (ImGui::InputText("##adding", value, sizeof(value), ImGuiInputTextFlags_EnterReturnsTrue) && value[0]) {
			set_value(workspace, *text, block, state.adding, value, state);
			if (state.refused.empty()) state.adding.clear();
		}
		if (const particle::KeyRow *row = particle::key_row(block.kind, state.adding))
			ui_kit::tooltip(particle_key_words(*row) + ".\nEnter adds it on a line of its own where the game's reader takes "
			                "it (a key a graphic line copies before the first graphic line, a layer's after its graphic "
			                "line).");
	}
	ImGui::EndDisabled();
	if (!state.refused.empty()) ui_kit::refusal_text(state.refused);
	ImGui::Spacing();
}

} // namespace opennova::editor
