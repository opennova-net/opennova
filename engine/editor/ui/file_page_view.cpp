#include "file_page_view.h"

#include <editor/model/field_text.h>
#include <editor/project/project_files.h>
#include <editor/session/file_page.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/ui_kit.h>
#include <editor/ui/workspace.h>

#include <imgui.h>

namespace opennova::editor {
namespace {

// A list of the page's lines under its heading, each going where it leads (a click), cut to the room,
// whole in its tooltip.
void lines(Workspace &workspace, const char *heading, const std::vector<FilePageLine> &list, const char *none) {
	ImGui::SeparatorText((std::string(heading) + " (" + std::to_string(list.size()) + ")").c_str());
	if (list.empty()) return ui_kit::empty_state(none);
	ImGuiListClipper clipper;
	clipper.Begin(static_cast<int>(list.size()));
	while (clipper.Step())
		for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
			const FilePageLine &line = list[size_t(i)];
			ImGui::PushID(i);
			const std::string shown = ui_kit::fit(line.text, ImGui::GetContentRegionAvail().x);
			if (line.missing) ImGui::PushStyleColor(ImGuiCol_Text, ui_kit::reference_color(ReferenceStatus::Missing));
			if (ImGui::Selectable((shown + "###line").c_str()) && !line.target.file.empty())
				window_requests::go_to(workspace, line.target);
			if (line.missing) ImGui::PopStyleColor();
			ui_kit::tooltip(line.text + (line.target.file.empty() ? std::string() : "\nA click goes there."));
			ImGui::PopID();
		}
}

} // namespace

void draw_file_page(Workspace &workspace, const std::string &path) {
	const SessionView &view = workspace.view();
	const FilePage page = file_page(view, path);
	if (!page.found) {
		ui_kit::empty_state(("The project no longer has " + path + ".").c_str());
		if (ImGui::SmallButton("Close")) workspace.request(request::close_document(path));
		return;
	}
	ImGui::TextUnformatted(page.name.c_str());
	ImGui::SameLine();
	ImGui::TextDisabled("%s, %s", page.kind.c_str(), ui_kit::size_text(page.size).c_str());
	ImGui::TextWrapped("%s", page.what.c_str());
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
	ImGui::TextWrapped("Read by the game: %s", page.read_by.c_str());
	ImGui::PopStyleColor();
	ui_kit::tooltip("As the original game was seen to do: " + page.cite);
	ImGui::TextWrapped("%s %s", page.build.c_str(), page.editor.c_str());
	if (page.errors || page.warnings)
		ImGui::TextWrapped("Problems: %s, %s.", counted(page.errors, "error").c_str(), counted(page.warnings, "warning").c_str());
	if (ImGui::SmallButton("Show in Files")) workspace.request(request::show_in_files(page.path));
	ImGui::SameLine();
	if (ImGui::SmallButton("Show in folder")) workspace.request(request::reveal_path(join_path(view.project.root, page.path)));
	lines(workspace, "Used by", page.used_by, "No file of the project names it.");
	lines(workspace, "Names", page.names, "It names no other file or name the editor follows.");
}

} // namespace opennova::editor
