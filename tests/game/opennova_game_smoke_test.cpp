// opennova-game's smoke run (apps/game, ADR 0077 d9): the minted mission set staged as a loose
// game root (common/mission_set.h), the app started on its infantry mission headless (--headless:
// SDL's dummy video and audio drivers, set as SDL's hints, and no GPU device) for 120 frames. It
// exits 0, having booted the mission through the engine's load executor and run logic ticks over
// the single player's session. No retail data.
#include "common/mission_set.h"
#include "common/temp_dir.h"

#include <base/io/file_io.h>
#include <base/proc/child_process.h>

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>

namespace {

int failures = 0;
#define CHECK(c)                                                                     \
	do {                                                                             \
		if (!(c)) {                                                                  \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                 \
			++failures;                                                              \
		}                                                                            \
	} while (0)

// The logic ticks the app's closing line reports ("opennova-game: <n> frames, <m> logic ticks"),
// -1 when the line is not there.
long long reported_ticks(const std::string &log) {
	const std::string tail = " logic ticks";
	const size_t end = log.find(tail);
	if (end == std::string::npos) return -1;
	size_t start = end;
	while (start > 0 && log[start - 1] >= '0' && log[start - 1] <= '9') --start;
	if (start == end) return -1;
	return std::atoll(log.substr(start, end - start).c_str());
}

} // namespace

int main(int argc, char **argv) {
	if (argc < 2) {
		std::printf("usage: opennova_game_smoke_test <opennova-game>\n");
		return 2;
	}
	using namespace opennova;
	test_mission_set::Root root("opennova_game_smoke");
	CHECK(root.ok());
	test_temp::TempDir work("opennova_game_smoke_cwd");
	const std::string log_path = work.file("opennova-game.log");

	proc::SpawnOptions spawn;
	spawn.argv = {argv[1], "--resource-dir", root.path(), "--mission", "infantry.bms", "/game", "jo",
			"--frames", "120", "--headless"};
	spawn.working_dir = work.root();
	spawn.output_path = log_path;
	proc::ChildProcess child;
	std::string error;
	CHECK(child.spawn(spawn, error));
	if (!error.empty()) std::printf("spawn: %s\n", error.c_str());
	// 120 frames at the frame lock's 16 ms are some 2 s; the mission's load comes first.
	const std::optional<uint32_t> code = child.wait(120000);
	std::string log;
	(void)io::read_file_text(log_path, log, error);
	std::printf("--- opennova-game ---\n%s--- end ---\n", log.c_str());
	CHECK(code.has_value());
	CHECK(code.has_value() && *code == 0);
	CHECK(reported_ticks(log) > 0);

	if (failures != 0) {
		std::printf("opennova_game_smoke_test: %d failure(s)\n", failures);
		return 1;
	}
	std::puts("opennova_game_smoke_test: ok");
	return 0;
}
