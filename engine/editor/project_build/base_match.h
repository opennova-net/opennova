#pragma once

#include <cstdint>
#include <map>
#include <string>

#include <base/io/json.h>
#include <editor/assets/install_view.h>

namespace opennova::editor {

// What an expansion's base game serves its build (ADR 0046 S16, lean packing): the install as a stock
// launch of the project's game mounts it, with no expansion (InstallView over base_install_spec's
// spec), and the loose files beside its archives. A file of the project identical to what the base
// serves under its name is the base's to serve, and the expansion's build leaves it out: the game
// reads it through the archives below the expansion's pair [orig: PFF_OpenAllArchives @ 0x4a4310,
// slots 2..4 after the pair], or, for a video, from the install's folder where the expansion's has
// none [orig: UI_CreateMenuBinkVideos @ 0x54b5ff..0x54b74a; Game_PlayIntroVideos @
// 0x5637d7..0x563848]. A file is compared with an archive's member as stored and as the game's loaders
// are served it (an imported file is stored decoded, the base's member may be stored in the SCR form:
// either is the same file to the game).
//
// Every hash read is kept (the build cache's `base` section, the project's build_cache.json), by the
// install, the game and the stamps (size and last write) of what it came from (the boot archives
// together for a member, the file for a loose one), so the next build reads none of the base's bytes
// while the install is unchanged.
struct BaseCopy {
	uint64_t size = 0;   // as stored
	uint64_t raw = 0;    // FNV-1a 64 of the bytes as stored
	uint64_t served = 0; // of the bytes as the game's loader is served them
	uint64_t served_size = 0;
};

class BaseMatch {
public:
	// The install at `install` mounted for the game `game`: false, `error` saying why, when it does not
	// mount (no folder, none of the game's archives).
	bool open(const std::string &install, const std::string &game, std::string &error);

	// The copy of `name` the base's archives serve: false when they hold none. `read_bytes` grows by what
	// was read to hash it (0 when the cache held it).
	bool archive_copy(const std::string &name, BaseCopy &out, uint64_t &read_bytes);
	// The install folder's loose file `name` (compared without case, as the game's file system does):
	// false when there is none.
	bool root_copy(const std::string &name, BaseCopy &out, uint64_t &read_bytes);

	// The cache as the build cache's `base` section keeps it: read before the comparison (one of
	// another install or game reads as empty), and what it holds after: every copy asked for this
	// time (a copy no longer asked for leaves it).
	void load(const io::JsonValue &section);
	io::JsonValue save() const;

private:
	struct Cached {
		std::string stamp; // the size and last write of what the copy came from
		BaseCopy copy;
	};
	std::string stamp_of(const std::string &path);
	bool cached(const std::string &key, const std::string &stamp, BaseCopy &out);

	std::string install_;
	std::string game_;
	InstallView view_;
	std::string archives_stamp_;               // the boot archives' stamps together
	std::map<std::string, std::string> loose_; // normalized name -> the install folder's file
	std::map<std::string, Cached> cache_;      // read from the cache
	std::map<std::string, Cached> kept_;       // asked for this time
};

} // namespace opennova::editor
