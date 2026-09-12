// The game.wac / server.wac / <mission>.wac layered load (engine/runtime/wac/
// wac_layered_load.h): the retail layer order compiled as ONE program, the
// absent-layers BMS-only case, and the two diagnostic policies — the game's
// lenient one and the dedicated golden host's strict one, where EVERY
// diagnostic is fatal because running a partial script is a known wire-parity
// failure. (The behavioral checks moved here from the deleted apps/nw_server
// startup lib's tests, ADR 0042 d3.)

#include <runtime/wac/wac_layered_load.h>
#include <runtime/wac/wac_system.h>
#include <runtime/world/world.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

namespace w = opennova::world;
namespace wc = opennova::wac;
namespace mission = opennova::mission;

int failures = 0;

#define CHECK(condition)                                                        \
	do {                                                                          \
		if (!(condition)) {                                                          \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #condition);           \
			++failures;                                                                \
		}                                                                           \
	} while (0)

class TempResourceRoot {
public:
	TempResourceRoot() {
		static uint64_t sequence = 0;
		const uint64_t stamp = static_cast<uint64_t>(
				std::chrono::steady_clock::now().time_since_epoch().count());
		path = std::filesystem::temp_directory_path() /
				("opennova-wac-layered-" + std::to_string(stamp) + "-" +
				 std::to_string(++sequence));
		std::error_code error;
		std::filesystem::create_directories(path, error);
		CHECK(!error);
	}

	~TempResourceRoot() {
		std::error_code ignored;
		std::filesystem::remove_all(path, ignored);
	}

	void write(const std::string &name, const std::string &source) const {
		std::ofstream output(path / name, std::ios::binary);
		output << source;
		CHECK(output.good());
	}

	// The embedder's mounted-file source over this directory (the shape the
	// kernel boot hands the layered load).
	mission::BootFileSource files() const {
		mission::BootFileSource source;
		const std::filesystem::path root = path;
		source.has_file = [root](const std::string &name) {
			std::error_code ignored;
			return std::filesystem::exists(root / name, ignored);
		};
		source.read_file = [root](const std::string &name,
									  std::vector<uint8_t> &out) {
			std::ifstream input(root / name, std::ios::binary);
			if (!input) return false;
			out.assign(std::istreambuf_iterator<char>(input),
					std::istreambuf_iterator<char>());
			return true;
		};
		return source;
	}

	std::filesystem::path path;
};

void test_wac_layers_execute_in_retail_order() {
	TempResourceRoot root;
	root.write("game.wac", "if never then set(v1,1) endif\n");
	root.write("server.wac", "if never then set(v1,2) endif\n");
	root.write("sample.wac", "if never then set(v1,3) endif\n");

	w::World world;
    // A mission only advances while a human is in the world - retail holds the
    // WAC tick on `wac_var_humans || !wac_var_ticks` (World::script_may_advance).
	world.cached.humans = 1;
	wc::WacSystem wac;
	std::string error;
	CHECK(wc::wac_layered_load(wac, root.files(), "sample", &world.registry,
				  /*strict_diagnostics=*/true, error) ==
			wc::WacLayeredLoadStatus::kLoaded);
	CHECK(error.empty());
	world.add_system(&wac);
	world.load_systems();
	CHECK(wac.execute_initial(world));
	CHECK(wac.runs() == 1);
	CHECK(world.script.vars.get_mission(1) == 3);
}

void test_absent_layers_are_the_valid_bms_only_mission() {
	TempResourceRoot root;
	wc::WacSystem wac;
	std::string error;
	CHECK(wc::wac_layered_load(wac, root.files(), "sample", nullptr,
				  /*strict_diagnostics=*/false, error) ==
			wc::WacLayeredLoadStatus::kAbsent);
	CHECK(error.empty());
	CHECK(wc::wac_layered_load(wac, root.files(), "sample", nullptr,
				  /*strict_diagnostics=*/true, error) ==
			wc::WacLayeredLoadStatus::kAbsent);
	CHECK(error.empty());
}

void test_retail_two_word_else_if_chain_is_clean_under_strict() {
	// The shipped retail scripts chain alternatives as the two-word
	// `Else if ... then` closed by the chain's ONE endif (00TRg.wac's form).
	// It must parse as the chain — diagnostics-free, so the strict host can
	// boot the training missions, and WITHOUT nesting the second if into the
	// else branch (nesting would swallow the endif and fold every trailing
	// statement into the never-taken alternative).
	TempResourceRoot root;
	root.write("sample.wac",
			"if never then\n"
			"\t\tset(v1,1)\n"
			"else if never then\n"
			"\t\tset(v1,2)\n"
			"endif\n"
			"if never then\n"
			"\t\tset(v2,9)\n"
			"endif\n");
	w::World world;
	world.cached.humans = 1;
	wc::WacSystem wac;
	std::string error;
	CHECK(wc::wac_layered_load(wac, root.files(), "sample", &world.registry,
				  /*strict_diagnostics=*/true, error) ==
			wc::WacLayeredLoadStatus::kLoaded);
	CHECK(error.empty());
	world.add_system(&wac);
	world.load_systems();
	CHECK(wac.execute_initial(world));
	CHECK(world.script.vars.get_mission(1) == 1); // the taken then-branch
	CHECK(world.script.vars.get_mission(2) == 9); // the trailing statement stayed top-level
}

