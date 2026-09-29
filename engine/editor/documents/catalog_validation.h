#pragma once
#include <memory>
#include <vector>

#include <editor/documents/validation_cache.h>
#include <editor/model/diagnostic.h>

namespace opennova::editor {

class AssetGraph;

// The catalog document type's validator: used by the editor, CLI validate and Build.
// Open documents replace their disk versions so diagnostics describe the current
// draft. Input the game ignores is reported as a warning (saving drops it); input the
// typed model cannot carry is an error. The references a record makes (models,
// animation maps, ammo and weapon names, item ids, string ids) are the asset graph's.
std::vector<Diagnostic> validate_catalogs(const ValidationInput &input, const AssetGraph &graph);
} // namespace opennova::editor
