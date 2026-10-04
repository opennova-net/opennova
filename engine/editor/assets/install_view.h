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
// decoded with that game's key, with `/exp <expansion>` when the spec names one: the expansion's two
// archives before the base's, the lowest slot first [orig: PFF_OpenAllArchives @ 0x4a4310 over the
// table @ 0x829f90; Expansion_LoadAssets @ 0x4a48d6..0x4a48ed]. An expansion that does not mount is
// refused, never quietly the base game (the game itself falls back [orig: @ 0x4a4767/@ 0x4a4775], which
// an import must not take for the expansion asked). Beside the archives' files, the loose files of the
// kinds the game ships loose and reads by path (install_loose_kind), each where no archive has the name:
// with an expansion, its folder's videos and music banks first, which the game reads there before the
// root's [orig: UI_CreateMenuBinkVideos @ 0x54b590; Game_PlayIntroVideos @ 0x5637a0; Expansion_LoadAssets
// "expansion\%s\M%s.sbf" @ 0x4a4906, "expansion\%s\G%s.sbf" @ 0x4a4936], then the root's. Under an
// expansion the base game's music pairs (MENUMUS.*, GAMEMUS.*) are not listed: the game reads the
// expansion's in their place [orig: Expansion_LoadAssets @ 0x4a4906..0x4a494a].
//
// With `project_expansion` (the project builds as expansion <b>), the files the game reads by an
// expansion's own name are listed under the project's (expansion_files.h): the installed expansion's
// (<e>.bin, M<e>.*, G<e>.*, <e>L.lwf, <e>.lwf) as <b>.bin, M<b>.*, ...; on the base game its music pairs
// as M<b>.* and G<b>.*, which the game reads in their place under /exp <b>. The expansion's version
// text is never listed (a build makes its own), nor the player's own files (assets/player_files.h).
struct InstallSpec {
	std::string root;              // the install's folder
	std::string game;              // the gameprofile code its archives are keyed by
	std::string expansion;         // the installed expansion mounted (/exp), "" for the base game
	std::string project_expansion; // the project's own expansion name, "" for a standalone project

	bool operator==(const InstallSpec &other) const {
		return root == other.root && game == other.game && expansion == other.expansion &&
		       project_expansion == other.project_expansion;
	}
	bool operator!=(const InstallSpec &other) const { return !(*this == other); }
};

// The view of an install a project imports from: its game, `/exp` the expansion it builds on, its own
// expansion's name.
InstallSpec install_spec(const std::string &root, const ProjectDocument &document);
// The base game's view of the same install: no expansion, no renames.
InstallSpec base_install_spec(const std::string &root, const ProjectDocument &document);

struct InstallFile {
	enum class Layer { Base, Expansion };
	std::string name;       // the name the project gets (renamed where `project_expansion` says so)
	std::string member;     // the name the install serves it by (an archive's member, a loose file's)
	Layer layer = Layer::Base;
	std::string loose_path; // a loose file's path on the disk; "" for an archive's member
};

class InstallView {
public:
	// False with `error` saying why: the folder holds none of the game's archives, or the expansion
	// asked does not mount (no expansion\<e>\<e>.pff). A view opened again forgets what it held.
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

// The import choice of an install's file (ImportChoice::install): the member it is served by, and the
// name the project gets where it differs (ImportChoice::as).
ImportChoice install_choice(const std::string &root, const InstallFile &file);

} // namespace opennova::editor
