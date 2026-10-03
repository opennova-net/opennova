#pragma once

#include <string>
#include <vector>

#include <editor/project/project_document.h>
#include <editor/session/view/project_view.h>

namespace opennova::editor {

// The fields a project's expansion is set with (ADR 0046 S16), drawn by the new-project form and the
// project settings dialog alike: "Builds on" (the base game, or one of the game install's expansions by
// its folder's name and the Mods list's name) and "Build as an expansion" with its name, checked as the
// name is typed (expansion_name_problem: the game's own rule), the problem shown under it.
class ExpansionFields {
public:
	// The fields set from a project's expansion (a standalone one: unchecked, the base game), which
	// they are then weighed against: as it stands, nothing is checked again.
	void set(const ProjectExpansion &expansion);
	// Draws them over the install's expansions `installed` (the open project's install for its settings, the
	// install a new project opens with for the New project form); true while they hold an expansion the
	// game takes (or none).
	bool draw(const std::vector<ProjectView::InstallExpansion> &installed);
	// The expansion they hold: none when "Build as an expansion" is unchecked.
	ProjectExpansion value() const;

private:
	ProjectExpansion current_;
	bool as_expansion_ = false;
	char name_[32] = "";
	std::string builds_on_;
};

} // namespace opennova::editor
