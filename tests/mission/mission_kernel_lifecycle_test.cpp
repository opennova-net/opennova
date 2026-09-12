// The mission kernel's lifecycle contract (ADR 0042 d3), ungated, over the
// synthetic mission mission_kernel_test boots: the ordering guards (boot
// before open, an unmountable root), the play-start baseline the embedders
// seal after the spawn (capture_baseline / restore_baseline rewind the
// registry, the local player's position and health, the logic clock and the
// event latches), the strict-vs-lenient WAC diagnostic policy (a program
// that compiles with a warning loads under the game's policy and refuses the
// boot under the dedicated host's), and the no-terrain path (no field, no
// grounding, the teleport seams still work).
#include <runtime/inmatch/local_role.h>
#include <runtime/mission/mission_kernel.h>

#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace opennova;
namespace ms = opennova::mission;
namespace w = opennova::world;

static int failures = 0;
#define CHECK(c)                                                                            \
	do {                                                                                    \
		if (!(c)) {                                                                         \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                        \
			++failures;                                                                     \
		}                                                                                   \
	} while (0)

namespace {

bms::Entity organic(int32_t x, int32_t y, int32_t z, uint8_t team) {
	bms::Entity e{};
	e.type = bms::ItemType::Organic;
	e.x = x;
	e.y = y;
	e.z = z;
	e.yaw = 90;
	e.team = team;
	return e;
}

bms::Entity item(int32_t type_id, int32_t x, int32_t y, int32_t z) {
	bms::Entity e{};
	e.type = bms::ItemType::Item;
	e.type_id = type_id;
	e.x = x;
	e.y = y;
	e.z = z;
	return e;
}

ms::BootFileSource source_over(const std::map<std::string, std::string> *files) {
	ms::BootFileSource s;
	s.has_file = [files](const std::string &name) {
		return files->find(name) != files->end();
	};
	s.read_file = [files](const std::string &name, std::vector<uint8_t> &out) {
		const auto it = files->find(name);
		if (it == files->end()) return false;
		out.assign(it->second.begin(), it->second.end());
		return true;
	};
	return s;
}

bms::File synthetic_mission() {
	bms::File m{};
	m.items.push_back(item(/*type_id=*/164, 10 << 16, 20 << 16, 3 << 16));
	m.items[0].id = 21;
	m.organics.push_back(organic(1 << 16, 1 << 16, 0, /*team=*/1));
	m.organics[0].id = 31;
	m.events.push_back(bms::Event{});
	return m;
}

bool near_equal(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }

} // namespace

// The bare no-net tick: the local role over the kernel (ADR 0043 d3; the
// kernel itself owns no tick).
static void tick_no_net(opennova::mission::MissionKernel &kernel) {
	opennova::inmatch::LocalRole role;
	role.bind(kernel);
	role.run_tick(opennova::inmatch::TickInput{});
}

