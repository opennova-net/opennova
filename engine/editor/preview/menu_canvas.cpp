#include <editor/preview/menu_canvas.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <editor/documents/mnu_document.h>
#include <editor/preview/menu_render_check.h>

namespace opennova::editor {

namespace {

constexpr NodeKind kWindowKind = node_kind(MenuKind::Window);
constexpr float kNoteMark = 7.0f; // a note's corner mark, pixels along each edge

bool has(const std::vector<NodeAddress> &windows, const NodeAddress &window) {
	return std::find(windows.begin(), windows.end(), window) != windows.end();
}

// The picture's scale: device pixels per design unit.
float scale_x_of(const CanvasInput &in) {
	return float(in.width) / float(menu::kMenuDesignWidth);
}
float scale_y_of(const CanvasInput &in) {
	return float(in.height) / float(menu::kMenuDesignHeight);
}

// A compiled widget's rect on the picture; false for none (index -1, or out of range).
bool widget_on_picture(const MenuCanvasFrame &frame, int index, float sx, float sy, CanvasPoint &a,
		CanvasPoint &b) {
	mnu::RectEdges rect{};
	if (index < 0 || !frame.compiler->widget_rect(index, *frame.state, &rect))
		return false;
	menu_picture_rect(rect, sx, sy, a, b);
	return true;
}

bool inside(CanvasPoint at, CanvasPoint a, CanvasPoint b) {
	return at.x >= a.x && at.x < b.x && at.y >= a.y && at.y < b.y;
}

// Which way a handle's side of the rect faces the inside on each axis: +1 the left or top
// edge, -1 the right or bottom edge, 0 a handle in the middle of that axis.
int inward_x(LayoutHandle handle) {
	switch (handle) {
		case LayoutHandle::Left:
		case LayoutHandle::TopLeft:
		case LayoutHandle::BottomLeft:
			return 1;
		case LayoutHandle::Right:
		case LayoutHandle::TopRight:
		case LayoutHandle::BottomRight:
			return -1;
		default:
			return 0;
	}
}
int inward_y(LayoutHandle handle) {
	switch (handle) {
		case LayoutHandle::Top:
		case LayoutHandle::TopLeft:
		case LayoutHandle::TopRight:
			return 1;
		case LayoutHandle::Bottom:
		case LayoutHandle::BottomLeft:
		case LayoutHandle::BottomRight:
			return -1;
		default:
			return 0;
	}
}

// Whether `v` is on a handle at `p` along one axis: `reach` either way along its edge
// (`inward` 0), else `reach` outside the rect and at most `in` inside it.
bool within_handle(float v, float p, int inward, float reach, float in) {
	if (inward == 0)
		return std::fabs(v - p) <= reach;
	const float t = (v - p) * float(inward);
	return t >= -reach && t <= in;
}

std::string rect_text(const mnu::RectEdges &r) {
	char text[112];
	std::snprintf(text, sizeof(text), "left %d, top %d, right %d, bottom %d (%d x %d)", r.left,
			r.top, r.right, r.bottom, r.right - r.left, r.bottom - r.top);
	return text;
}

} // namespace

void menu_canvas_select(MenuCanvasFrame &frame, const NodeAddress &primary,
		const std::vector<NodeAddress> &selected) {
	const NodeId screen = frame.screen ? frame.screen->id : 0;
	frame.record = primary;
	frame.selected = selected;
	frame.primary = window_holding(*frame.document, primary, screen);
	frame.windows = selected_windows(*frame.document, primary, selected, screen);
}

MenuClipboard menu_canvas_clipboard(const MenuCanvasFrame &frame, bool full) {
	return menu_clipboard(*frame.document, frame.screen->id, frame.record, frame.selected, full);
}

void menu_picture_rect(
		const mnu::RectEdges &rect, float scale_x, float scale_y, CanvasPoint &a, CanvasPoint &b) {
	a = CanvasPoint{ menu::menu_scaled_edge(rect.left, scale_x),
		menu::menu_scaled_edge(rect.top, scale_y) };
	b = CanvasPoint{ menu::menu_scaled_edge(rect.right, scale_x),
		menu::menu_scaled_edge(rect.bottom, scale_y) };
}

CanvasPoint menu_handle_point(LayoutHandle handle, CanvasPoint a, CanvasPoint b) {
	const float mx = (a.x + b.x) * 0.5f, my = (a.y + b.y) * 0.5f;
	switch (handle) {
		case LayoutHandle::Left:
			return CanvasPoint{ a.x, my };
		case LayoutHandle::Right:
			return CanvasPoint{ b.x, my };
		case LayoutHandle::Top:
			return CanvasPoint{ mx, a.y };
		case LayoutHandle::Bottom:
			return CanvasPoint{ mx, b.y };
		case LayoutHandle::TopLeft:
			return a;
		case LayoutHandle::TopRight:
			return CanvasPoint{ b.x, a.y };
		case LayoutHandle::BottomLeft:
			return CanvasPoint{ a.x, b.y };
		case LayoutHandle::BottomRight:
			return b;
		case LayoutHandle::Move:
			break;
	}
	return CanvasPoint{ mx, my };
}

bool menu_handle_at(CanvasPoint at, CanvasPoint a, CanvasPoint b, LayoutHandle &out) {
	const float reach = kMenuHandleSize * 0.5f + kMenuHandleSlop;
	const float in_x = std::min(reach, (b.x - a.x) * 0.25f);
	const float in_y = std::min(reach, (b.y - a.y) * 0.25f);
	for (const LayoutHandle handle : kMenuHandles) {
		const CanvasPoint p = menu_handle_point(handle, a, b);
		if (within_handle(at.x, p.x, inward_x(handle), reach, in_x) &&
				within_handle(at.y, p.y, inward_y(handle), reach, in_y)) {
			out = handle;
			return true;
		}
	}
	return false;
}

CanvasCursor menu_handle_cursor(LayoutHandle handle) {
	switch (handle) {
		case LayoutHandle::Left:
		case LayoutHandle::Right:
			return CanvasCursor::ResizeEW;
		case LayoutHandle::Top:
		case LayoutHandle::Bottom:
			return CanvasCursor::ResizeNS;
		case LayoutHandle::TopLeft:
		case LayoutHandle::BottomRight:
			return CanvasCursor::ResizeNWSE;
		case LayoutHandle::TopRight:
		case LayoutHandle::BottomLeft:
			return CanvasCursor::ResizeNESW;
		case LayoutHandle::Move:
			break;
	}
	return CanvasCursor::Move;
}

NodeAddress menu_selected_window_at(
		const MenuCanvasFrame &frame, CanvasPoint at, float scale_x, float scale_y) {
	NodeAddress found;
	int front = -1;
	for (const NodeAddress &window : frame.windows) {
		const int index = frame.document->window_index(window);
		CanvasPoint a, b;
		if (!widget_on_picture(frame, index, scale_x, scale_y, a, b) || !inside(at, a, b))
			continue;
		if (window == frame.primary)
			return window;
		if (index > front) {
			front = index;
			found = window;
		}
	}
	return found;
}

MenuPress menu_canvas_press(const MenuCanvasFrame &frame, const CanvasInput &in) {
	MenuPress press;
	press.scale_x = scale_x_of(in);
	press.scale_y = scale_y_of(in);
	press.from = press.to = CanvasPoint{ in.mouse.x / press.scale_x, in.mouse.y / press.scale_y };
	press.join = canvas_join(in.keys);
	if (!frame.current)
		return press; // an old picture maps no index
	const MnuDocument &document = *frame.document;
	const menu::MenuFrameCompiler &compiler = *frame.compiler;
	press.pick =
			compiler.hit_widget(*frame.state, in.mouse.x, in.mouse.y, press.scale_x, press.scale_y);
	const NodeId picked =
			press.pick >= 0 ? document.window_at(*frame.screen, size_t(press.pick)) : 0;
	const NodeAddress picked_window =
			picked ? NodeAddress{ frame.screen->id, kWindowKind, picked } : NodeAddress();
	// The screen's background: no window, or a root window that is not selected.
	const bool background = !picked ||
			(compiler.widget_parent(press.pick) < 0 && !has(frame.windows, picked_window));
	if (press.join != CanvasJoin::Replace) {
		press.marquee = background;
		return press;
	}
	const int primary = frame.primary.child ? document.window_index(frame.primary) : -1;
	CanvasPoint a, b;
	if (widget_on_picture(frame, primary, press.scale_x, press.scale_y, a, b) &&
			menu_handle_at(in.mouse, a, b, press.handle)) {
		press.resize = true;
		press.window = frame.primary;
		if (!layout_press(document, press.window, press.handle, frame.windows, compiler,
					*frame.state, press.layout))
			press.window = NodeAddress();
		return press;
	}
	// A selected window moves every selected one; the window picked, not selected, moves alone.
	press.window = menu_selected_window_at(frame, in.mouse, press.scale_x, press.scale_y);
	if (!press.window.child && background) {
		press.marquee = true;
		return press;
	}
	if (!press.window.child)
		press.window = picked_window;
	if (press.window.child &&
			!layout_press(document, press.window, LayoutHandle::Move, frame.windows, compiler,
					*frame.state, press.layout))
		press.window = NodeAddress();
	return press;
}

NodeAddress menu_window_at(const MenuCanvasFrame &frame, const CanvasInput &in) {
	if (!frame.current)
		return NodeAddress();
	const int under = frame.compiler->hit_widget(
			*frame.state, in.mouse.x, in.mouse.y, scale_x_of(in), scale_y_of(in));
	const NodeId id = under >= 0 ? frame.document->window_at(*frame.screen, size_t(under)) : 0;
	return id ? NodeAddress{ frame.screen->id, kWindowKind, id } : NodeAddress();
}

std::vector<NodeAddress> menu_marquee_windows(
		const MenuCanvasFrame &frame, CanvasPoint from, CanvasPoint to) {
	const menu::MenuFrameCompiler &compiler = *frame.compiler;
	const float left = std::min(from.x, to.x), right = std::max(from.x, to.x);
	const float top = std::min(from.y, to.y), bottom = std::max(from.y, to.y);
	std::vector<NodeAddress> touched;
	for (int index = 0; index < compiler.widget_count(); ++index) {
		mnu::RectEdges rect{};
		if (compiler.widget_parent(index) < 0 || !compiler.widget_shown(index, *frame.state) ||
				!compiler.widget_rect(index, *frame.state, &rect) || rect.right <= rect.left ||
				rect.bottom <= rect.top)
			continue;
		const NodeId id = frame.document->window_at(*frame.screen, size_t(index));
		if (id && float(rect.left) < right && float(rect.right) > left &&
				float(rect.top) < bottom && float(rect.bottom) > top)
			touched.push_back({ frame.screen->id, kWindowKind, id });
	}
	return touched;
}

bool menu_canvas_escape(const MenuCanvasFrame &frame, CanvasRequests &out) {
	if (!frame.record.child || frame.record.row != frame.screen->id)
		return false;
	const std::vector<NodeAddress> owners = frame.document->ancestors(frame.record);
	if (!owners.empty())
		out.select(frame.document->path(), owners.back(), CanvasJoin::Replace);
	return true;
}

void menu_canvas_arrange(const MenuCanvasFrame &frame, ArrangeOp op, CanvasRequests &out) {
	if (!frame.current)
		return;
	std::vector<Edit> edits;
	if (arrange_edits(*frame.document, frame.windows, frame.primary, op, *frame.compiler,
				*frame.state, edits, nullptr) &&
			!edits.empty())
		out.edits(frame.document->path(), std::move(edits));
}

// --- MenuCanvas ------------------------------------------------------------------------------

void MenuCanvas::follow(const MenuCanvasFrame &frame, CanvasRequests &out) {
	gesture_.frame(frame.document ? frame.document->path() : std::string(), out);
	if (gesture_.nudging() && nudged_ != frame.windows)
		gesture_.end(out);
	if (!gesture_.pressed())
		press_ = MenuPress();
}

void MenuCanvas::input(const MenuCanvasFrame &frame, const CanvasInput &in, CanvasRequests &out) {
	if (in.pressed && !in.middle) {
		gesture_.press(frame.document->path(), in.mouse, out);
		press_ = menu_canvas_press(frame, in);
	}
	if (!gesture_.pressed())
		return;
	if (!in.down)
		release_(frame, out);
	else
		move_(frame, in, out);
}

// One sample of a drag: the marquee's box, or the step from where it began, planned and sent
// when it moved (the edits of one drag share its token, so the drag is one undo step; a move
// writes every window it takes in one batch).
void MenuCanvas::move_(const MenuCanvasFrame &frame, const CanvasInput &in, CanvasRequests &out) {
	if (!gesture_.dragging()) {
		if (!press_.window.child && !press_.marquee)
			return;
		if (!gesture_.move(in.mouse))
			return;
		// The window a drag holds is the primary one (its handles follow it); the others the move
		// takes stay selected.
		if (!press_.marquee && press_.window != frame.primary)
			out.select(gesture_.path(), press_.window,
					press_.layout.windows.size() > 1 ? CanvasJoin::Add : CanvasJoin::Replace);
	}
	if (press_.marquee) {
		press_.to = CanvasPoint{ in.mouse.x / press_.scale_x, in.mouse.y / press_.scale_y };
		return;
	}
	const CanvasPoint from = gesture_.from();
	const int dx = int(std::lround((in.mouse.x - from.x) / press_.scale_x));
	const int dy = int(std::lround((in.mouse.y - from.y) / press_.scale_y));
	const int grid = frame.snap && !in.keys.alt ? kLayoutGrid : 0;
	if (dx == press_.dx && dy == press_.dy && grid == press_.grid)
		return;
	if (!frame.compiler)
		return; // no solve until the picture is back
	press_.dx = dx;
	press_.dy = dy;
	press_.grid = grid;
	std::vector<Edit> edits;
	if (layout_press_edits(*frame.document, press_.layout, *frame.compiler, dx, dy, grid,
				gesture_.token(), edits) &&
			!edits.empty()) {
		out.edits(gesture_.path(), std::move(edits));
		gesture_.sent();
	}
}

void MenuCanvas::release_(const MenuCanvasFrame &frame, CanvasRequests &out) {
	const std::string &path = gesture_.path();
	if (press_.marquee && gesture_.dragging()) {
		// What the box touches: the selection (the last one the primary), or added to it with
		// Shift or Ctrl; a box that touches nothing selects the screen.
		if (frame.current) {
			const std::vector<NodeAddress> touched =
					menu_marquee_windows(frame, press_.from, press_.to);
			if (press_.join == CanvasJoin::Replace && touched.empty())
				out.select(path, { frame.screen->id, frame.screen->kind, 0 }, CanvasJoin::Replace);
			const bool replace = press_.join == CanvasJoin::Replace;
			for (size_t i = 0; i < touched.size(); ++i)
				out.select(path, touched[i],
						i == 0 && replace ? CanvasJoin::Replace : CanvasJoin::Add);
		}
	} else if (!gesture_.dragging() && !press_.resize && press_.pick >= 0 && frame.current) {
		const NodeId id = frame.document->window_at(*frame.screen, size_t(press_.pick));
		if (id)
			out.select(path, { frame.screen->id, kWindowKind, id }, press_.join);
	}
	gesture_.release(out);
	press_ = MenuPress();
}

void MenuCanvas::nudge(const MenuCanvasFrame &frame, int dx, int dy, CanvasRequests &out) {
	if ((!dx && !dy) || gesture_.pressed() || !frame.primary.child || !frame.compiler)
		return;
	if (!gesture_.nudging()) {
		// The nudge starts where the picture shows the windows: it must be the document's own.
		if (!frame.current ||
				!layout_press(*frame.document, frame.primary, LayoutHandle::Move, frame.windows,
						*frame.compiler, *frame.state, nudge_))
			return;
		gesture_.nudge(frame.document->path(), out);
		nudged_ = frame.windows;
		nudge_dx_ = 0;
		nudge_dy_ = 0;
	}
	nudge_dx_ += dx;
	nudge_dy_ += dy;
	std::vector<Edit> edits;
	if (layout_press_edits(*frame.document, nudge_, *frame.compiler, nudge_dx_, nudge_dy_, 0,
				gesture_.token(), edits) &&
			!edits.empty()) {
		out.edits(gesture_.path(), std::move(edits));
		gesture_.sent();
	}
}

void MenuCanvas::end(CanvasRequests &out) {
	gesture_.end(out);
	press_ = MenuPress();
}

void MenuCanvas::end_frame(CanvasRequests &out) {
	gesture_.end_frame(out);
	if (!gesture_.pressed())
		press_ = MenuPress();
}

OverlayList MenuCanvas::shapes(const MenuCanvasFrame &frame, const CanvasInput &in) const {
	OverlayList list;
	if (!frame.current)
		return list; // what the picture maps, only while it is the document's own
	const MnuDocument &document = *frame.document;
	const menu::MenuFrameCompiler &compiler = *frame.compiler;
	const float sx = scale_x_of(in), sy = scale_y_of(in);
	// A mark on every window that has a note: a small triangle in its top-left corner.
	std::vector<int> marked;
	for (const menu::MenuFrameNote &note : frame.notes) {
		mnu::RectEdges rect{};
		if (note.widget < 0 ||
				std::find(marked.begin(), marked.end(), note.widget) != marked.end() ||
				!compiler.widget_rect(note.widget, *frame.state, &rect))
			continue;
		marked.push_back(note.widget);
		const CanvasPoint corner{ menu::menu_scaled_edge(rect.left, sx),
			menu::menu_scaled_edge(rect.top, sy) };
		list.marker(corner, OverlayGlyph::Corner, kNoteMark, OverlayRole::Note);
	}
	// Hover: the front-most shown widget, the last drawn (the game's own hit test), outlined thin.
	const bool idle = in.hovered && !in.panning && !gesture_.pressed();
	const int under = idle ? compiler.hit_widget(*frame.state, in.mouse.x, in.mouse.y, sx, sy) : -1;
	CanvasPoint a, b;
	if (widget_on_picture(frame, under, sx, sy, a, b))
		list.rect(a, b, OverlayRole::Hover);
	// The other selected windows, outlined thin: a press inside one moves them all.
	for (const NodeAddress &window : frame.windows) {
		const int index = window == frame.primary ? -1 : document.window_index(window);
		if (widget_on_picture(frame, index, sx, sy, a, b))
			list.rect(a, b, OverlayRole::Selected);
	}
	// The marquee's box.
	if (press_.marquee && gesture_.dragging()) {
		const float left = std::min(press_.from.x, press_.to.x) * sx;
		const float top = std::min(press_.from.y, press_.to.y) * sy;
		const float right = std::max(press_.from.x, press_.to.x) * sx;
		const float bottom = std::max(press_.from.y, press_.to.y) * sy;
		list.rect(CanvasPoint{ left, top }, CanvasPoint{ right, bottom }, OverlayRole::Marquee);
	}
	// The primary window, on the pixels the game draws its rect on, with its eight handles.
	const int primary = frame.primary.child ? document.window_index(frame.primary) : -1;
	if (widget_on_picture(frame, primary, sx, sy, a, b)) {
		list.rect(a, b, OverlayRole::Selected, 2.0f);
		for (const LayoutHandle handle : kMenuHandles)
			list.marker(menu_handle_point(handle, a, b), OverlayGlyph::Square,
					kMenuHandleSize * 0.5f, OverlayRole::Normal);
	}
	return list;
}

CanvasCursor MenuCanvas::cursor(const MenuCanvasFrame &frame, const CanvasInput &in) const {
	if (!frame.current)
		return CanvasCursor::Default;
	const float sx = scale_x_of(in), sy = scale_y_of(in);
	const bool idle = in.hovered && !in.panning && !gesture_.pressed();
	CanvasCursor cursor = CanvasCursor::Default;
	if (idle && menu_selected_window_at(frame, in.mouse, sx, sy).child)
		cursor = CanvasCursor::Move;
	// The primary window: the cursor says what a press there does.
	const int primary = frame.primary.child ? frame.document->window_index(frame.primary) : -1;
	CanvasPoint a, b;
	if (!widget_on_picture(frame, primary, sx, sy, a, b))
		return cursor;
	LayoutHandle handle = LayoutHandle::Move;
	if (gesture_.pressed() && press_.window == frame.primary)
		cursor = menu_handle_cursor(press_.handle);
	else if (idle && menu_handle_at(in.mouse, a, b, handle))
		cursor = menu_handle_cursor(handle);
	else if (idle && inside(in.mouse, a, b))
		cursor = CanvasCursor::Move;
	return cursor;
}

std::string MenuCanvas::hover_tip(const MenuCanvasFrame &frame, const CanvasInput &in) const {
	if (!frame.current || !in.hovered || in.panning || in.pressed || gesture_.pressed())
		return std::string();
	const menu::MenuFrameCompiler &compiler = *frame.compiler;
	const int under = compiler.hit_widget(
			*frame.state, in.mouse.x, in.mouse.y, scale_x_of(in), scale_y_of(in));
	mnu::RectEdges rect{};
	if (under < 0 || !compiler.widget_rect(under, *frame.state, &rect))
		return std::string();
	std::string tip = compiler.widget_name(under);
	const int kind = compiler.widget_kind(under);
	if (kind >= 0)
		tip += std::string(" (") + mnu::window_type_name(static_cast<mnu::WindowType>(kind)) + ")";
	tip += "\nOn the screen: " + rect_text(rect);
	mnu::RectEdges parent{};
	const int owner = compiler.widget_parent(under);
	if (compiler.widget_rect(owner, *frame.state, &parent)) {
		const mnu::RectEdges local{ rect.left - parent.left, rect.top - parent.top,
			rect.right - parent.left, rect.bottom - parent.top };
		tip += "\nIn " + compiler.widget_name(owner) + ": " + rect_text(local);
	}
	for (const menu::MenuFrameNote &note : frame.notes)
		if (note.widget == under)
			tip += "\n- " + menu_note_message(note);
	return tip;
}

} // namespace opennova::editor
