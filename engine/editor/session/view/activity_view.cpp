#include <editor/session/view/activity_view.h>

#include <editor/assets/asset_registry.h>
#include <editor/project_build/build_run.h>
#include <editor/project_build/export_build.h>

namespace opennova::editor {

ActivityView::ActivityView() :
		last_build(std::make_shared<const BuildReport>()), last_export(std::make_shared<const ExportReport>()) {}

bool ActivityView::missing_at_boot(const std::string &name) const {
	for (const std::string &reported : boot_missing)
		if (pff::normalized_logical_name(reported) == pff::normalized_logical_name(name)) return true;
	return false;
}

} // namespace opennova::editor
