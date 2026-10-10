#include <editor/ui/particle_key_inspector.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <imgui.h>

#include <editor/documents/particle_keys.h>
#include <editor/model/document_base.h>
#include <editor/model/text_document.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/ui_kit.h>

namespace opennova::editor {

namespace {

// The block each file's panel shows and the last refusal it met, by the file's path (the panel's own state).
struct PanelState {
	size_t block = 0;
	std::string adding; // the key an Add picked, its value not yet typed
	std::string refused;
};
std::map<std::string, PanelState> &panels() {
	static std::map<std::string, PanelState> state;
	return state;
}

// A value set: the edit's request, or why the reader would take it otherwise.
void set_value(Workspace &workspace, const TextDocument &text, const ParticleKeyBlock &block, const std::string &key,
		const std::string &value, PanelState &state) {
	Edit edit;
	std::string why;
	if (!particle_key_edit(text, block, key, value, edit, why)) {
		state.refused = key + ": " + why + ".";
		return;
	}
	state.refused.clear();
	workspace.request(request::edit_record(text.path(), std::move(edit)));
}

} // namespace

void draw_particle_key_inspector(Workspace &workspace, const DocumentBase &document) {
	const TextDocument *text = text_of(document);
	if (!text) return;
	const std::vector<ParticleKeyBlock> blocks = particle_key_blocks(*text);
	if (blocks.empty()) {
		ui_kit::empty_state("The game's reader stops in this file.",
		                    "Its Problems row names the line; the keys show here once the reader takes the text.");
		return;
	}
	PanelState &state = panels()[document.path()];
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
	const ParticleKeyBlock &block = blocks[state.block];
	const bool editable = workspace.view().allows(EditorRequestKind::EditRecord) && !document.blocked();
	ImGui::BeginDisabled(!editable);
	const float label_width = ImGui::GetFontSize() * 9.0f;
	std::vector<const ParticleKeyField *> lacking;
	for (const ParticleKeyField &field : block.fields) {
		if (!field.present) {
			lacking.push_back(&field);
			continue;
		}
		ImGui::PushID(&field);
		ImGui::AlignTextToFramePadding();
		if (field.row) ImGui::TextUnformatted(field.key.c_str());
		else ImGui::TextDisabled("%s", field.key.c_str());
		ImGui::SameLine(label_width);
		ImGui::SetNextItemWidth(-1.0f);
		char value[512] = {};
		std::strncpy(value, field.value.c_str(), sizeof(value) - 1);
		if (ImGui::InputText("##value", value, sizeof(value), ImGuiInputTextFlags_EnterReturnsTrue) &&
		    field.value != value && field.row)
			set_value(workspace, *text, block, field.key, value, state);
		ui_kit::tooltip(field.row ? particle_key_words(*field.row) + ".\nEnter sets it, on its own line of the text."
		                          : std::string("A key the game's reader keeps as unknown: it reads nothing of it."));
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
			ui_kit::tooltip(particle_key_words(*row) + ".\nEnter adds it on a line of its own before the block's end.");
	}
	ImGui::EndDisabled();
	if (!state.refused.empty()) ImGui::TextWrapped("%s", state.refused.c_str());
	ImGui::Spacing();
}

} // namespace opennova::editor
