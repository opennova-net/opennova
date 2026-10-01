// opennova-extract: write named entries out of a mounted game root.
//
//   opennova-extract --game <dir> [/exp <name>] [/game <code>] [/d]
//                    --out <dir> <name>...
//
// The root mounts exactly as a launch with those flags mounts it (mount_install,
// the flags read by the runtime's own parser): the retail .pff table with the
// optional expansion layered on top, `/d` selecting the loose-override policy
// (retail's /d), `/game <code>` the game profile's SCR policy. Each named entry is
// resolved through that mount — so the bytes written are the EFFECTIVE bytes the
// runtime serves (base + expansion override), decoded as its loader is served them
// (a shader as stored: its loader unwraps its own SCR form, vfs_loader_takes_stored)
// — and written to <out>/<basename>.
// Exit 0 when every entry was written, 1 when any entry was missing, 2 on a usage
// or mount error.
#include <base/io/strutil.h>
#include <base/resource_index/boot_policy.h>
#include <base/vfs/vfs.h>
#include <base/vfs/vfs_decode.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

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
	using opennova::strutil::iequals;
	const std::vector<std::string> args(argv + 1, argv + argc);
	std::string game_dir, out_dir;
	std::vector<std::string> names;
	// /d, /exp and /game are the runtime's launch flags: the walk gathers them, with
	// their values, apart from this tool's own options and their values, and the
	// runtime's parser reads them.
	std::vector<std::string> launch;
	for (size_t i = 0; i < args.size(); ++i) {
		const std::string &arg = args[i];
		// The value after a flag, into `into`.
		const auto value = [&](std::string &into) -> bool {
			if (i + 1 >= args.size()) return false;
			into = args[++i];
			return true;
		};
		// A launch flag with its value, kept for the runtime's parser.
		const auto launch_value = [&]() -> bool {
			if (i + 1 >= args.size()) return false;
			launch.push_back(arg);
			launch.push_back(args[++i]);
			return true;
		};
		if (arg == "--game") {
			if (!value(game_dir)) return usage("--game needs a directory");
		} else if (iequals(arg, opennova::kLaunchFlagExpansion)) {
			if (!launch_value()) return usage("/exp needs a name");
		} else if (iequals(arg, opennova::kLaunchFlagGame)) {
			if (!launch_value()) return usage("/game needs a code");
		} else if (iequals(arg, opennova::kLaunchFlagLooseOverride)) {
			launch.push_back(arg);
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
	const opennova::LaunchFlags flags = opennova::parse_launch_flags(launch);

	opennova::Vfs vfs;
	if (!opennova::mount_install(vfs, game_dir, flags)) {
		if (!vfs.last_error().empty())
			std::fprintf(stderr, "opennova-extract: could not mount %s: %s\n", game_dir.c_str(),
					vfs.last_error().c_str());
		else
			std::fprintf(stderr, "opennova-extract: no game data archives under %s\n", game_dir.c_str());
		return 2;
	}
	std::printf("mounted %s (expansion: %s, %s)\n", game_dir.c_str(),
			vfs.mounted_expansion().empty() ? "none" : vfs.mounted_expansion().c_str(),
			flags.loose_override ? "loose overrides" : "archives only");

	int missing = 0;
	for (const std::string &name : names) {
		std::vector<uint8_t> bytes;
		const bool read = opennova::vfs_loader_takes_stored(name) ? vfs.read_file_raw(name, bytes)
				: vfs.read_file(name, bytes);
		if (!read) {
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
