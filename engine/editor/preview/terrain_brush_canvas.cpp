#include <editor/preview/terrain_brush_canvas.h>

#include <editor/model/document_base.h>
#include <editor/preview/terrain_brush_shapes.h>
#include <editor/session/terrain_tools_parts.h>
#include <editor/session/view/session_view.h>

namespace opennova::editor {

void TerrainBrushHalf::follow(const SessionView &view, const DocumentBase *document) {
	brush_ = view.workspace.terrain_brush;
	path_ = document ? document->path() : std::string();
	live_ = document && terrain_brush_live(view, *document);
}

bool TerrainBrushHalf::input(CanvasGesture &gesture, const CanvasSubject &subject, const TerrainBrushSpace *space,
                             const CanvasInput &in, CanvasRequests &out) {
	// S25 C1 strokes here; until then the host keeps its input.
	(void)gesture;
	(void)subject;
	(void)space;
	(void)in;
	(void)out;
	return false;
}

void TerrainBrushHalf::shapes(const TerrainBrushSpace *space, const CanvasInput &in, OverlayList &out) const {
	(void)space;
	(void)in;
	(void)out;
}

std::string TerrainBrushHalf::hint(const TerrainBrushSpace *space, const CanvasInput &in) const {
	(void)space;
	(void)in;
	return std::string();
}

} // namespace opennova::editor
