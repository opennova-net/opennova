#include "music_bank_inspector.h"

#include <cstdio>
#include <string>

#include <imgui.h>

#include <base/io/strutil.h>
#include <editor/documents/music_bank_document.h>
#include <editor/project/project_files.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/ui_kit.h>

namespace opennova::editor {

namespace {

// A sentence, muted and wrapped.
void note(const std::string &text) {
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
	ImGui::TextWrapped("%s", text.c_str());
	ImGui::PopStyleColor();
}

} // namespace

bool draw_music_bank_inspector(Workspace &workspace, const Document &document, const NodeAddress &record,
                               InspectorTaken &) {
	const auto *bank = dynamic_cast<const MusicBankDocument *>(&document);
	const MusicBankRow *row = bank ? bank->bank_row() : nullptr;
	if (!row || row->id != record.row) return false;
	if (!record.child) {
		double seconds = 0.0;
		for (const MusicBankStream &stream : row->streams) seconds += music_stream_seconds(stream);
		char total[32];
		std::snprintf(total, sizeof(total), "%.0f", seconds);
		note(std::to_string(row->streams.size()) + " streams, " + total + " s in all. The game streams the bank loose "
		     "beside its archives, and its music script plays each stream by its place.");
		return true;
	}
	int place = -1;
	if (!row->ids.lists.empty())
		for (size_t i = 0; i < row->ids.lists[0].size(); ++i)
			if (row->ids.lists[0][i].id == record.child) place = int(i);
	if (place < 0 || size_t(place) >= row->streams.size()) return false;
	const MusicBankStream &stream = row->streams[size_t(place)];
	note("The music script plays this stream as sound " + std::to_string(place) + ": moving or removing a stream before it "
	     "changes what the script plays.");
	ui_kit::WrapRow tools;
	const bool saved = !bank->dirty();
	if (ui_kit::tool(tools, "Play", saved,
	                 saved ? "Streams " + stream.name + " from the bank's file as the game streams it."
	                       : std::string("Save the bank first: the editor streams the bank's file, as the game does."),
	                 true))
		workspace.request(request::play_stream(document.path(), int(place)));
	if (ui_kit::tool(tools, "Stop", true, "Stops the sound the editor plays.", true)) workspace.request(request::stop_sound());
	const WorkspaceView::Sound &sound = workspace.view().workspace.sound;
	if (!sound.voices.empty() && sound.voices.front().stream == place && sound.voices.front().path == document.path() &&
	    !sound.words.empty())
		note("Last played: " + sound.words);
	return true;
}

} // namespace opennova::editor
