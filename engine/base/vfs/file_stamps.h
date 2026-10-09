// What a build read from a FileSource, by stamp (header-only): hot-reload bookkeeping over
// base/vfs/file_source.h. Not a port: nothing here is witnessed engine behaviour.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <base/io/strutil.h>
#include <base/vfs/file_source.h>

namespace opennova {

// A file read, with its stamp when it was read.
struct FileStamp {
	std::string name;
	uint64_t stamp = 0;
};

// The files something made from a FileSource read, each once with the stamp it had then
// (ADR 0046 S13 V5): a menu screen's stylesheets, string tables, fonts and textures, a model's
// textures, a rig's model, table and clips. What they made is made again when one of them moves
// its stamp.
class FileStamps {
public:
	// `name` read at `stamp`; a name read before (in any case) keeps its first stamp. True when it
	// was not read before.
	bool note(const std::string &name, uint64_t stamp) {
		for (const FileStamp &seen : files_)
			if (strutil::iequals(seen.name, name)) return false;
		files_.push_back({name, stamp});
		return true;
	}
	// Every file `other` read, as note() takes each; true when one was not read before.
	bool add(const FileStamps &other) {
		bool added = false;
		for (const FileStamp &file : other.files_) added = note(file.name, file.stamp) || added;
		return added;
	}
	// True when a file read has another stamp in `files` now; moved_but: a file other than those
	// `except` names (in any case).
	bool moved(const FileSource &files) const {
		for (const FileStamp &file : files_)
			if (files.stamp(file.name) != file.stamp) return true;
		return false;
	}
	bool moved_but(const FileSource &files, const std::vector<std::string> &except) const {
		for (const FileStamp &file : files_) {
			bool excepted = false;
			for (const std::string &name : except) excepted = excepted || strutil::iequals(name, file.name);
			if (!excepted && files.stamp(file.name) != file.stamp) return true;
		}
		return false;
	}
	// Each file read takes the stamp it has in `files` now (a move that leaves what was made as it
	// is).
	void restamp(const FileSource &files) {
		for (FileStamp &file : files_) file.stamp = files.stamp(file.name);
	}
	const std::vector<FileStamp> &files() const { return files_; }
	bool empty() const { return files_.empty(); }
	void clear() { files_.clear(); }

private:
	std::vector<FileStamp> files_;
};

// A file source that notes every name read through it (its stamp as it was read): what a device
// read while it built a model (its textures, a flipbook frame loaded when first drawn), what a rig
// read (its table and its clips).
class StampedFiles : public FileSource {
public:
	explicit StampedFiles(std::shared_ptr<const FileSource> files) : files_(std::move(files)) {}
	bool read(const std::string &name, std::vector<uint8_t> &out) const override {
		const bool found = files_ && files_->read(name, out);
		stamps_.note(name, files_ ? files_->stamp(name) : 0);
		return found;
	}
	uint64_t stamp(const std::string &name) const override {
		const uint64_t stamp = files_ ? files_->stamp(name) : 0;
		stamps_.note(name, stamp);
		return stamp;
	}
	const FileStamps &stamps() const { return stamps_; }

private:
	std::shared_ptr<const FileSource> files_;
	mutable FileStamps stamps_;
};

} // namespace opennova
