#include <editor/ui/expansion_fields.h>

#include <algorithm>
#include <cstring>

#include <base/io/strutil.h>
#include <editor/project/expansion_name.h>
#include <editor/session/view/session_view.h>
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

void ExpansionFields::set(const ProjectExpansion &expansion) {
	current_ = expansion;
	as_expansion_ = !expansion.standalone();
	const size_t n = std::min(expansion.name.size(), sizeof(name_) - 1);
	std::memcpy(name_, expansion.name.data(), n);
	name_[n] = '\0';
	builds_on_ = expansion.builds_on;
}

bool ExpansionFields::draw(const SessionView &view) {
	// Builds on: the base game, or an expansion of the game install (a name it no longer has stays
	// offered, as the project holds it).
	std::string shown = "The base game";
	for (const ProjectView::InstallExpansion &installed : view.project.install_expansions)
		if (installed.name == builds_on_) shown = installed_words(installed);
	if (!builds_on_.empty() && shown == "The base game") shown = builds_on_ + " (not in the game install)";
	if (ImGui::BeginCombo("Builds on", shown.c_str())) {
		if (ImGui::Selectable("The base game", builds_on_.empty())) builds_on_.clear();
		for (const ProjectView::InstallExpansion &installed : view.project.install_expansions) {
			if (ImGui::Selectable(installed_words(installed).c_str(), installed.name == builds_on_)) builds_on_ = installed.name;
			if (!installed.description.empty()) ui_kit::tooltip(installed.description);
		}
		ImGui::EndCombo();
	}
	ui_kit::tooltip("The game data the project's files come from: the base game, or an expansion the game install has, "
	                "mounted as the game mounts it with /exp.");
	// A project builds on an installed expansion only as an expansion of its own: the game reads an
	// expansion's own files only under /exp.
	if (!builds_on_.empty()) as_expansion_ = true;
	ImGui::BeginDisabled(!builds_on_.empty());
	ImGui::Checkbox("Build as an expansion", &as_expansion_);
	ImGui::EndDisabled();
	ui_kit::tooltip("The build is an expansion folder, expansion\\<name>\\, played with /exp <name> in OpenNova and "
	                "in the game; its folder is what Export ships.");
	if (!as_expansion_) return true;
	ImGui::InputText("Expansion name", name_, sizeof(name_));
	if (value() == current_ && !current_.standalone()) return true; // as the project holds it: nothing to weigh again
	const std::string problem = expansion_name_problem(name_, ExpansionNameUse::Own);
	if (!problem.empty()) {
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetFontSize() * 28.0f);
		ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.45f, 1.0f), "%s", problem.c_str());
		ImGui::PopTextWrapPos();
		return false;
	}
	for (const ProjectView::InstallExpansion &installed : view.project.install_expansions)
		if (strutil::iequals(installed.name, name_)) {
			ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.45f, 1.0f), "The game install has an expansion named %s already.",
			                   installed.name.c_str());
			return false;
		}
	return true;
}

ProjectExpansion ExpansionFields::value() const {
	if (!as_expansion_) return ProjectExpansion();
	return ProjectExpansion{ name_, builds_on_ };
}

} // namespace opennova::editor
