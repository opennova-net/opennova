#pragma once

#include <editor/session/editor_request.h>

namespace opennova::editor {

class SessionCore;

// A .cpt's heights written out (the export_heights request): the file at the request's path (left out, the active
// document) read as the game would read it now, so an open document's unsaved heights are in it (ProjectAssetSource,
// the bytes its Save would write), and its 1024 x 1024 depth words written to values.to as the game's own 16-bit
// .raw (trngen::encode_heightmap_raw16), the atlas as the .cpt holds it, which a terrain import takes as it is.
// Nothing of the project changes, so it works on an import's .cpt too and opens no document. values.to, a relative
// one from the project's folder, lands outside the project: a project's files are every file of its folder, and a
// push sends them all. Refused, nothing written (terrain_heights.export).
void export_heights(SessionCore &core, const EditorRequest &request);

} // namespace opennova::editor
