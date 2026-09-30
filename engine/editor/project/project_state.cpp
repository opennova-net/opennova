#include <editor/project/project_state.h>

#include <utility>

#include <editor/assets/project_scan.h>
#include <editor/project/project_refresh.h>

namespace opennova::editor {

ProjectState refresh_project_state(const ProjectPaths &paths, const ProjectDocument &doc, bool force_import,
                                   const std::string &only) {
	ProjectRefresh refresh(paths, doc, force_import, only);
	while (!refresh.step(kWholeWalkStep)) {
	}
	ProjectState state;
	state.imports = std::move(refresh.imports());
	state.scan = std::move(refresh.scan());
	state.requirements = std::move(refresh.requirements());
	return state;
}

} // namespace opennova::editor
