#include <editor/preview/viewport_follow.h>

#include <base/io/strutil.h>

namespace opennova::editor {

bool FileStamps::note(const std::string &name, uint64_t stamp) {
	for (const FileStamp &seen : files_)
		if (strutil::iequals(seen.name, name)) return false;
	files_.push_back({name, stamp});
	return true;
}

bool FileStamps::add(const FileStamps &other) {
	bool added = false;
	for (const FileStamp &file : other.files_) added = note(file.name, file.stamp) || added;
	return added;
}

bool FileStamps::moved(const FileSource &files) const {
	for (const FileStamp &file : files_)
		if (files.stamp(file.name) != file.stamp) return true;
	return false;
}

bool StampedFiles::read(const std::string &name, std::vector<uint8_t> &out) const {
	const bool found = files_ && files_->read(name, out);
	stamps_.note(name, files_ ? files_->stamp(name) : 0);
	return found;
}

uint64_t StampedFiles::stamp(const std::string &name) const {
	const uint64_t stamp = files_ ? files_->stamp(name) : 0;
	stamps_.note(name, stamp);
	return stamp;
}

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
