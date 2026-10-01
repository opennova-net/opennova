// The clip viewport a widget's own passes draw through (CWnd_SetClipRect):
// the draw-list ops the passes emitted, cut to the scaled clip rect.
// [orig: CWnd_SetClipRect @ 0x646210; CWnd_ApplyClipViewport @ 0x6472a0 —
//  x = (int64)(left * sx), y = (int64)(top * sy), w = (int64)((right - left)
//  * sx) + 1, h = (int64)((bottom - top) * sy) + 1, the D3D viewport the
//  screen-space quads clip to; CWnd_RestoreViewport @ 0x6473f0]

#include <runtime/menu/menu_frame_internal.h>

#include <algorithm>
#include <cstdint>

namespace opennova::menu {

namespace {

struct ClipBox {
	float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
};

float lerp(float a, float b, float t) { return a + (b - a) * t; }

// An axis-aligned quad cut to the box, its UV span cut in proportion; an
// empty result collapses the quad.
void clip_quad(MenuQuad &q, const ClipBox &box) {
	const float w = q.x1 - q.x0;
	const float h = q.y1 - q.y0;
	const float nx0 = std::max(q.x0, box.x0);
	const float nx1 = std::min(q.x1, box.x1);
	const float ny0 = std::max(q.y0, box.y0);
	const float ny1 = std::min(q.y1, box.y1);
	if (nx1 <= nx0 || ny1 <= ny0 || w == 0.0f || h == 0.0f) {
		q.x1 = q.x0;
		q.y1 = q.y0;
		return;
	}
	const float u0 = q.u0, u1 = q.u1, v0 = q.v0, v1 = q.v1;
	q.u0 = lerp(u0, u1, (nx0 - q.x0) / w);
	q.u1 = lerp(u0, u1, (nx1 - q.x0) / w);
	q.v0 = lerp(v0, v1, (ny0 - q.y0) / h);
	q.v1 = lerp(v0, v1, (ny1 - q.y0) / h);
	q.x0 = nx0;
	q.x1 = nx1;
	q.y0 = ny0;
	q.y1 = ny1;
}

// Liang-Barsky against the box; a segment wholly outside collapses.
void clip_line(MenuLine &l, const ClipBox &box) {
	const float dx = l.x1 - l.x0;
	const float dy = l.y1 - l.y0;
	float t0 = 0.0f;
	float t1 = 1.0f;
	const float p[4] = {-dx, dx, -dy, dy};
	const float q[4] = {l.x0 - box.x0, box.x1 - l.x0, l.y0 - box.y0, box.y1 - l.y0};
	for (int i = 0; i < 4; ++i) {
		if (p[i] == 0.0f) {
			if (q[i] < 0.0f) {
				l.x1 = l.x0;
				l.y1 = l.y0;
				l.color = 0;
				return;
			}
			continue;
		}
		const float t = q[i] / p[i];
		if (p[i] < 0.0f)
			t0 = std::max(t0, t);
		else
			t1 = std::min(t1, t);
	}
	if (t0 > t1) {
		l.x1 = l.x0;
		l.y1 = l.y0;
		l.color = 0;
		return;
	}
	const float x0 = l.x0 + t0 * dx, y0 = l.y0 + t0 * dy;
	l.x1 = l.x0 + t1 * dx;
	l.y1 = l.y0 + t1 * dy;
	l.x0 = x0;
	l.y0 = y0;
}

// A glyph quad cut to the box: the rows by the top/bottom edges, the columns
// by the top edge's span with the same cut applied to the bottom edge (the
// italic shear kept).
void clip_glyph(hud::GameFontQuad &g, const ClipBox &box) {
	const float h = g.y_bottom - g.y_top;
	const float w = g.x_top_right - g.x_top_left;
	if (h <= 0.0f || w <= 0.0f) return;
	const float ny0 = std::max(g.y_top, box.y0);
	const float ny1 = std::min(g.y_bottom, box.y1);
	const float nx0 = std::max(g.x_top_left, box.x0);
	const float nx1 = std::min(g.x_top_right, box.x1);
	if (ny1 <= ny0 || nx1 <= nx0) {
		g.x_top_right = g.x_top_left;
		g.x_bottom_right = g.x_bottom_left;
		g.y_bottom = g.y_top;
		return;
	}
	const float u0 = g.u0, u1 = g.u1, v0 = g.v0, v1 = g.v1;
	const float left_cut = nx0 - g.x_top_left;
	const float right_cut = g.x_top_right - nx1;
	g.u0 = lerp(u0, u1, left_cut / w);
	g.u1 = lerp(u0, u1, (w - right_cut) / w);
	g.v0 = lerp(v0, v1, (ny0 - g.y_top) / h);
	g.v1 = lerp(v0, v1, (ny1 - g.y_top) / h);
	g.x_top_left += left_cut;
	g.x_bottom_left += left_cut;
	g.x_top_right -= right_cut;
	g.x_bottom_right -= right_cut;
	g.y_top = ny0;
	g.y_bottom = ny1;
}

} // namespace

void MenuFrameCompiler::clip_ops_(int32_t first_op, const mnu::RectEdges &clip,
		const WalkScale &s) {
	ClipBox box;
	box.x0 = static_cast<float>(static_cast<int64_t>(static_cast<double>(clip.left) * s.x));
	box.y0 = static_cast<float>(static_cast<int64_t>(static_cast<double>(clip.top) * s.y));
	box.x1 = box.x0 +
			static_cast<float>(static_cast<int64_t>(static_cast<double>(clip.right - clip.left) * s.x) + 1);
	box.y1 = box.y0 +
			static_cast<float>(static_cast<int64_t>(static_cast<double>(clip.bottom - clip.top) * s.y) + 1);
	for (size_t i = static_cast<size_t>(first_op); i < draw_list_.draw_ops.size(); ++i) {
		const MenuDrawList::DrawOp &op = draw_list_.draw_ops[i];
		switch (op.kind) {
			case MenuDrawList::DrawOp::Kind::Quad:
				clip_quad(draw_list_.quads[static_cast<size_t>(op.index)], box);
				break;
			case MenuDrawList::DrawOp::Kind::Line:
				clip_line(draw_list_.lines[static_cast<size_t>(op.index)], box);
				break;
			case MenuDrawList::DrawOp::Kind::FontRun: {
				const MenuDrawList::FontRun &run = draw_list_.font_runs[static_cast<size_t>(op.index)];
				for (int32_t g = 0; g < run.count; ++g)
					clip_glyph(draw_list_.glyphs[static_cast<size_t>(run.first + g)], box);
				for (int32_t u = 0; u < run.underline_count; ++u) {
					hud::GameFontUnderline &line =
							draw_list_.underlines[static_cast<size_t>(run.underline_first + u)];
					if (line.y < box.y0 || line.y >= box.y1) {
						line.x1 = line.x0;
						continue;
					}
					line.x0 = std::max(line.x0, box.x0);
					line.x1 = std::min(line.x1, box.x1);
					if (line.x1 < line.x0) line.x1 = line.x0;
				}
				break;
			}
		}
	}
}

} // namespace opennova::menu