// Every kernel below lives on the heap: sizeof(MissionKernel) is ~270 KB (the
// World inside it alone ~230 KB, with the 512-dword script var bank and the
// per-entity/brain records), the carry block holds three of them live at once,
// and MSVC does not reliably overlap main()'s block-scoped locals, so stack
// kernels overflow the 1 MB default stack (STATUS_STACK_OVERFLOW in main).
int main() {
	// --- the ordering guards ---------------------------------------------------
	{
		auto kernel_box = std::make_unique<ms::MissionKernel>();
		ms::MissionKernel &kernel = *kernel_box;
		ms::KernelBootOptions options;
		std::string error;
		CHECK(!kernel.boot(options, error));
		CHECK(error == "open() / open_document() first");
		CHECK(!kernel.local.has_local_player());
		CHECK(!kernel.have_baseline);
		CHECK(!kernel.restore_baseline());
	}
	{
		auto kernel_box = std::make_unique<ms::MissionKernel>();
		ms::MissionKernel &kernel = *kernel_box;
		std::string error;
		CHECK(!kernel.open("./definitely/not/a/mounted/root", "nowhere.bms", error));
		CHECK(!error.empty());
	}

	// --- the sealed play-start baseline ----------------------------------------
	{
		std::map<std::string, std::string> files;
		files["synth.wac"] = "if never() then set(v1,1) endif\n";
		auto kernel_box = std::make_unique<ms::MissionKernel>();
		ms::MissionKernel &kernel = *kernel_box;
		kernel.open_document(synthetic_mission(), "synth", source_over(&files));
		ms::KernelBootOptions options;
		std::string error;
		CHECK(kernel.boot(options, error));
		CHECK(kernel.local.has_local_player());
		CHECK(kernel.have_baseline);
		CHECK(kernel.have_wac_baseline);
		// The boot's own baseline is the post-PreMission point; the embedders
		// re-seal it once the spawn and the eager WAC have settled (the
		// kernel's complete_mission_start). Seal here, then mutate.
		tick_no_net(kernel);
		kernel.capture_baseline();
		const w::Vec3 spawn_pos = kernel.local.player_position();
		const int32_t spawn_health = kernel.local.player_health();
		const uint32_t sealed_tick = kernel.world.logic_tick;
		const w::EntityHandle player_h = kernel.local.player()->handle;
		CHECK(spawn_health > 0);

		tick_no_net(kernel);
		tick_no_net(kernel);
		kernel.local.teleport_local_player(w::Vec3{100.0f, 200.0f, 5.0f}, /*yaw_deg=*/90.0, /*pitch_deg=*/0.0);
		kernel.world.commands.set_entity_health(player_h, 37);
		kernel.world.script.vars.set_mission(3, 99);
		CHECK(kernel.world.logic_tick == sealed_tick + 2);
		CHECK(kernel.local.player_health() == 37);
		CHECK(near_equal(kernel.local.player_position().x, 100.0f, 0.001f));

		CHECK(kernel.restore_baseline());
		CHECK(kernel.local.has_local_player());
		CHECK(kernel.local.player() != nullptr);
		CHECK(kernel.local.player_ai() != nullptr);
		CHECK(kernel.world.logic_tick == sealed_tick);
		CHECK(kernel.local.player_health() == spawn_health);
		CHECK(near_equal(kernel.local.player_position().x, spawn_pos.x, 0.001f));
		CHECK(near_equal(kernel.local.player_position().y, spawn_pos.y, 0.001f));
		CHECK(near_equal(kernel.local.player_position().z, spawn_pos.z, 0.001f));
		if (const w::AiEntity *body = kernel.local.player_ai()) {
			CHECK(near_equal(body->pos[0] / 65536.0f, spawn_pos.x, 0.01f));
			CHECK(near_equal(body->pos[1] / 65536.0f, spawn_pos.y, 0.01f));
		}
		CHECK(kernel.world.script.vars.get_mission(3) == 0);
		CHECK(kernel.world.registry.by_net_id(21) != nullptr);
		CHECK(kernel.world.registry.by_net_id(31) != nullptr);
		// The restored world ticks on from the sealed point.
		tick_no_net(kernel);
		CHECK(kernel.world.logic_tick == sealed_tick + 1);
		// A second restore rewinds again (the SP round restart).
		CHECK(kernel.restore_baseline());
		CHECK(kernel.world.logic_tick == sealed_tick);
		CHECK(kernel.local.has_local_player());
	}

	// --- the WAC diagnostic policy ---------------------------------------------
	// An unknown command is a compiler WARNING (older games extend the
	// keyword set): the program compiles, so the game's lenient policy loads
	// it and the dedicated host's strict policy refuses the boot.
	{
		std::map<std::string, std::string> files;
		files["synth.wac"] = "if never() then bogus_command(1) endif\n";
		auto kernel_box = std::make_unique<ms::MissionKernel>();
		ms::MissionKernel &kernel = *kernel_box;
		kernel.open_document(synthetic_mission(), "synth", source_over(&files));
		ms::KernelBootOptions options; // lenient
		std::string error;
		CHECK(kernel.boot(options, error));
		CHECK(error.empty());
		CHECK(kernel.wac_loaded);
		CHECK(kernel.wac.vm().loaded());
	}
	{
		std::map<std::string, std::string> files;
		files["synth.wac"] = "if never() then bogus_command(1) endif\n";
		auto kernel_box = std::make_unique<ms::MissionKernel>();
		ms::MissionKernel &kernel = *kernel_box;
		kernel.open_document(synthetic_mission(), "synth", source_over(&files));
		ms::KernelBootOptions options;
		options.wac_strict_diagnostics = true;
		std::string error;
		CHECK(!kernel.boot(options, error));
		CHECK(!kernel.wac_loaded);
		CHECK(error.find("failed to compile cleanly") != std::string::npos);
		CHECK(error.find("unknown command") != std::string::npos);
		CHECK(error.find("bogus_command") != std::string::npos);
	}
	// No script at all is the valid BMS-only mission under both policies.
	{
		std::map<std::string, std::string> files;
		auto kernel_box = std::make_unique<ms::MissionKernel>();
		ms::MissionKernel &kernel = *kernel_box;
		kernel.open_document(synthetic_mission(), "synth", source_over(&files));
		ms::KernelBootOptions options;
		options.wac_strict_diagnostics = true;
		std::string error;
		CHECK(kernel.boot(options, error));
		CHECK(!kernel.wac_loaded);
	}

	// --- the declared-variable carry across kernels -------------------------------
	// Retail's compiler-declared VAR/ARRAY slots (0xC6B640 + 4n, n = declaration
	// order) are never zeroed by any load path: WacScript_InitAndLoad clears
	// V0..V255 only (memset 0x400 @0x4f95ee) and Script_Compile's declaration
	// arm stores name/address/type without writing the slot (@0x4f3812..
	// 0x4f3964). A rebuilt kernel carries that half (the embedder's
	// reset_world seam: ScriptVarStore::carry_declared_from) so a restart or
	// the next mission reads slot n at the previous run's value while V#
	// restart at zero.
	{
		std::map<std::string, std::string> files;
		files["synth.wac"] = "var a\ninc(a)\nv1 = a\n";
		auto first_box = std::make_unique<ms::MissionKernel>();
		ms::MissionKernel &first = *first_box;
		first.open_document(synthetic_mission(), "synth", source_over(&files));
		ms::KernelBootOptions options;
		std::string error;
		CHECK(first.boot(options, error));
		CHECK(first.wac_loaded);
		CHECK(first.world.script.vars.get_mission(1) == 1);   // the eager execution ran once
		CHECK(first.world.script.vars.get_mission(256) == 1); // `a` is slot 0 of the declared half

		auto second_box = std::make_unique<ms::MissionKernel>();
		ms::MissionKernel &second = *second_box;
		second.world.script.vars.carry_declared_from(first.world.script.vars);
		CHECK(second.world.script.vars.get_mission(256) == 1);
		CHECK(second.world.script.vars.get_mission(1) == 0); // V# start at zero
		second.open_document(synthetic_mission(), "synth", source_over(&files));
		CHECK(second.boot(options, error));
		CHECK(second.world.script.vars.get_mission(1) == 2);   // inc over the carried slot
		CHECK(second.world.script.vars.get_mission(256) == 2);

		// A different program whose first declaration is another name reads
		// the SAME slot: the carry is by index, not by name.
		std::map<std::string, std::string> other;
		other["synth.wac"] = "var b\nv2 = b\n";
		auto third_box = std::make_unique<ms::MissionKernel>();
		ms::MissionKernel &third = *third_box;
		third.world.script.vars.carry_declared_from(second.world.script.vars);
		third.open_document(synthetic_mission(), "synth", source_over(&other));
		CHECK(third.boot(options, error));
		CHECK(third.world.script.vars.get_mission(2) == 2);
	}

	// --- the no-terrain path -----------------------------------------------------
	{
		std::map<std::string, std::string> files;
		auto kernel_box = std::make_unique<ms::MissionKernel>();
		ms::MissionKernel &kernel = *kernel_box;
		kernel.open_document(synthetic_mission(), "synth", source_over(&files));
		ms::KernelBootOptions options;
		options.collision = false;
		std::string error;
		CHECK(kernel.boot(options, error));
		CHECK(!kernel.has_terrain());
		CHECK(!kernel.terrain_store.valid());
		CHECK(kernel.world.tables.terrain == nullptr);
		CHECK(kernel.world.ai.terrain == nullptr);
		CHECK(kernel.world.collision == nullptr);
		CHECK(kernel.collision_attached == 0);
		// The frame legs run without a field: the clock advances and the
		// teleport seam still writes both stores.
		const uint32_t tick0 = kernel.world.logic_tick;
		tick_no_net(kernel);
		CHECK(kernel.world.logic_tick == tick0 + 1);
		kernel.local.teleport_local_player(w::Vec3{7.0f, 8.0f, 9.0f}, 0.0, 0.0);
		CHECK(near_equal(kernel.local.player_position().z, 9.0f, 0.001f));
		if (const w::AiEntity *body = kernel.local.player_ai()) CHECK(body->pos[2] == 9 << 16);
		tick_no_net(kernel);
		CHECK(kernel.world.logic_tick == tick0 + 2);
	}

	if (failures == 0) std::printf("mission_kernel_lifecycle: all checks passed\n");
	return failures == 0 ? 0 : 1;
}
