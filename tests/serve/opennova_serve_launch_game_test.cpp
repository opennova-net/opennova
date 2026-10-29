// opennova-serve chooses the game it speaks and mounts as the game's launch chooses it
// (boot_policy.h launch_product, ADR 0067 d2): /game when given, else the folder's UPDATE.WIZ
// (JOPTR.CMD JO, DFX2.CMD DFX2), a folder with no wizard being OpenNova's own (its own title);
// a folder whose wizard names a game OpenNova does not play, with no /game, runs nothing.
// The folders and their UPDATE.WIZ text are minted here.

#include "server.h"

#include <base/version/one_build.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

using namespace opennova;
namespace fs = std::filesystem;

int failures = 0;
#define CHECK(c)                                                                               \
	do {                                                                                       \
		if (!(c)) {                                                                            \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                           \
			++failures;                                                                        \
		}                                                                                      \
	} while (0)

std::string folder(const fs::path &root, const char *name, const char *wizard) {
	const fs::path dir = root / name;
	std::error_code ec;
	fs::create_directories(dir, ec);
	if (wizard != nullptr) std::ofstream((dir / "UPDATE.WIZ").string(), std::ios::binary) << wizard;
	return dir.string();
}

serve::ServeOptions options_for(const std::string &dir, const std::string &game) {
	serve::ServeOptions options;
	options.resource_dir = dir;
	options.game = game;
	return options;
}

void the_launch_picks_the_game() {
	const fs::path root = fs::temp_directory_path() /
			("opennova_serve_launch_game_" +
					std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	const std::string jo = folder(root, "jo", "[HEADER]\r\nCMD=JOPTR.CMD\r\nPRODUCT_LOGIN=JOTRqq\r\n");
	const std::string dfx2 = folder(root, "dfx2", "[HEADER]\r\nCMD=DFX2.CMD\r\nPRODUCT_LOGIN=DFX2\r\n");
	const std::string own = folder(root, "own", nullptr);
	const std::string bhd = folder(root, "bhd", "[HEADER]\r\nCMD=DFBHD.CMD\r\n");

	// The folder's wizard names the game when /game does not.
	CHECK(serve::Server(options_for(jo, "")).game_code() == "jo");
	CHECK(serve::Server(options_for(dfx2, "")).game_code() == "dfx2");
	// A folder with no wizard is OpenNova's own, run as its own title.
	{
		serve::Server server(options_for(own, ""));
		CHECK(server.game_code() == "acidgambit" && !server.foreign_folder());
	}
	// /game wins over the folder, its case folded.
	CHECK(serve::Server(options_for(dfx2, "JO")).game_code() == "jo");
	CHECK(serve::Server(options_for(jo, "dfx2")).game_code() == "dfx2");
	// A foreign folder runs nothing without /game, and with it plays the named game; a /game
	// the launch does not know is left to the title's own refusal (its start's error).
	CHECK(serve::Server(options_for(bhd, "")).foreign_folder());
	{
		serve::Server server(options_for(bhd, "jo"));
		CHECK(!server.foreign_folder() && server.game_code() == "jo");
	}
	{
		serve::Server server(options_for(own, "nosuchgame"));
		CHECK(!server.foreign_folder() && server.game_code() == "nosuchgame");
	}
	// The line it logs as it starts and CMD VERSION posts: the launch's game, the release its
	// folder holds (its command file's VERSION), ONE's build and the protocol its title speaks; no
	// release named where the folder holds none, no protocol for a title whose protocol is
	// unwitnessed.
	const std::string one = one_build().label();
	const std::string dot = " \xC2\xB7 ";
	CHECK(serve::Server(options_for(jo, "")).version_line() ==
			"Joint Operations" + dot + one + dot + "JO protocol 16");
	CHECK(serve::Server(options_for(dfx2, "")).version_line() ==
			"Delta Force Xtreme 2" + dot + one + dot + "DFX2 protocol 16");
	std::ofstream((fs::path(own) / "ACIDGAMBIT.CMD").string(), std::ios::binary)
			<< "[HEADER]\r\nVERSION=0.1.4\r\n\r\n[COMMAND]\r\n";
	CHECK(serve::Server(options_for(own, "")).version_line() ==
			"OP: Acid Gambit 0.1.4" + dot + one + dot + "JO protocol 16");
	CHECK(serve::Server(options_for(own, "jodemo")).version_line() ==
			"Joint Operations (Demo)" + dot + one);
	std::error_code ec;
	fs::remove_all(root, ec);
	std::printf("  /game, else the folder's UPDATE.WIZ, else OpenNova's own; a foreign folder needs /game\n");
}

} // namespace

int main() {
	std::printf("opennova_serve_launch_game:\n");
	the_launch_picks_the_game();
	if (failures != 0) {
		std::printf("opennova_serve_launch_game: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("opennova_serve_launch_game: OK\n");
	return 0;
}
