#pragma once

#include <memory>

#include <editor/ui/document_views.h>

namespace opennova::editor {

// The Terrain pane (ADR 0046 S25; CONTEXT.md "Terrain pane"): a mission's or a terrain's, beside its 3D view while the
// document view's `terrain` is on (exclusive with the Logic pane, sharing its split). Its header says where the
// brush's edits go and why (session/terrain_family's words), its tabs are the tool families (Sculpt, Paint, Tiles), a
// tool pressed again going to None; its parts (ui/terrain_pane_parts.h) draw the brush's shape and each family's
// values. Every change is a request (set_workspace's terrain_brush), as the wire makes it.
std::unique_ptr<DocumentPane> make_terrain_pane();

} // namespace opennova::editor
