#include <editor/preview/text_burst.h>

#include <editor/model/edit.h>
#include <editor/preview/canvas_gesture.h>
#include <editor/session/request_factories.h>

namespace opennova::editor {

bool TextBurst::continues(const std::string &path, size_t from, size_t removed) const {
	return sent_ && path == path_ && from <= at_ && at_ <= from + removed;
}

uint64_t TextBurst::token() {
	if (!token_) token_ = next_edit_gesture();
	return token_;
}

void TextBurst::sent(const std::string &path, size_t at, double now) {
	path_ = path;
	at_ = at;
	last_ = now;
	sent_ = true;
}

bool TextBurst::quiet(double now) const {
	return sent_ && now - last_ >= kQuietSeconds;
}

void TextBurst::end(CanvasRequests &out) {
	if (sent_) out.request(request::end_edit(path_));
	drop();
}

void TextBurst::drop() {
	token_ = 0;
	sent_ = false;
	at_ = 0;
}

} // namespace opennova::editor
