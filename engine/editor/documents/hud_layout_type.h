#pragma once

#include <memory>
#include <vector>

#include <editor/model/diagnostic.h>
#include <editor/model/document_base.h>
#include <editor/model/finding_code_row.h>
#include <editor/model/text_document.h>

namespace opennova::editor {

// The HUD layout type (the editor deep-integration plan's DI-20, "the HUD preview over hudpos.def"):
// hudpos.def, the table of where the HUD draws its parts [orig: HUD_ParseHudposToken @ 0x59F370],
// held as its text: the file is its text, so what a person types is what Save writes (no writer
// rewrites it whole; DI-37's handles and fields write an element's values into its lines as the engine's
// own writer writes them, preview/hud_layout_edit over formats/def/def_hudpos_write.h, on the line the game
// takes of each key, formats/def/def_hudpos_text.h). Its picture is the HUD viewport's
// (preview/hud_viewport), the game's own layout fill and frame compiler over the text as Save would write it; its names (the fonts and textures it names) are the graph's through
// the engine's own parser (graph/extractors, extract_hudpos), read from its text as it stands. The
// game's reader cuts its lines at CR LF alone [orig: File_ParseASCIIFile @ 0x53D8C7..0x53D8F5]
// (TextLineEnds::CrLf): Save writes an LF alone as CR LF, and a text holding one is the line-ends
// rule's finding (documents/line_ends.h), which its fix sets right in the text. It has no finding of
// its own: its table is empty.
std::unique_ptr<DocumentBase> make_hud_layout_document();
std::vector<Diagnostic> validate_hud_layout_file(const DocumentBase &document);
FindingTable hud_layout_finding_codes();

} // namespace opennova::editor
