#pragma once

#include <string>

namespace opennova::editor {

// A native file: loose, a named member of one PFF, or (retail) the effective file of
// a mounted game install, resolved the way a stock launch resolves it (the archive
// table's precedence; a loose file beside the archives is not what it reads, as it is
// only under /d). Importing makes an editable
// project copy; a scene text the Blender add-on writes (`.o3d`, `.o3a`) converts once
// into the native files it makes, the source not kept (editor/import/converter); a
// loose source an importer keeps converting (a PNG from the disk) is copied with its
// sidecar (editor/import/importer), while the game's own file (from the install or an
// archive) is copied as it is, with no record: a PNG of the game stays a texture. A value
// of its own (S13 V4), so a request and the view name one without the import machinery
// (asset_import.h).
struct ImportSource {
	std::string path;
	std::string entry;   // empty for a loose file; the logical name for a member or a retail file
	bool retail = false; // `path` is the game install to mount
	// A loose file copied as the game's own, as it is and with no import record (a PNG stays
	// the texture a menu names): a file the import plan found beside the file that names it.
	// A loose file picked from the disk is the author's source otherwise.
	bool native = false;
	std::string name() const;
};

inline bool operator==(const ImportSource &a, const ImportSource &b) {
	return a.path == b.path && a.entry == b.entry && a.retail == b.retail && a.native == b.native;
}
inline bool operator!=(const ImportSource &a, const ImportSource &b) { return !(a == b); }

} // namespace opennova::editor
