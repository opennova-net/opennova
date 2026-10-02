#include <editor/preview/canvas_gesture.h>

#include <cmath>

#include <editor/session/request_factories.h>

namespace opennova::editor {

CanvasJoin canvas_join(const CanvasKeys &keys) {
	if (keys.ctrl)
		return CanvasJoin::Toggle;
	return keys.shift ? CanvasJoin::Add : CanvasJoin::Replace;
}

SelectMode select_mode(CanvasJoin join) {
	switch (join) {
		case CanvasJoin::Add:
			return SelectMode::Add;
		case CanvasJoin::Toggle:
			return SelectMode::Toggle;
		case CanvasJoin::Replace:
			break;
	}
	return SelectMode::Replace;
}

void CanvasGesture::press(const CanvasSubject &subject, CanvasPoint at, CanvasRequests &out) {
	end(out);
	mode_ = Mode::Press;
	subject_ = subject;
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

void CanvasGesture::nudge(const CanvasSubject &subject, CanvasRequests &out) {
	end(out);
	mode_ = Mode::Nudge;
	subject_ = subject;
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
		out.request(request::end_edit(subject_.path));
	mode_ = Mode::None;
	dragging_ = false;
	sent_ = false;
	from_ = CanvasPoint();
	token_ = 0;
	subject_ = CanvasSubject();
}

void CanvasGesture::frame(const CanvasSubject &subject, CanvasRequests &out) {
	drawn_ = true;
	if (mode_ != Mode::None && subject != subject_)
		end(out);
}

void CanvasGesture::end_frame(CanvasRequests &out) {
	if (!drawn_)
		end(out);
	drawn_ = false;
}

} // namespace opennova::editor
