#include <editor/session/view/dialogs_view.h>

#include <editor/graph/rename_transaction.h>
#include <editor/import/import_plan.h>

namespace opennova::editor {

DialogsView::ImportPreview::ImportPreview() : plan(std::make_shared<const ImportPlan>()) {}

DialogsView::RenamePreview::RenamePreview() :
		sites(std::make_shared<const std::vector<RenameSite>>()) {}

} // namespace opennova::editor
