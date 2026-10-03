// The game.wac / server.wac / <mission>.wac layered load (engine/runtime/wac/
// wac_layered_load.h): the retail layer order compiled as ONE program, the
// absent-layers BMS-only case, and retail's diagnostic policy: the program
// always installs with its first error recorded.

#include <formats/particle/parser.h>
#include <formats/rtxt/rtxt.h>
#include <formats/wac/bytecode.h>
#include <runtime/particle/effect_catalog_names.h>
#include <runtime/wac/mission_effect_interns.h>
#include <runtime/wac/wac_layered_load.h>
#include <runtime/wac/wac_system.h>
#include <runtime/world/world.h>

#include "common/file_io.h"
#include "common/test_paths.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
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
				(test_paths_unique("opennova-wac-layered") + "-" + std::to_string(stamp) + "-" +
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

	// One RTXT table with a single "Text" section.
	void write_text(const std::string &name,
			const std::vector<std::pair<std::string, std::string>> &rows) const {
		opennova::rtxt::File table;
		table.sections.push_back({"Text", uint32_t(rows.size())});
		for (const auto &[key, text] : rows) table.entries.push_back({key, text, {}, 0});
		std::vector<uint8_t> bytes;
		std::string error;
		CHECK(opennova::rtxt::write(table, bytes, error));
		write(name, std::string(bytes.begin(), bytes.end()));
	}

	// The embedder's mounted-file source over this directory (the shape the
	// kernel boot hands the layered load). Names match case-insensitively, as
	// the mounted file system does: RUN asks for the upper-cased token.
	mission::BootFileSource files() const {
		mission::BootFileSource source;
		const std::filesystem::path root = path;
		const auto find = [root](const std::string &name) {
			std::error_code ignored;
			for (const auto &entry : std::filesystem::directory_iterator(root, ignored)) {
				const std::string file = entry.path().filename().string();
				if (file.size() == name.size() &&
						std::equal(file.begin(), file.end(), name.begin(), [](char a, char b) {
							return std::tolower(static_cast<unsigned char>(a)) ==
									std::tolower(static_cast<unsigned char>(b));
						}))
					return entry.path();
			}
			return std::filesystem::path();
		};
		source.has_file = [find](const std::string &name) { return !find(name).empty(); };
		source.read_file = [find](const std::string &name, std::vector<uint8_t> &out) {
			const std::filesystem::path file = find(name);
			return !file.empty() && test_io::read_file(file.string(), out);
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
    // WAC tick on `g_WacVarHumans || !g_WacVarTicks` (World::script_may_advance).
	world.cached.humans = 1;
	wc::WacSystem wac;
	CHECK(wc::wac_layered_load(wac, root.files(), "sample", &world) ==
			wc::WacLayeredLoadStatus::kLoaded);
	world.add_system(&wac);
	world.load_systems();
	CHECK(wac.execute_initial(world));
	CHECK(wac.runs() == 1);
	CHECK(world.script.vars.get_mission(1) == 3);
}

void test_absent_layers_are_the_valid_bms_only_mission() {
	TempResourceRoot root;
	wc::WacSystem wac;
	CHECK(wc::wac_layered_load(wac, root.files(), "sample", nullptr) ==
			wc::WacLayeredLoadStatus::kAbsent);
}

void test_retail_two_word_else_if_chain_nests() {
	// The shipped retail scripts chain alternatives as the two-word
	// `Else if ... then` closed by ONE endif (00TRg.wac's form). Retail's
	// compiler reads ELSE and IF as two keywords: the IF opens a block inside
	// the else branch, the endif closes only that block, the trailing IF lands
	// inside the never-taken branch too, and the file ends with the first
	// error "Missing END", which the game runs past.
	// [orig: Script_Compile @0x4F4566..0x4F4619 (ELSE), @0x4F498F..0x4F4A57
	// (IF), @0x4F5669..0x4F56A5 (Missing END)]
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
	CHECK(wc::wac_layered_load(wac, root.files(), "sample", &world) ==
			wc::WacLayeredLoadStatus::kLoaded);
	CHECK(wac.program().diagnostics.size() == 1 &&
			wac.program().diagnostics[0].message == "Missing END");
	world.add_system(&wac);
	world.load_systems();
	CHECK(wac.execute_initial(world));
	CHECK(world.script.vars.get_mission(1) == 1); // the taken then-branch
	CHECK(world.script.vars.get_mission(2) == 0); // the trailing IF sits in the else branch
}

void test_retail_first_errors_always_load() {
	// Retail never refuses a script: an unknown command, an out-of-range V#
	// or a literal the mounted FX catalog misses (here the root has none) is
	// a first error and the program runs [orig: WacScript_InitAndLoad
	// @0x4F94A8 / @0x4F950E / @0x4F9597 ignore the returns, @0x4F976B
	// executes].
	for (const char *source : {"if never then bogus_command_xyz(1) endif\n",
				 "if never then set(v9999,1) endif\n",
				 "if never then fx2tgt(nosuch, 1) endif\n"}) {
		TempResourceRoot root;
		root.write("sample.wac", source);
		wc::WacSystem wac;
		CHECK(wc::wac_layered_load(wac, root.files(), "sample", nullptr) ==
				wc::WacLayeredLoadStatus::kLoaded);
		CHECK(!wac.program().diagnostics.empty());
	}
}

// A GLOOP operand ORs the dword behind its resolved address into the GROUP
// word while the compile runs: a named value, a V# and an event read what the
// world and the installed program hold before the load resets anything.
// [orig: Script_Compile @0x4F365D..0x4F3693 (`mov ecx, [eax]` @0x4F368A,
// `or [eax], ecx` @0x4F3693)]
void test_gloop_operand_ors_the_load_time_dword() {
	TempResourceRoot root;
	root.write("sample.wac",
			"gloop accuracyspread inc(v1) end\n"
			"gloop v5 inc(v2) end\n"
			"gloop g_redai inc(v3) end\n");
	w::World world;
	world.script.wac_values.accuracy_spread = 3;
	world.script.vars.set_mission(5, 2);
	wc::WacSystem wac;
	CHECK(wc::wac_layered_load(wac, root.files(), "sample", &world) ==
			wc::WacLayeredLoadStatus::kLoaded);
	std::vector<uint32_t> groups;
	for (const uint32_t word : wac.program().code)
		if (wc::instr_op(word) == wc::Op::GroupIter) groups.push_back(word);
	CHECK(groups.size() == 3);
	if (groups.size() == 3) {
		CHECK(groups[0] == 0x0A000003u); // the named value's dword
		CHECK(groups[1] == 0x0A000002u); // V5's dword
		CHECK(groups[2] == 0x0A000006u); // the pooled group index
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
    CHECK(wc::wac_layered_load(wac, root.files(), "sample", &world) == wc::WacLayeredLoadStatus::kLoaded);
    world.add_system(&wac);
    world.load_systems();
    CHECK(wac.execute_initial(world));
    CHECK(world.script.vars.get_mission(1) == 11);
    CHECK(world.script.vars.get_mission(2) == 1);
    CHECK(wac.program().source_names.size() == 5);

    // A third RUN level and a RUN inside a block are first errors only: the
    // file is skipped and the compile goes on; the third level fails its
    // depth gate before any file read. [orig: Script_Compile @0x4F35A5..0x4F35E1]
    const auto first_error = [&](const char *leaf) {
        root.write("leaf.wac", leaf);
        const bool loaded = wc::wac_layered_load(wac, root.files(), "sample", &world) == wc::WacLayeredLoadStatus::kLoaded;
        const auto &diagnostics = wac.program().diagnostics;
        return loaded && !diagnostics.empty() ? diagnostics[0].message : std::string();
    };
    CHECK(first_error("run game\n") == "A run file can't run more files");
    CHECK(first_error("if never then run extra endif\n") == "Can't run files inside blocks");
    CHECK(first_error("run missing\n") == "A run file can't run more files");
}

// A TextToken resolves at the compile: the mission's text table, then
// gametext.bin; with no mission table every key is the shared "".
// [orig: MissionText_GetStringByKeyOrGameText @0x51ECD0 (no table @0x51ECE7,
// the game-text fallback @0x51ED08, the "" @0x51ED2A)]
void test_text_tokens_read_the_mounted_text_tables() {
	const char *script = "ssnname(5, TT_GREETING) ssnname(6, TT_FALLBACK) ssnname(7, TT_NOSUCH)\n";
	const auto texts = [](const wc::Program &program) {
		std::vector<std::string> out;
		for (const wc::TextToken &token : program.text_tokens) out.push_back(token.key + "=" + token.text);
		return out;
	};
	TempResourceRoot root;
	root.write("sample.wac", script);
	root.write_text("sample.bin", {{"GREETING", "Welcome"}});
	root.write_text("gametext.bin", {{"FALLBACK", "From game"}, {"GREETING", "Shadowed"}});
	wc::WacSystem wac;
	CHECK(wc::wac_layered_load(wac, root.files(), "sample", nullptr) ==
			wc::WacLayeredLoadStatus::kLoaded);
	CHECK((texts(wac.program()) ==
			std::vector<std::string>{"GREETING=Welcome", "FALLBACK=From game", "="}));

	TempResourceRoot bare;
	bare.write("sample.wac", script);
	bare.write_text("gametext.bin", {{"FALLBACK", "From game"}});
	CHECK(wc::wac_layered_load(wac, bare.files(), "sample", nullptr) ==
			wc::WacLayeredLoadStatus::kLoaded);
	CHECK((texts(wac.program()) == std::vector<std::string>{"="}));
}

} // namespace

// `gloop(G_x)` is the retail Unknown Group leg, not a structural error: the
// `(` is the group token, the compile keeps running with group 0, and the
// script loads. [orig: Script_Compile tokenizer
//  @0x4f32e0..0x4f3464; the GLOOP operand resolve @0x4f365d..0x4f3693 ->
//  WacScript_ResolveParameter group leg @0x4f30fc]
void test_parenthesised_gloop_is_the_unknown_group_leg() {
	TempResourceRoot root;
	root.write("sample.wac", "gloop(G_ai) inc(v1) end\ninc(v2)\n");
	w::World world;
	world.registry.configure_pool(0, 4);
	wc::WacSystem wac;
	CHECK(wc::wac_layered_load(wac, root.files(), "sample", &world) ==
			wc::WacLayeredLoadStatus::kLoaded);
	world.add_system(&wac);
	world.load_systems();
	CHECK(wac.execute_initial(world));
	CHECK(world.script.vars.get_mission(1) == 0); // group 0 is empty: the body never runs
	CHECK(world.script.vars.get_mission(2) == 1); // the script kept running past the error
	bool unknown_group = false;
	for (const wc::Diagnostic &d : wac.program().diagnostics)
		if (!d.error && d.message.find("Unknown Group") != std::string::npos) unknown_group = true;
	CHECK(unknown_group);
}

// The def texts the intern-order tests share (CR LF, as every shipped def).
const char *const kAmmoDef =
		"ammo AMMO_A\r\n"
		" velocity 100\r\n"
		" ai_launcheffect Effect_MuzzleA\r\n"
		" effects_table\r\n"
		"  dirt Effect_DirtHit SND_X 3\r\n"
		"  dirt Effect_Redefined SND_X 3\r\n" // the tag is defined: skipped
		"  bogus Effect_Bogus SND 1\r\n"      // unknown tag
		"  grass none none 1\r\n"             // "none" pools nothing
		"  metal Effect_AirExp none 2\r\n"    // already pooled
		"  sand Effect_Short\r\n"             // under four columns
		" end\r\n"
		" secondary_effect Effect_Blast\r\n"
		"end\r\n"
		"ammo AMMO_B\r\n"
		" effects_table\r\n"
		"  dirt Effect_DirtHit2 none 1\r\n"   // a new def: the tags reset
		" end\r\n"
		"end\r\n"
		"ai_launcheffect Effect_Outside\r\n"  // no def open
		"ammo AMMO_C\r\n"
		"ammo AMMO_D\r\n"                     // missing end: the walk stops
		" secondary_effect Effect_AfterStop\r\n"
		"end\r\n";
const char *const kWeaponDef =
		"weapon WPN_A\r\n"
		" particle Effect_NotInAction\r\n"     // a weapon key, not an action's
		" action fire\r\n"
		"  particle Effect_WpnFire\r\n"
		" end\r\n"
		" action bogus\r\n"                    // invalid: no action opens
		"  particle Effect_WpnBogus\r\n"
		" end\r\n"                             // so this end closes the weapon
		" action fire\r\n"
		"  particle Effect_AfterClose\r\n"
		"end\r\n";
const char *const kPowerupDef =
		"powerup PWR_A\r\n"
		" particle Effect_PwrProp\r\n"
		" action pickup\r\n"
		"  particle Effect_PwrPickup\r\n"
		" end\r\n"
		"end\r\n";

// D-WAC-5 (the Fx half): the mission start pools the 78-name material table,
// ammo.def, weapon.def, the vehicle fire set and powerup.def before the WAC
// compile, so a script's first new FX literal compiles to the slot after them.
// [orig: Game_StartMission @0x52499e / @0x525495 / @0x5254bd / @0x52563d /
//  @0x5256d2 / @0x525cb3; CEffectWorld_InternEffectHandle @0x5F7310]
void test_mission_start_intern_order() {
	opennova::particle::ParticleFile file;
	file.effects.push_back({"stockeffect", {}});
	file.effects.push_back({"EFFECT_AIREXP", {}});
	opennova::particle::EffectCatalogNames effects;
	effects.add_document(file);
	wc::intern_mission_start_effects(effects, {kAmmoDef, kWeaponDef, kPowerupDef});
	const std::vector<std::string> names = effects.interned_names();
	CHECK(names.size() == 78u + 10u);
	if (names.size() != 78u + 10u) return;
	CHECK(names[0] == "EFFECT_AIREXP" && names[1] == "Effect_BigSplash" &&
			names[77] == "Effect_surfaceRings");
	const std::vector<std::string> tail(names.begin() + 78, names.end());
	const std::vector<std::string> expected = {"Effect_MuzzleA", "Effect_DirtHit", "Effect_Blast",
			"Effect_DirtHit2", "Effect_WpnFire", "Effect_smkSigB", "Effect_vehicleFireLarge",
			"Effect_vehicleFireMed", "Effect_vehicleFireSmall", "Effect_PwrPickup"};
	CHECK(tail == expected);
	CHECK(effects.intern("Effect_ScriptFx").value == 89u);
	CHECK(effects.intern("effect_blast").value == 81u);
	// Without stockeffect only a defined name pools: the table's first entry.
	opennova::particle::ParticleFile plain;
	plain.effects.push_back({"Effect_AirExp", {}});
	plain.effects.push_back({"Effect_PwrPickup", {}});
	opennova::particle::EffectCatalogNames bare;
	bare.add_document(plain);
	wc::intern_mission_start_effects(bare, {kAmmoDef, kWeaponDef, kPowerupDef});
	CHECK((bare.interned_names() == std::vector<std::string>{"Effect_AirExp", "Effect_PwrPickup"}));
}

// The catalog the kernel builds carries that order: the mounted documents,
// then the mission-start names, then the compile's.
void test_effect_catalog_load_seeds_the_mission_start_names() {
	TempResourceRoot root;
	opennova::particle::ParticleFile file;
	opennova::particle::ParticleDef particle;
	particle.id = "loop";
	file.particles.push_back(particle);
	file.effects.push_back({"stockeffect", {"loop"}});
	std::ostringstream text;
	std::string error;
	CHECK(opennova::particle::save_particles(text, file, error));
	root.write("script.ptl", text.str());
	root.write("ammo.def", kAmmoDef);
	root.write("weapon.def", kWeaponDef);
	root.write("powerup.def", kPowerupDef);
	mission::BootFileSource files = root.files();
	files.list_files = [](const std::string &extension) {
		return extension == ".ptl" ? std::vector<std::string>{"script.ptl"}
				: std::vector<std::string>{};
	};
	opennova::particle::EffectCatalogNames effects;
	wc::load_script_effect_catalog(files, effects);
	const std::vector<std::string> names = effects.interned_names();
	CHECK(names.size() == 88u);
	if (names.size() == 88u) CHECK(names[78] == "Effect_MuzzleA" && names[87] == "Effect_PwrPickup");
	CHECK(effects.intern("Effect_ScriptFx").value == 89u);
}

int main() {
	test_mission_start_intern_order();
	test_effect_catalog_load_seeds_the_mission_start_names();
    test_run_files_share_order_symbols_and_diagnostics();
	test_parenthesised_gloop_is_the_unknown_group_leg();
	test_wac_layers_execute_in_retail_order();
	test_absent_layers_are_the_valid_bms_only_mission();
	test_retail_two_word_else_if_chain_nests();
	test_retail_first_errors_always_load();
	test_gloop_operand_ors_the_load_time_dword();
	test_text_tokens_read_the_mounted_text_tables();
	std::printf(failures ? "WAC LAYERED LOAD TEST FAILED (%d)\n"
	                     : "wac layered load test passed\n",
	            failures);
	return failures ? 1 : 0;
}
