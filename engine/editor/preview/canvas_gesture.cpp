#include <editor/preview/canvas_gesture.h>

#include <cmath>

namespace opennova::editor {

CanvasJoin canvas_join(const CanvasKeys &keys) {
	if (keys.ctrl)
		return CanvasJoin::Toggle;
	return keys.shift ? CanvasJoin::Add : CanvasJoin::Replace;
}

void CanvasGesture::press(const std::string &path, CanvasPoint at, CanvasRequests &out) {
	end(out);
	mode_ = Mode::Press;
	path_ = path;
	from_ = at;
}

bool CanvasGesture::move(CanvasPoint at) {
	if (mode_ != Mode::Press || dragging_)
		return false;
	if (std::hypot(at.x - from_.x, at.y - from_.y) < kDragThreshold)
		return false;
	dragging_ = true;
	return true;
}

void CanvasGesture::nudge(const std::string &path, CanvasRequests &out) {
	end(out);
	mode_ = Mode::Nudge;
	path_ = path;
}

uint64_t CanvasGesture::token() {
	if (!token_)
		token_ = next_edit_gesture();
	return token_;
}

bool CanvasGesture::release(CanvasRequests &out) {
	if (mode_ != Mode::Press)
		return false;
	const bool click = !dragging_;
	end(out);
	return click;
}

void CanvasGesture::end(CanvasRequests &out) {
	if (mode_ != Mode::None && sent_)
		out.end_edit(path_);
	mode_ = Mode::None;
	dragging_ = false;
	sent_ = false;
	from_ = CanvasPoint();
	token_ = 0;
	path_.clear();
}

void CanvasGesture::frame(const std::string &path, CanvasRequests &out) {
	drawn_ = true;
	if (mode_ != Mode::None && path != path_)
		end(out);
}

void CanvasGesture::end_frame(CanvasRequests &out) {
	if (!drawn_)
		end(out);
	drawn_ = false;
}

} // namespace opennova::editor
