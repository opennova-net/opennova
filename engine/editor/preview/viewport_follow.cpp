#include <editor/preview/viewport_follow.h>

namespace opennova::editor {

const char *viewport_action_token(ViewportAction action) {
	switch (action) {
	case ViewportAction::Keep: return "keep";
	case ViewportAction::Rebuild: return "rebuild";
	case ViewportAction::Update: return "update";
	case ViewportAction::Clear: return "clear";
	}
	return "keep";
}

PreviewFollow::Found PreviewFollow::follow(const Key &key, bool moved, const FileSource &files,
		uint64_t generation) {
	if (!shows_ || moved || key != key_) return Found::Anew;
	// The stamps are read again only when the source says one may have moved, or a file was read
	// since they last were.
	if (generation == generation_ && !unchecked_) return Found::Same;
	generation_ = generation;
	unchecked_ = false;
	if (!files_.moved(files)) return Found::Same;
	// A failure is tried again from the start once a file it read moved (a rig's model written
	// again); a picture is made again from the files that moved.
	return failed_ ? Found::Anew : Found::Files;
}

void PreviewFollow::show(const Key &key, uint64_t generation) {
	shows_ = true;
	failed_ = false;
	key_ = key;
	generation_ = generation;
}

ViewportAction PreviewFollow::built(FileStamps files) {
	files_ = std::move(files);
	unchecked_ = false;
	return ViewportAction::Rebuild;
}

ViewportAction PreviewFollow::failed(FileStamps files) {
	failed_ = true;
	files_ = std::move(files);
	unchecked_ = false;
	return ViewportAction::Clear;
}

ViewportAction PreviewFollow::stop() {
	shows_ = false;
	failed_ = false;
	key_ = Key();
	files_.clear();
	unchecked_ = false;
	return ViewportAction::Clear;
}

} // namespace opennova::editor
