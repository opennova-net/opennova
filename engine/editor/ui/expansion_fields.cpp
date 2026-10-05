#include <editor/ui/expansion_fields.h>

#include <base/io/strutil.h>
#include <editor/project/expansion_name.h>
#include <editor/ui/ui_kit.h>

#include <imgui.h>

namespace opennova::editor {

namespace {

// An installed expansion as the combo names it: its folder's name, the Mods list's name after it.
std::string installed_words(const ProjectView::InstallExpansion &installed) {
	return installed.title.empty() || installed.title == installed.name ? installed.name
	                                                                      : installed.name + " - " + installed.title;
}

} // namespace

bool ExpansionFields::draw(ExpansionChoice &choice, const std::vector<ProjectView::InstallExpansion> &expansions,
                           const ProjectExpansion &current, bool &changed) {
	changed = false;
	// Builds on: the base game, or an expansion of the game install the game can mount (a name it no
	// longer has stays offered, as the project holds it). Names compared as the file system does.
	std::string shown = "The base game";
	for (const ProjectView::InstallExpansion &installed : expansions)
		if (strutil::iequals(installed.name, choice.builds_on)) shown = installed_words(installed);
	if (!choice.builds_on.empty() && shown == "The base game") shown = choice.builds_on + " (not in the game install)";
	if (ImGui::BeginCombo("Builds on", shown.c_str())) {
		if (ImGui::Selectable("The base game", choice.builds_on.empty()) && !choice.builds_on.empty()) {
			choice.builds_on.clear();
			changed = true;
		}
		for (const ProjectView::InstallExpansion &installed : expansions) {
			// Only what Apply takes is offered (expansion_name_problem's rule for an installed one).
			if (!expansion_name_problem(installed.name, ExpansionNameUse::BuildsOn).empty()) continue;
			if (ImGui::Selectable(installed_words(installed).c_str(), strutil::iequals(installed.name, choice.builds_on)) &&
			    choice.builds_on != installed.name) {
				choice.builds_on = installed.name;
				changed = true;
			}
			if (!installed.description.empty()) ui_kit::tooltip(installed.description);
		}
		ImGui::EndCombo();
	}
	ui_kit::tooltip("The game data the project's files come from: the base game, or an expansion the game install has, "
	                "mounted as the game mounts it with /exp.");
	// A project builds on an installed expansion only as an expansion of its own: the game reads an
	// expansion's own files only under /exp (the session holds the fields to that too).
	if (!choice.builds_on.empty() && !choice.as_expansion) {
		choice.as_expansion = true;
		changed = true;
	}
	ImGui::BeginDisabled(!choice.builds_on.empty());
	if (ImGui::Checkbox("Build as an expansion", &choice.as_expansion)) changed = true;
	ImGui::EndDisabled();
	ui_kit::tooltip("The build is an expansion folder, expansion\\<name>\\, played with /exp <name> in OpenNova and "
	                "in the game; its folder is what Export ships.");
	if (!choice.as_expansion) return true;
	name_.follow(choice.name);
	if (ImGui::InputText("Expansion name", name_.text, sizeof(name_.text))) {
		choice.name = name_.sent();
		changed = true;
	}
	if (choice.value() == current && !current.standalone()) return true; // as the project holds it: nothing to weigh again
	const std::string problem = expansion_name_problem(choice.name, ExpansionNameUse::Own);
	if (!problem.empty()) {
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetFontSize() * 28.0f);
		ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.45f, 1.0f), "%s", problem.c_str());
		ImGui::PopTextWrapPos();
		return false;
	}
	for (const ProjectView::InstallExpansion &installed : expansions)
		if (strutil::iequals(installed.name, choice.name)) {
			ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.45f, 1.0f), "The game install has an expansion named %s already.",
			                   installed.name.c_str());
			return false;
		}
	return true;
}

} // namespace opennova::editor
