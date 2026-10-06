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

// One line going where it leads (a click), cut to the room, whole in its tooltip; the line a Go to landed
// on drawn selected, and scrolled to with `reveal`. A name nothing resolves goes where it belongs (a
// symbol's file), or for a file the project lacks to its finding in Problems, with its fixes (DI-17: a Go
// to never dead-ends).
void line_of(Workspace &workspace, const FilePageLine &line, bool reveal) {
	const std::string shown = ui_kit::fit(line.text, ImGui::GetContentRegionAvail().x);
	const bool to_problems = line.missing && line.target.file.empty() && !line.name.empty();
	if (line.missing) ImGui::PushStyleColor(ImGuiCol_Text, ui_kit::reference_color(ReferenceStatus::Missing));
	if (ImGui::Selectable((shown + "###line").c_str(), line.at)) {
		if (!line.target.file.empty()) {
			window_requests::go_to(workspace, line.target);
		} else if (to_problems) {
			io::JsonValue problems = io::JsonValue::make_object();
			problems.set("text", io::JsonValue::make_string(line.name));
			problems.set("scope", io::JsonValue::make_string("project"));
			window_requests::set_workspace(workspace, "problems", std::move(problems));
			window_requests::focus(workspace, "problems");
		}
	}
	if (line.missing) ImGui::PopStyleColor();
	if (line.at && reveal) ImGui::SetScrollHereY(0.3f);
	ui_kit::tooltip(line.text + (!line.target.file.empty() ? "\nA click goes there: " + window_requests::go_to_words(line.target)
	                             : to_problems ? "\nA click shows it in Problems, with what can make it."
	                                           : std::string()));
}

// A list of the page's lines under its heading, the one a Go to landed on kept drawn to be scrolled to.
void lines(Workspace &workspace, const char *heading, const std::vector<FilePageLine> &list, const char *none, bool reveal) {
	ImGui::SeparatorText((std::string(heading) + " (" + std::to_string(list.size()) + ")").c_str());
	if (list.empty()) return ui_kit::empty_state(none);
	ImGui::PushID(heading);
	ImGuiListClipper clipper;
	clipper.Begin(static_cast<int>(list.size()));
	if (reveal)
		for (size_t i = 0; i < list.size(); ++i)
			if (list[i].at) clipper.IncludeItemByIndex(static_cast<int>(i));
	while (clipper.Step())
		for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
			ImGui::PushID(i);
			line_of(workspace, list[size_t(i)], reveal);
			ImGui::PopID();
		}
	ImGui::PopID();
}

// What the file defines (DI-17): each name with how many name it, opened onto those uses (each a line
// going there); the one a Go to landed on drawn selected and open.
void definitions(Workspace &workspace, const FilePage &page, bool reveal) {
	if (page.defines.empty()) return;
	ImGui::SeparatorText(("Defines (" + std::to_string(page.defines.size()) + ")").c_str());
	ImGui::PushID("defines");
	for (size_t i = 0; i < page.defines.size(); ++i) {
		const FilePageDefinition &defined = page.defines[i];
		ImGui::PushID(static_cast<int>(i));
		ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth;
		if (defined.at) flags |= ImGuiTreeNodeFlags_Selected;
		if (defined.users.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
		if (defined.at && reveal) ImGui::SetNextItemOpen(true);
		const std::string uses = defined.read ? "used by " + std::to_string(defined.users.size()) : "no lookup finds it";
		const bool open = ImGui::TreeNodeEx("definition", flags, "%s - %s", defined.text.c_str(), uses.c_str());
		if (defined.at && reveal) ImGui::SetScrollHereY(0.3f);
		ui_kit::tooltip(defined.read ? defined.text + "\n" + counted(defined.users.size(), "field") + " of the project name it."
		                             : defined.text + "\nThe game never reads it: " + defined.unread + ".");
		if (open) {
			for (size_t u = 0; u < defined.users.size(); ++u) {
				ImGui::PushID(static_cast<int>(u));
				line_of(workspace, defined.users[u], false);
				ImGui::PopID();
			}
			ImGui::TreePop();
		}
		ImGui::PopID();
	}
	ImGui::PopID();
}

// A wave's Play and Stop (DI-17: the page plays what it is), and how the sound goes as the Shell reports it.
void wave_tools(Workspace &workspace, const FilePage &page) {
	ImGui::SameLine();
	if (ImGui::SmallButton("Play")) workspace.request(request::play_sound(page.path));
	ui_kit::tooltip("Play it as the game decodes it.");
	ImGui::SameLine();
	if (ImGui::SmallButton("Stop")) workspace.request(request::stop_sound());
	const WorkspaceView::Sound &sound = workspace.view().workspace.sound;
	using State = WorkspaceView::SoundState;
	if (sound.path != page.path || sound.state == State::Idle) return;
	const std::string said = sound.state == State::Starting  ? std::string("Starting...")
	                         : sound.state == State::Playing ? std::string("Playing")
	                         : sound.state == State::Ended   ? std::string("Played")
	                         : sound.state == State::Stopped ? std::string("Stopped")
	                                                         : "Does not play: " + sound.error;
	ImGui::SameLine();
	ImGui::TextDisabled("%s", said.c_str());
}

} // namespace

void draw_file_page(Workspace &workspace, const std::string &path, FilePageCache &cache, bool reveal) {
	const SessionView &view = workspace.view();
	const RevisionKey key = revision_key(view.revisions, {ViewConcern::Graph, ViewConcern::Files, ViewConcern::Findings,
	                                                      ViewConcern::Project});
	const bool shown = path == view.documents.page;
	const std::string &locator = shown ? view.documents.page_locator : std::string();
	const std::string &field = shown ? view.documents.page_field : std::string();
	if (!cache.made || cache.path != path || cache.locator != locator || cache.field != field || !(cache.key == key)) {
		cache.page = file_page(view, path, locator, field);
		cache.path = path;
		cache.locator = locator;
		cache.field = field;
		cache.key = key;
		cache.made = true;
	}
	const FilePage &page = cache.page;
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
	if (page.wave) wave_tools(workspace, page);
	definitions(workspace, page, reveal);
	lines(workspace, "Used by", page.used_by, "No file of the project names it.", false);
	lines(workspace, "Names", page.names, "It names no other file or name the editor follows.", reveal);
}

} // namespace opennova::editor
