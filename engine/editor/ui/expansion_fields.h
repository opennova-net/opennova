#pragma once

#include <string>
#include <vector>

#include <editor/project/project_document.h>
#include <editor/session/view/project_view.h>
#include <editor/ui/ui_kit.h>

namespace opennova::editor {

// What a project's expansion fields hold (ADR 0046 S16): what it builds on (an installed expansion's
// folder name, "" the base game), whether it builds as an expansion, and that expansion's name. The
// workspace holds it for the new-project form and the project settings (the MCP gaps lane).
struct ExpansionChoice {
	std::string builds_on;
	bool as_expansion = false;
	std::string name;
	// The expansion they hold: none while it does not build as one.
	ProjectExpansion value() const { return as_expansion ? ProjectExpansion{ name, builds_on } : ProjectExpansion(); }
};

// The fields a project's expansion is set with (ADR 0046 S16), drawn by the new-project form and the
// project settings dialog alike over what the workspace holds of them: "Builds on" (the base game, or one of
// the game install's expansions by its folder's name and the Mods list's name) and "Build as an expansion"
// with its name, checked as the name is typed (expansion_name_problem: the game's own rule), the problem
// shown under it. The name's text field is its own; what they hold is the caller's.
class ExpansionFields {
public:
	// Draws them over `choice` (what the caller holds) and the install's expansions `installed` (the open
	// project's install for its settings, the install a new project opens with for the New project form);
	// a person's change made in `choice`, `changed` then true (the caller raises it). True while they hold
	// an expansion the game takes (or none). `current`: the project's own, which as it stands is not weighed
	// again (a new project's: none).
	bool draw(ExpansionChoice &choice, const std::vector<ProjectView::InstallExpansion> &installed,
	          const ProjectExpansion &current, bool &changed);

private:
	ui_kit::HeldText<32> name_;
};

} // namespace opennova::editor
