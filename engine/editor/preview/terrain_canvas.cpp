#include <editor/preview/terrain_canvas.h>

#include <editor/model/document_base.h>
#include <editor/preview/terrain_brush_spaces.h>
#include <editor/preview/terrain_viewport.h>
#include <editor/preview/viewport_model.h>

namespace opennova::editor {

void TerrainCanvas::follow(const ViewportModel &viewport, const ViewportContext &context, CanvasRequests &out) {
	const DocumentBase *document = context.input.document;
	viewport_ = &viewport;
	brush_.follow(context.input.view, document);
	subject_ = CanvasSubject();
	subject_.path = viewport.path();
	subject_.identity = document ? document->identity() : 0;
	gesture_.frame(subject_, out);
	orbit_.follow(viewport, context, out);
}

void TerrainCanvas::input(const ViewportContext &context, const CanvasInput &in, CanvasRequests &out) {
	if (brush_.live() && viewport_) {
		const auto &terrain = static_cast<const TerrainViewport &>(*viewport_);
		const CameraBrushSpace space(context, terrain.camera(), &terrain.followed_ground(context.input.view));
		if (brush_.input(gesture_, subject_, &space, in, out, &context)) return;
	}
	orbit_.input(context, in, out);
}

void TerrainCanvas::end(CanvasRequests &out) {
	gesture_.end(out);
	orbit_.end(out);
}

void TerrainCanvas::end_frame(CanvasRequests &out) {
	gesture_.end_frame(out);
	orbit_.end_frame(out);
}

OverlayList TerrainCanvas::shapes(const ViewportContext &context, const CanvasInput &in) const {
	OverlayList out = orbit_.shapes(context, in);
	if (brush_.live() && viewport_) {
		const auto &terrain = static_cast<const TerrainViewport &>(*viewport_);
		const CameraBrushSpace space(context, terrain.camera(), &terrain.followed_ground(context.input.view));
		brush_.shapes(&space, in, out);
	}
	return out;
}

CanvasCursor TerrainCanvas::cursor(const ViewportContext &context, const CanvasInput &in) const {
	return orbit_.cursor(context, in);
}

std::string TerrainCanvas::hint(const ViewportContext &context, const CanvasInput &in) const {
	if (!brush_.live() || !viewport_) return std::string();
	const auto &terrain = static_cast<const TerrainViewport &>(*viewport_);
	const MissionGround &ground = terrain.followed_ground(context.input.view);
	const CameraBrushSpace space(context, terrain.camera(), &ground);
	return brush_.hint(&space, in, &ground);
}

bool TerrainCanvas::brush_ground(const ViewportContext &context, const CanvasInput &in, double &x, double &y) const {
	if (!brush_.live() || !viewport_) return false;
	const auto &terrain = static_cast<const TerrainViewport &>(*viewport_);
	const CameraBrushSpace space(context, terrain.camera(), &terrain.followed_ground(context.input.view));
	TerrainPoint at;
	if (!brush_.ground(&space, in, at)) return false;
	x = at.x;
	y = at.y;
	return true;
}

std::string TerrainCanvas::hover_tip(const ViewportContext &context, const CanvasInput &in) const {
	return orbit_.hover_tip(context, in);
}

} // namespace opennova::editor
