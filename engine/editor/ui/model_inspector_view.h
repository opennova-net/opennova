#pragma once

// A model's part of the Inspector (ADR 0046 S17, Models): what the model's row of ui/document_views
// hooks in. The data is the portable core's (documents/model_surfaces, model_labels); this draws it.

#include <editor/model/document.h>
#include <editor/ui/document_views.h>
#include <editor/ui/workspace.h>

namespace opennova::editor {

// The top of the Inspector over a model's record. A material: what bullets meet on it, its bullet faces'
// surface picked by name (every face made from it set at once, one undo step) and their flags, a mixed
// material's surfaces counted with Make all and Select the faces that differ. A bullet face: the material
// it was made from (a click selects it). A user point: what the game makes of its name. A LOD: what it
// draws at. False, nothing taken, for any other record.
bool draw_model_inspector(Workspace &workspace, const Document &document, const NodeAddress &record,
                          InspectorTaken &taken);

} // namespace opennova::editor
