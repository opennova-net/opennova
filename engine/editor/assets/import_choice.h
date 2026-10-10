#pragma once

#include <cstdint>
#include <string>

#include <editor/assets/asset_kind.h>

namespace opennova::editor {

// A file chosen to import (the import dialog's rows, a request's `imports`), named so since S13 A8
// apart from an import source, the project file an importer converts (AssetKind::ImportSource):
// a native file, loose, a named member of one PFF, or (install) the effective file of
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
struct ImportChoice {
	std::string path;
	std::string entry;    // empty for a loose file; the logical name of a member or an install's
	bool install = false; // `path` is the game install to mount
	// A loose file copied as the game's own, as it is and with no import record (a PNG stays
	// the texture a menu names): a file the import plan found beside the file that names it.
	// A loose file picked from the disk is the author's source otherwise.
	bool native = false;
	// An install's file the project gets under a name of its own (ADR 0046 S16, assets/install_view.h:
	// a file the game reads by an expansion's name, under the project's expansion's): that name, "" for
	// the member's own.
	std::string as;
	// The name the project gets: `as`, else the member's, else the loose file's.
	std::string name() const;
};

inline bool operator==(const ImportChoice &a, const ImportChoice &b) {
	return a.path == b.path && a.entry == b.entry && a.install == b.install && a.native == b.native && a.as == b.as;
}
inline bool operator!=(const ImportChoice &a, const ImportChoice &b) { return !(a == b); }
// An order over the same members, so a set of choices looks one up in log time (a whole install's
// nine thousand, review F7).
inline bool operator<(const ImportChoice &a, const ImportChoice &b) {
	if (a.path != b.path) return a.path < b.path;
	if (a.entry != b.entry) return a.entry < b.entry;
	if (a.install != b.install) return b.install;
	if (a.native != b.native) return b.native;
	return a.as < b.as;
}

// What a list of choices says of each beside its name (the UX round's project lane: the import
// dialog's chooser, the wire's choices): its kind by its name alone (file_kind_for_required_name:
// a .bin a string table unless its name says otherwise, nothing read) and its size as stored where it is.
struct ImportChoiceFacts {
	AssetKind kind = AssetKind::Unknown;
	uint64_t size = 0;
};

} // namespace opennova::editor
