#include "sound_inspector.h"

#include <string>

#include <imgui.h>

#include <base/io/os_path.h>
#include <base/io/strutil.h>
#include <editor/documents/dialog_bank_document.h>
#include <editor/documents/sound_bank_document.h>
#include <editor/documents/sound_profile_document.h>
#include <editor/preview/sound_preview.h>
#include <editor/project/project_files.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/ui_kit.h>
#include <runtime/audio/dialog_queue.h>

namespace opennova::editor {

namespace {

// A sentence of the game's words, muted and wrapped.
void note(const std::string &text) {
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
	ImGui::TextWrapped("%s", text.c_str());
	ImGui::PopStyleColor();
}

// What the editor's last play of `set` played, where it is that set's.
void last_play(const Workspace &workspace, const std::string &set) {
	const WorkspaceView::Sound &sound = workspace.view().workspace.sound;
	if (sound.set.empty() || !strutil::iequals(sound.set, set) || sound.words.empty()) return;
	note("Last played: " + sound.words);
}

constexpr audio::FootSurface kSurfaces[] = {audio::FootSurface::Ground, audio::FootSurface::Snow, audio::FootSurface::Object, audio::FootSurface::Water};

const char *surface_label(audio::FootSurface surface) {
	switch (surface) {
	case audio::FootSurface::Snow: return "Snow";
	case audio::FootSurface::Object: return "Object";
	case audio::FootSurface::Water: return "Water";
	case audio::FootSurface::Ground: break;
	}
	return "Ground";
}

// A footstep's Play on each surface: the slot the game's test picks there [orig: org2 @0x4b77c6-0x4b78a8].
void footstep_tools(Workspace &workspace, const std::string &profile, int foot) {
	ui_kit::WrapRow row;
	for (const audio::FootSurface surface : kSurfaces) {
		const int slot = audio::footstep_slot_on(surface, foot);
		const std::string tip = std::string("Plays ") + audio::sound_profile_slot_keyword(slot) + ", the " +
		                        (foot == 0 ? "left" : "right") + " foot on " + foot_surface_word(surface) +
		                        ": the game tests water over a plane first, then standing on an object, then snow, then the "
		                        "ground.";
		if (ui_kit::tool(row, surface_label(surface), true, tip, true))
			workspace.request(request::play_footstep(profile, foot_surface_word(surface), foot == 0 ? "left" : "right"));
	}
}

} // namespace

bool draw_sound_bank_inspector(Workspace &workspace, const Document &document, const NodeAddress &record,
                               InspectorTaken &) {
	const auto *bank = dynamic_cast<const SoundBankDocument *>(&document);
	const Node *node = bank ? bank->row(record.row) : nullptr;
	if (!node) return false;
	const auto &row = static_cast<const SoundBankRow &>(*node);
	ui_kit::WrapRow tools;
	if (row.kind == node_kind(SoundBankKind::Wave)) {
		const std::string file = io::utf8_file_name(row.wave.file); // either separator, on every host
		if (ui_kit::tool(tools, "Play", !file.empty(),
		                 file.empty() ? std::string("The wave names no file.") : "Plays " + file + " as recorded.", true))
			workspace.request(request::play_sound(file));
		return true;
	}
	const std::string &set = row.set.name;
	if (ui_kit::tool(tools, "Play", !set.empty(),
	                 "Plays " + set + " from this bank as the game plays it: each layer the view admits picks its member "
	                 "(in order, a random start then in order, or at random by its flags) and composes the set's and the "
	                 "member's pitch.",
	                 true))
		workspace.request(request::play_set(set, document.path()));
	if (ui_kit::tool(tools, "Stop", true, "Stops the sound the editor plays.", true)) workspace.request(request::stop_sound());
	last_play(workspace, set);
	return true;
}

bool draw_dialog_bank_inspector(Workspace &workspace, const Document &document, const NodeAddress &record,
                                InspectorTaken &) {
	const auto *bank = dynamic_cast<const DialogBankDocument *>(&document);
	const Node *node = bank ? bank->row(record.row) : nullptr;
	if (!node) return false;
	const auto &row = static_cast<const DialogBankRow &>(*node);
	const std::string &name = row.dialog.name;
	const int64_t number = audio::dialog_index_of(name);
	note(number >= 1 ? "A mission plays this dialog as dialog " + std::to_string(number) + " (its Play dialog action, its "
	                   "Dialog triggers): each line the wave of its name in " + dialog_sounds_scope(document.path()) +
	                   ", one after another, its subtitle in the chat."
	                 : "No Play dialog plays this dialog: a mission names one by the number that forms dlg%03i.");
	// A line: its index in the dialog, by its identity.
	int line = -1;
	if (record.child && !row.ids.lists.empty())
		for (size_t i = 0; i < row.ids.lists[0].size(); ++i)
			if (row.ids.lists[0][i].id == record.child) line = int(i);
	ui_kit::WrapRow tools;
	if (ui_kit::tool(tools, "Play", !name.empty(),
	                 "Plays " + name + " as the game plays it: its lines one after another, each once the one before has "
	                 "ended and after its wait.",
	                 true))
		workspace.request(request::play_dialog(name, document.path()));
	if (line >= 0 && ui_kit::tool(tools, "Play line", true, "Plays this line alone: its wave at its dialog volume.", true))
		workspace.request(request::play_dialog(name, document.path(), line));
	if (ui_kit::tool(tools, "Stop", true, "Stops the sound the editor plays.", true)) workspace.request(request::stop_sound());
	last_play(workspace, name);
	return true;
}

bool draw_sound_profile_inspector(Workspace &workspace, const Document &document, const NodeAddress &record,
                                  InspectorTaken &) {
	const auto *profiles = dynamic_cast<const SoundProfileDocument *>(&document);
	const Node *node = profiles ? profiles->row(record.row) : nullptr;
	if (!node) return false;
	const std::string &profile = static_cast<const SoundProfileRow &>(*node).profile.name;
	if (!record.child) {
		note("Footsteps of this profile, as the game picks the slot by what is under the foot:");
		footstep_tools(workspace, profile, 0);
		return true;
	}
	const RecordHandle handle = profiles->record_in(*node, record);
	if (!handle || record.kind != node_kind(SoundProfileKind::Slot)) return false;
	const ProfileSlot &slot = handle.as<ProfileSlot>();
	note(std::string(audio::sound_profile_slot_keyword(slot.slot)) + ": " + sound_profile_slot_words(slot.slot) + " (" +
	     sound_profile_slot_family(slot.slot) + ").");
	ui_kit::WrapRow tools;
	if (ui_kit::tool(tools, "Play", !slot.set.empty(),
	                 slot.set.empty() ? std::string("The slot names no set: the game plays nothing for it.")
	                                  : "Plays " + slot.set + " as the game finds it across its banks.",
	                 true))
		workspace.request(request::play_profile_slot(profile, audio::sound_profile_slot_keyword(slot.slot)));
	if (ui_kit::tool(tools, "Stop", true, "Stops the sound the editor plays.", true)) workspace.request(request::stop_sound());
	if (slot.slot >= audio::kSlotFootLGround && slot.slot <= audio::kSlotFootWater)
		footstep_tools(workspace, profile, (slot.slot - audio::kSlotFootLGround) % 2);
	last_play(workspace, slot.set);
	return true;
}

} // namespace opennova::editor
