// opennova-extract: write named entries out of a mounted game root.
//
//   opennova-extract --game <dir> [/exp <name>] [/game <code>] [/d]
//                    --out <dir> <name>...
//
// The root mounts exactly as the game runtime mounts it: the retail .pff table
// with the optional expansion layered on top, `/d` selecting the loose-override
// policy (retail's /d), `/game <code>` the game profile's SCR policy. Each
// named entry is resolved through that mount — so the bytes written are the
// EFFECTIVE bytes the runtime serves (base + expansion override) — and written
// to <out>/<basename>. Exit 0 when every entry was written, 1 when any entry
// was missing, 2 on a usage or mount error.
#include <base/gameprofile/gameprofile.h>
#include <base/resource_index/boot_policy.h>
#include <base/resource_index/resource_index.h>
#include <base/vfs/vfs.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace opennova::gameprofile;

namespace {

int usage(const char *why) {
	if (why != nullptr) std::fprintf(stderr, "opennova-extract: %s\n", why);
	std::fprintf(stderr,
			"usage: opennova-extract --game <dir> [/exp <name>] [/game <code>] [/d] --out <dir> <name>...\n"
			"  --game <dir>   the retail install to mount (its .pff set, as the runtime does)\n"
			"  /exp <name>    the expansion to layer on top (e.g. revx02)\n"
			"  /game <code>   the game profile code selecting the SCR policy (default: version-detect)\n"
			"  /d             loose files override archive entries (retail's /d)\n"
			"  --out <dir>    where each entry is written, as <out>/<basename>\n"
			"  <name>...      the entries to extract (e.g. weapon.def M82_1st.adm joAmmoHit.ptl)\n");
	return 2;
}

std::string basename_of(const std::string &name) {
	const size_t cut = name.find_last_of("/\\");
	return cut == std::string::npos ? name : name.substr(cut + 1);
}

} // namespace

int main(int argc, char **argv) {
	std::string game_dir, expansion, game_code, out_dir;
	bool loose_override = false;
	std::vector<std::string> names;
	for (int i = 1; i < argc; ++i) {
		const std::string arg = argv[i];
		const auto value = [&](std::string &into) -> bool {
			if (i + 1 >= argc) return false;
			into = argv[++i];
			return true;
		};
		if (arg == "--game") {
			if (!value(game_dir)) return usage("--game needs a directory");
		} else if (arg == "/exp" || arg == "--exp") {
			if (!value(expansion)) return usage("/exp needs a name");
		} else if (arg == "/game") {
			if (!value(game_code)) return usage("/game needs a code");
		} else if (arg == "/d") {
			loose_override = true;
		} else if (arg == "--out") {
			if (!value(out_dir)) return usage("--out needs a directory");
		} else if (arg == "-h" || arg == "--help") {
			return usage(nullptr);
		} else if (!arg.empty() && arg[0] == '-') {
			return usage(("unknown option " + arg).c_str());
		} else {
			names.push_back(arg);
		}
	}
	if (game_dir.empty()) return usage("--game is required");
	if (out_dir.empty()) return usage("--out is required");
	if (names.empty()) return usage("name at least one entry to extract");

	opennova::ResourceIndex index;
	const opennova::VfsMountMode mode = loose_override
			? opennova::VfsMountMode::PackedWithLooseOverride
			: opennova::VfsMountMode::Packed;
	if (!index.scan(game_dir, expansion, mode, opennova::VfsArchiveDiscovery::RetailTable)) {
		std::fprintf(stderr, "opennova-extract: could not mount %s: %s\n", game_dir.c_str(),
				index.last_error().c_str());
		return 2;
	}
	if (!index.has_mounted_archive()) {
		std::fprintf(stderr, "opennova-extract: no game data archives under %s\n", game_dir.c_str());
		return 2;
	}
	index.set_scr_policy(gameprofile_scr_policy_for_code(game_code.c_str()));
	std::printf("mounted %s (expansion: %s, %s)\n", game_dir.c_str(),
			index.mounted_expansion().empty() ? "none" : index.mounted_expansion().c_str(),
			loose_override ? "loose overrides" : "archives only");

	int missing = 0;
	for (const std::string &name : names) {
		std::vector<uint8_t> bytes;
		if (!index.read_file(name, bytes)) {
			std::fprintf(stderr, "opennova-extract: %s is not on the mount\n", name.c_str());
			++missing;
			continue;
		}
		const std::string path = opennova::boot_path_join(out_dir, basename_of(name));
		std::ofstream out(path, std::ios::binary);
		if (!out) {
			std::fprintf(stderr, "opennova-extract: cannot write %s\n", path.c_str());
			return 2;
		}
		out.write(reinterpret_cast<const char *>(bytes.data()),
				static_cast<std::streamsize>(bytes.size()));
		std::printf("%s -> %s (%zu bytes)\n", name.c_str(), path.c_str(), bytes.size());
	}
	return missing == 0 ? 0 : 1;
}
