#pragma once

#include <memory>

namespace opennova::editor {

class ProjectCheck;

// The mission type's registry hook (DocumentType::project_check, ADR 0046 S13 V9): a ground check
// (preview/mission_ground_check.h, DI-28) with nothing checked yet. Declared alone, so the registry
// names the hook without the check's rules.
std::unique_ptr<ProjectCheck> make_mission_ground_check();

} // namespace opennova::editor
