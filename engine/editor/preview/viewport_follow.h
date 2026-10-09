#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <base/vfs/file_source.h>
#include <base/vfs/file_stamps.h>

namespace opennova::editor {

// The files a viewport's picture read are a FileStamps (base/vfs/file_stamps.h), each once with the
// stamp it had then (ADR 0046 S13 V5): a menu screen's stylesheets, string tables, fonts and
// textures, a model's textures, a rig's model, table and clips; a picture is made again when one
// of them moves its stamp. What a device read is noted through a StampedFiles.

// What a viewport's device does after a follow (ADR 0046 S13 V5).
enum class ViewportAction : uint8_t {
	Keep, // nothing it draws changed
	Rebuild, // its picture made again from the viewport (a menu screen configured again, a model's
			 // scene built again), then the state applied
	Update, // the picture stands: the state applied again (a model's level, registers and rig);
			// what changed only the overlays show
	Clear, // what it built dropped (the viewport's status says why there is nothing)
};
// "keep", "rebuild", "update", "clear".
const char *viewport_action_token(ViewportAction action);

// The follow every viewport kind shares (ADR 0046 S13 V5): the state it shows (its key), whether
// that state failed (a document the game could not read, a screen it lacks, a rig model that does
// not read: kept failed until the key or the document moves, never tried again every frame), and
// the files its picture read, whose stamps it checks only when the file source's generation moved.
class PreviewFollow {
public:
	// What a viewport shows, besides its document's state (the follow's ChangeClass, viewport_model.h):
	// the part of the document drawn (a menu's screen row; 0 the whole) and the serial of the state
	// the picture is made from (a menu's options; 0 for a kind that applies its state to a picture
	// that stands).
	struct Key {
		uint64_t part = 0;
		uint64_t state = 0;
		bool operator==(const Key &other) const { return part == other.part && state == other.state; }
		bool operator!=(const Key &other) const { return !(*this == other); }
	};

	// What a follow found (follow()).
	enum class Found : uint8_t {
		Same, // it shows `key` of the document as it was, and nothing its picture read moved (or
			  // what it shows failed and none of the files the failure read moved): Keep
		Files, // it shows `key` of the document as it was, and a file its picture read moved: the
			   // picture made again from the files (a menu screen configured again, a model's scene
			   // built again over the document read before)
		Anew, // another key, the document moved, a failure whose files moved, or it shows nothing:
			  // the caller makes what it shows anew, then says show() and built(), or failed(), or
			  // stop()
	};
	// The follow of `key` over the document, `moved` when the document changed since the last
	// follow in what the picture reads of it (its ChangeClass, viewport_model.h, as the kind reads
	// it: a change set naming only what the picture does not read is no move, S13 V8). The stamps of
	// the files its picture read are compared only when the file source's `generation` moved, which
	// it does whenever a stamp may have, or when the device read a file since the last comparison
	// (S13 V6: a build runs its units between two follows, so a file read after one comparison is
	// compared at the next).
	Found follow(const Key &key, bool moved, const FileSource &files, uint64_t generation);
	// `key` is what it shows now (the files' generation as the caller read them); the files its
	// picture read stand until built(), failed() or stop() replaces them (a picture that stands, a
	// model's scene its state is applied to again, keeps the textures it read).
	void show(const Key &key, uint64_t generation);
	// What it shows was made, its picture reading `files`: Rebuild.
	ViewportAction built(FileStamps files);
	// The game could not read the state it shows (`files`: what the attempt read, a rig's model
	// that did not read): kept so until the key or the document moves, or one of `files` does:
	// Clear.
	ViewportAction failed(FileStamps files = FileStamps());
	// Nothing to show (no project, no document, no screen): the key forgotten. Clear.
	ViewportAction stop();
	// The device read `files` as it made the picture (its report: a model's textures, read by a
	// build's units over the frames since the last report); one not read before is compared at the
	// next follow, whatever the generation (it may have moved after it was read and before it was
	// reported).
	void read(const FileStamps &files) { unchecked_ = files_.add(files) || unchecked_; }
	// What moved of the files its picture read leaves the picture as it is (a stylesheet variable
	// the picture does not name, S13 V8): each takes the stamp it has now, so the move is not found
	// again.
	void restamp(const FileSource &files) { files_.restamp(files); }
	// True when a file its picture read moved its stamp since the files' generation it last read
	// (`generation` the source's now), or since it was read when it was not compared yet: what a
	// caller making its state anew over a picture that stands asks before show(), a model's texture
	// moved in the same follow as an edit of it.
	bool files_moved(const FileSource &files, uint64_t generation) const {
		return shows_ && (generation != generation_ || unchecked_) && files_.moved(files);
	}
	bool shows() const { return shows_; }
	bool is_failed() const { return failed_; }
	const FileStamps &files() const { return files_; }

private:
	bool shows_ = false;
	bool failed_ = false;
	Key key_;
	uint64_t generation_ = 0;
	bool unchecked_ = false; // a file read since the last comparison of the stamps
	FileStamps files_;
};

} // namespace opennova::editor
