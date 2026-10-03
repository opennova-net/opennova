#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <base/vfs/vfs.h>
#include <editor/assets/import_choice.h>
#include <editor/project/project_document.h>

namespace opennova::editor {

// A game install as the game is served it (ADR 0046 S16), the one view behind every read of an install
// the editor makes: the import's listing and its reads, the import plan's install origin, the fold of
// the game's own data (session/original_files) and the Problems Import fixes.
//
// The install is mounted as a stock launch of the project's game mounts it (mount_install: the
// witnessed archive table, no /d, read alone while one of its archives is mounted), its payloads
// decoded with that game's key. Beside its archives' files, the root's loose files of the kinds the
// game ships loose and reads by path (install_loose_kind), each where no archive has the name. The
// player's own files are never listed (assets/player_files.h).
struct InstallSpec {
	std::string root; // the install's folder
	std::string game; // the gameprofile code its archives are keyed by
};

// The view of an install a project imports from.
InstallSpec install_spec(const std::string &root, const ProjectDocument &document);

struct InstallFile {
	std::string name;       // the name the project gets
	std::string member;     // the name the install serves it by (an archive's member, a loose file's)
	std::string loose_path; // a loose file's path on the disk; "" for an archive's member
};

class InstallView {
public:
	// False with `error` saying why: the folder holds none of the game's archives. A view opened again
	// forgets what it held.
	bool open(const InstallSpec &spec, std::string &error);
	bool is_open() const { return open_; }
	const InstallSpec &spec() const { return spec_; }

	// Every file, each name once: the archives' in their names' order, then the loose files in theirs.
	const std::vector<InstallFile> &files() const { return files_; }
	// The file the project gets as `project_name` (compared without case, as the game compares
	// names); null when the view has none.
	const InstallFile *find(const std::string &project_name) const;
	// The file as the game's loader is served it: an archive's member decoded (read_served), a loose
	// file's bytes. False when it cannot be read.
	bool read(const InstallFile &file, std::vector<uint8_t> &out) const;
	// Its size as stored (an archive's entry, the file on the disk), without reading it.
	uint64_t size(const InstallFile &file) const;
	// The mount behind it (its archives alone), for a caller that walks or resolves them itself.
	const Vfs &vfs() const { return vfs_; }

private:
	InstallSpec spec_;
	bool open_ = false;
	Vfs vfs_;
	std::vector<InstallFile> files_;
	std::map<std::string, size_t> by_name_; // normalized project name -> files_ index
};

// The import choice of an install's file (ImportChoice::install).
ImportChoice install_choice(const std::string &root, const InstallFile &file);

} // namespace opennova::editor
