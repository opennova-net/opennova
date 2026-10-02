#include <editor/session/view/project_view.h>

#include <editor/assets/asset_registry.h>
#include <editor/import/import_run.h>
#include <editor/project/project_document.h>
#include <editor/requirements/requirements.h>

namespace opennova::editor {

ProjectView::ProjectView() :
		document(std::make_shared<const ProjectDocument>()),
		scan(std::make_shared<const AssetScan>()),
		requirements(std::make_shared<const RequirementReport>()),
		imports(std::make_shared<const std::vector<ImportedSource>>()) {}

} // namespace opennova::editor
