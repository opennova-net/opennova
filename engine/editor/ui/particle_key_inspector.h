#pragma once

#include <editor/ui/workspace.h>

namespace opennova::editor {

class DocumentBase;

// The particle type's part of the Inspector (ADR 0046 S23 B, documents/particle_keys): a block of the file picked
// from its list, each key it writes a field typed as the game's reader takes it (its words and range in its
// tooltip), and the keys of its kind it lacks to add. Enter sets a value: that one value's span of the text
// replaced, or the key's line put before the block's closing brace, one undo step; a value the reader would take
// otherwise is refused with why. The text stays the document. Each name a value holds (a pdefs member, a child_id, a
// curve's table, a graphic's texture: the graph's edges at their spans, extract_from_text, read again only when the
// document moves) is a text_reference_field (ui/reference_fields.h: completion, its state's dot and fixes, Pick, a
// Files row dropped, its jumps), a value of that one name alone drawn in its box's place; a name set replaces its
// span alone, the value checked as the reader takes it (particle_name_edit). An id's row (a name the block defines)
// has Rename everywhere (F2 over its box) and Find usages.
void draw_particle_key_inspector(Workspace &workspace, const DocumentBase &document);

} // namespace opennova::editor