void test_strict_mode_blocks_what_the_game_only_warns_on() {
	// The shared compiler labels recoverable/legacy issues as WARNINGS —
	// an unknown command, an out-of-range V# clamp — so the game's lenient
	// policy keeps retail scripts running. Both are lossy compiles, so the
	// golden host's strict policy must refuse them rather than diverge on
	// the wire.
	for (const char *source : {"if never then bogus_command_xyz(1) endif\n",
				 "if never then set(v9999,1) endif\n"}) {
		TempResourceRoot root;
		root.write("sample.wac", source);
		wc::WacSystem lenient, strict;
		std::string error;
		CHECK(wc::wac_layered_load(lenient, root.files(), "sample", nullptr,
					  /*strict_diagnostics=*/false, error) ==
				wc::WacLayeredLoadStatus::kLoaded);
		CHECK(error.empty());
		CHECK(wc::wac_layered_load(strict, root.files(), "sample", nullptr,
					  /*strict_diagnostics=*/true, error) ==
				wc::WacLayeredLoadStatus::kBlocked);
		CHECK(error.find("failed to compile cleanly") != std::string::npos);
	}
}

void test_run_files_share_order_symbols_and_diagnostics() {
    TempResourceRoot root;
    root.write("game.wac", "var shared\nset(shared,5) run extra inc(shared)\n");
    root.write("extra.wac", "add(shared,2) run leaf\n");
    root.write("leaf.wac", "add(shared,3)\n");
    root.write("server.wac", "store(v1)\n");
    root.write("sample.wac", "if eq(shared,11) then inc(v2) endif\n");
    w::World world;
    wc::WacSystem wac;
    std::string error;
    CHECK(wc::wac_layered_load(wac, root.files(), "sample", &world.registry,
            true, error) == wc::WacLayeredLoadStatus::kLoaded);
    CHECK(error.empty());
    world.add_system(&wac);
    world.load_systems();
    CHECK(wac.execute_initial(world));
    CHECK(world.script.vars.get_mission(1) == 11);
    CHECK(world.script.vars.get_mission(2) == 1);
    CHECK(wac.program().source_names.size() == 5);

    root.write("leaf.wac", "run game\n");
    CHECK(wc::wac_layered_load(wac, root.files(), "sample", &world.registry,
            false, error) == wc::WacLayeredLoadStatus::kBlocked);
    CHECK(error.find("RUN nesting") != std::string::npos);
    root.write("leaf.wac", "if never then run extra endif\n");
    CHECK(wc::wac_layered_load(wac, root.files(), "sample", &world.registry,
            false, error) == wc::WacLayeredLoadStatus::kBlocked);
    CHECK(error.find("inside blocks") != std::string::npos);
    root.write("leaf.wac", "run missing\n");
    // Third-level RUN fails its depth gate before attempting any file read.
    CHECK(wc::wac_layered_load(wac, root.files(), "sample", &world.registry,
            false, error) == wc::WacLayeredLoadStatus::kBlocked);
}

} // namespace

// `gloop(G_x)` is the retail Unknown Group leg, not a structural error: the
// `(` is the group token, the compile keeps running with group 0, and the
// game's lenient policy loads the script (the strict host still refuses every
// diagnostic). [orig: Script_Compile tokenizer @0x4f32e0..0x4f3464; the GLOOP
//  operand resolve @0x4f365d..0x4f3693 -> WacScript_ResolveParameter group leg
//  @0x4f30fc]
void test_parenthesised_gloop_is_the_unknown_group_leg() {
	TempResourceRoot root;
	root.write("sample.wac", "gloop(G_ai) inc(v1) end\ninc(v2)\n");
	w::World world;
	world.registry.configure_pool(0, 4);
	wc::WacSystem lenient, strict;
	std::string error;
	CHECK(wc::wac_layered_load(lenient, root.files(), "sample", &world.registry,
				  /*strict_diagnostics=*/false, error) ==
			wc::WacLayeredLoadStatus::kLoaded);
	CHECK(error.empty());
	world.add_system(&lenient);
	world.load_systems();
	CHECK(lenient.execute_initial(world));
	CHECK(world.script.vars.get_mission(1) == 0); // group 0 is empty: the body never runs
	CHECK(world.script.vars.get_mission(2) == 1); // the script kept running past the error
	CHECK(wc::wac_layered_load(strict, root.files(), "sample", &world.registry,
				  /*strict_diagnostics=*/true, error) ==
			wc::WacLayeredLoadStatus::kBlocked);
	CHECK(error.find("failed to compile cleanly") != std::string::npos);
	bool unknown_group = false;
	for (const wc::Diagnostic &d : lenient.program().diagnostics)
		if (!d.error && d.message.find("Unknown Group") != std::string::npos) unknown_group = true;
	CHECK(unknown_group);
}

int main() {
    test_run_files_share_order_symbols_and_diagnostics();
	test_parenthesised_gloop_is_the_unknown_group_leg();
	test_wac_layers_execute_in_retail_order();
	test_absent_layers_are_the_valid_bms_only_mission();
	test_retail_two_word_else_if_chain_is_clean_under_strict();
	test_strict_mode_blocks_what_the_game_only_warns_on();
	std::printf(failures ? "WAC LAYERED LOAD TEST FAILED (%d)\n"
	                     : "wac layered load test passed\n",
	            failures);
	return failures ? 1 : 0;
}
