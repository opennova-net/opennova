#pragma once

#include <memory>

namespace opennova::editor {

class ProjectCheck;

// The menu type's registry hook (DocumentType::project_check, ADR 0046 S13 V9): a render check
// (preview/menu_render_check.h) with nothing rendered yet. Declared alone, so the registry names
// the hook without the render path.
std::unique_ptr<ProjectCheck> make_menu_render_check();

} // namespace opennova::editor
