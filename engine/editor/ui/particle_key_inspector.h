#pragma once

#include <editor/ui/workspace.h>

namespace opennova::editor {

class DocumentBase;

// The particle type's part of the Inspector (ADR 0046 S23 B, documents/particle_keys): a block of the file picked
// from its list, each key it writes a field typed as the game's reader takes it (its words and range in its
// tooltip), and the keys of its kind it lacks to add. Enter sets a value: that one value's span of the text
// replaced, or the key's line put before the block's closing brace, one undo step; a value the reader would take
// otherwise is refused with why. The text stays the document.
void draw_particle_key_inspector(Workspace &workspace, const DocumentBase &document);

} // namespace opennova::editor
