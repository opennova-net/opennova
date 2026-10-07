#include <editor/ui/menu_inspector.h>

#include <memory>
#include <string>
#include <vector>

#include <imgui.h>

#include <editor/documents/mnu_document.h>
#include <editor/documents/mnu_table.h>
#include <editor/preview/menu_sounds.h>
#include <editor/session/request_factories.h>
#include <editor/ui/ui_kit.h>
#include <formats/mnu/mnu.h>
#include <runtime/menu/menu_sound.h>

namespace opennova::editor {

namespace {

// A sentence of the game's words, muted and wrapped.
void note(const std::string &text) {
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
	ImGui::TextWrapped("%s", text.c_str());
	ImGui::PopStyleColor();
}

// "On hover": a Play's head.
std::string when_words(int state) {
	std::string words = menu_sound_state_words(state);
	if (!words.empty()) words[0] = char(words[0] - 'a' + 'A');
	return words;
}

} // namespace

bool draw_menu_inspector(Workspace &workspace, const Document &document, const NodeAddress &record,
                         InspectorTaken &) {
	const auto *menu = dynamic_cast<const MnuDocument *>(&document);
	if (!menu || !record.child) return false;
	// A window, or one of its SOUND rows; the rest of a window's records (its actions, its parts) show their
	// own fields alone.
	constexpr NodeKind kWindow = node_kind(MenuKind::Window);
	NodeAddress window = record;
	if (record.kind != kWindow) {
		Document::Placement at;
		if (record.kind != menu_kind("sound") || !menu->placement(record, at) || at.owner.kind != kWindow) return false;
		window = at.owner;
	}
	std::shared_ptr<const mnu::Document> image;
	const mnu::Window *read = menu_image_window(*menu, window, image);
	if (!read) return false;
	const std::vector<MenuWindowSound> sounds = menu_window_sounds(*read);
	// A window with no row the pump plays heads nothing: its form shows its SOUND list (empty, or rows the game
	// refuses, which its findings name).
	if (sounds.empty()) return false;
	// One row of Plays, each what the game plays when: the last SOUND of its state, its set found in the bank the
	// SOUND names alone.
	ui_kit::WrapRow row;
	for (const MenuWindowSound &sound : sounds) {
		ImGui::PushID(sound.state);
		const std::string label = when_words(sound.state) + ": " + sound.set + " (" + sound.bank + ")";
		if (ui_kit::tool(row, label.c_str(), !sound.set.empty() && !sound.bank.empty(),
		                 std::string("What the game plays ") + menu_sound_state_words(sound.state) + " (its SOUND of state " +
		                         menu::menu_sound_state_token(sound.state) + ", the last of them): " + sound.set +
		                         ", from " + sound.bank + " alone. Click to play it as the game picks and pitches it.",
		                 true))
			workspace.request(request::play_set(sound.set, sound.bank));
		ImGui::PopID();
	}
	if (read->type == mnu::WindowType::SpinList)
		note("A click on one of its arrows plays the arrow's own SELECTED instead: each arrow is a button of its own.");
	return true;
}

} // namespace opennova::editor
