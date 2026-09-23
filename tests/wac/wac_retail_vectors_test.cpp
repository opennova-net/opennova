// The original compiler and VM on synthetic programs. Each listing in
// fixtures/wac_retail_compile_vectors.inc is what Script_Compile @0x4F31F0
// (with WacScript_ResolveParameter @0x4F2920) wrote for the source beside it,
// and each row of fixtures/wac_retail_vm_vectors.inc is four executions of
// WacScript_ExecuteBytecode @0x4F58B0 over the original compiler's program
// (scripts/oracles/wac_parity.py). The port compiles every source against the
// generator's synthetic catalogs and must write the same listing and reach
// the same state.
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <formats/lwf/lwf.h>
#include <formats/particle/particle.h>
#include <formats/wac/bytecode.h>
#include <runtime/audio/oneshot_play.h>
#include <runtime/particle/effect_catalog_names.h>
#include <runtime/wac/compiler.h>
#include <runtime/wac/vm.h>
#include <runtime/world/ammo_table.h>
#include <runtime/world/world.h>

#include "common/sha256.h"
#include "wac_listing.h"

using namespace opennova;
using namespace opennova::wac;

namespace {

int failures = 0;

struct CompileVector {
	const char *source;
	const char *files; // NAME \x01 SOURCE \x01 ... for RUN
	const char *listing;
};
const CompileVector kCompileVectors[] = {
#include "fixtures/wac_retail_compile_vectors.inc"
};

constexpr int kVmSteps = 4;
// result, V0..V7, G0..G3, DO counters 0..3, DO choices 0..3, the generator,
// then the fired tick, fired dword and condition byte of events 0..3.
constexpr int kVmValuesPerStep = 34;
struct VmVector {
	const char *source;
	int32_t values[kVmSteps * kVmValuesPerStep];
};
const VmVector kVmVectors[] = {
#include "fixtures/wac_retail_vm_vectors.inc"
};

std::string upper(std::string text) {
	for (char &c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
	return text;
}

// The generator's catalogs: effects SMOKE and FIRE, sound sets BOOM and
// ALARM, ammo RIFLE and AMMO_PISTOL after the null row, and the text keys
// HELLO and BYE; every other lookup misses.
struct Catalogs {
	particle::EffectCatalogNames effects;
	audio::SoundSetIndex sounds;
	world::AmmoTable ammo;
	Catalogs() {
		particle::ParticleFile file;
		for (const char *name : {"smoke", "fire"}) {
			particle::EffectDef effect;
			effect.id = name;
			file.effects.push_back(effect);
		}
		effects.add_document(file);
		lwf::File bank;
		for (const char *name : {"boom", "alarm"}) {
			lwf::Multi set;
			set.name = name;
			bank.multis.push_back(set);
		}
		sounds.add_bank(0, bank);
		for (const char *name : {"null", "rifle", "ammo_pistol"}) {
			world::AmmoTableEntry entry;
			entry.name = name;
			entry.valid = true;
			ammo.entries.push_back(entry);
		}
	}
};

CompileEnv environment(Catalogs &catalogs, const char *files) {
	CompileEnv env;
	env.effects = &catalogs.effects;
	env.sounds = &catalogs.sounds;
	env.ammo = &catalogs.ammo;
	env.source_names = {"script.wac"};
	env.text_token = [](const std::string &key) -> std::optional<std::string> {
		if (upper(key) == "HELLO") return std::string("Hello there");
		if (upper(key) == "BYE") return std::string("Goodbye");
		return std::nullopt;
	};
	std::vector<std::pair<std::string, std::string>> run_files;
	for (const char *s = files; *s != '\0';) {
		const char *name_end = std::strchr(s, '\x01');
		const char *text_end = std::strchr(name_end + 1, '\x01');
		run_files.emplace_back(upper(std::string(s, name_end)), std::string(name_end + 1, text_end));
		s = text_end + 1;
	}
	env.load_source = [run_files](const std::string &name, std::string &source) {
		for (const auto &[file, text] : run_files) {
			if (file != upper(name)) continue;
			source = text;
			return true;
		}
		return false;
	};
	return env;
}

// The first differing line, for the failure report.
void report(int index, const std::string &expected, const std::string &actual) {
	size_t e = 0, a = 0;
	int line = 1;
	while (e < expected.size() && a < actual.size()) {
		const size_t e_end = std::min(expected.find('\n', e), expected.size());
		const size_t a_end = std::min(actual.find('\n', a), actual.size());
		if (expected.compare(e, e_end - e, actual, a, a_end - a) != 0) {
			std::printf("FAIL compile vector %d line %d: expected '%s' got '%s'\n", index, line,
					expected.substr(e, e_end - e).c_str(), actual.substr(a, a_end - a).c_str());
			return;
		}
		e = e_end + 1;
		a = a_end + 1;
		++line;
	}
	std::printf("FAIL compile vector %d: listings differ in length at line %d\n", index, line);
}

void test_compile_vectors() {
	int index = 0;
	for (const CompileVector &vector : kCompileVectors) {
		// \x04 separates the game.wac / server.wac / mission layers of one load.
		Catalogs catalogs;
		CompileEnv env = environment(catalogs, vector.files);
		const std::string source = wac_listing::expand(vector.source);
		std::vector<std::string> layers;
		for (size_t start = 0;;) {
			const size_t stop = source.find('\x04', start);
			layers.push_back(source.substr(start, stop == std::string::npos ? std::string::npos : stop - start));
			if (stop == std::string::npos) break;
			start = stop + 1;
		}
		if (layers.size() > 1) env.source_names = {"game.wac", "server.wac", "mission.wac"};
		const Program program = layers.size() > 1 ? compile_program(layers, env) : compile_source(source, env);
		const std::string listing = wac_listing::document(program);
		const std::string expected = vector.listing;
		const bool same = expected.rfind("sha256:", 0) == 0
				? expected.substr(7) == testhash::sha256_hex(
						reinterpret_cast<const uint8_t *>(listing.data()), listing.size())
				: expected == listing;
		if (!same) {
			++failures;
			if (expected.rfind("sha256:", 0) == 0) std::printf("FAIL compile vector %d: listing hash differs\n", index);
			else report(index, expected, listing);
		}
		++index;
	}
	std::printf("%d original-compiler listings compared\n", index);
}

void test_vm_vectors() {
	int index = 0;
	for (const VmVector &vector : kVmVectors) {
		Catalogs catalogs;
		const Program program = compile_source(vector.source, environment(catalogs, ""));
		auto world = std::make_unique<world::World>();
		world->registry.configure_pool(0, 64);
		WacVm vm;
		vm.load(program);
		for (int step = 0; step < kVmSteps; ++step) {
			vm.execute(*world);
			const WacVm::RuntimeState state = vm.capture_runtime_state();
			std::vector<int32_t> got;
			got.push_back(vm.accumulator());
			for (int i = 0; i < 8; ++i) got.push_back(world->script.vars.get_mission(i));
			for (int i = 0; i < 4; ++i) got.push_back(world->script.vars.get_global(i));
			for (int i = 0; i < 4; ++i)
				got.push_back(size_t(i) < state.loop_counters.size() ? state.loop_counters[size_t(i)] : 0);
			for (int i = 0; i < 4; ++i)
				got.push_back(size_t(i) < state.loop_choices.size() ? state.loop_choices[size_t(i)] : 0);
			got.push_back(static_cast<int32_t>(state.rng_seed));
			const auto event = [&](int i) -> const EventState * {
				return i < program.event_count && size_t(i) < state.events.size() ? &state.events[size_t(i)] : nullptr;
			};
			for (int i = 0; i < 4; ++i) got.push_back(event(i) ? int32_t(event(i)->last_fired_tick) : 0);
			for (int i = 0; i < 4; ++i) got.push_back(event(i) && event(i)->ever_fired ? 1 : 0);
			for (int i = 0; i < 4; ++i) got.push_back(event(i) && event(i)->active ? 1 : 0);
			const int32_t *expected = vector.values + step * kVmValuesPerStep;
			for (int k = 0; k < kVmValuesPerStep; ++k) {
				if (got[size_t(k)] == expected[k]) continue;
				std::printf("FAIL vm vector %d step %d value %d: expected %d got %d\n", index, step, k,
						expected[k], got[size_t(k)]);
				++failures;
				break;
			}
			vm.advance_time();
		}
		++index;
	}
	std::printf("%d original-VM programs compared\n", index);
}

} // namespace

int main() {
	test_compile_vectors();
	test_vm_vectors();
	if (failures != 0) {
		std::printf("WAC RETAIL VECTORS FAILED (%d)\n", failures);
		return 1;
	}
	std::printf("wac retail vectors passed\n");
	return 0;
}
