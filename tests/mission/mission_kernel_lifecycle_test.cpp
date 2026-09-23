// The mission kernel's lifecycle contract (ADR 0042 d3), ungated, over the
// synthetic mission mission_kernel_test boots: the ordering guards (boot
// before open, an unmountable root), the play-start baseline the embedders
// seal after the spawn (capture_baseline / restore_baseline rewind the
// registry, the local player's position and health, the logic clock and the
// event latches), the strict-vs-lenient WAC diagnostic policy (retail's own
// first errors load under both, a literal the mounted catalogs miss refuses
// the dedicated host's boot), and the no-terrain path (no field, no
// grounding, the teleport seams still work).
#include <runtime/inmatch/local_role.h>
#include <runtime/mission/mission_kernel.h>
#include <formats/def/def.h>

#include "common/boot_file_source.h"

#include <array>
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

using test_boot::source_over;

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
	// --- the spawn-marker list is built before the PreMission pass ------------
	// A PreMission action that hands the lowest zone to team 2 does not reorder
	// the markers: the list was built with that zone on team 1, where a
	// marker's priority is its own zone number.
	// [orig: Game_StartMission — the build_spawn_marker_budget_list call
	//  @0x5252C6 precedes the EventTrigger_UpdateAllWithFlag2 call @0x525B86;
	//  build_spawn_marker_budget_list @0x529B40]
	{
		std::array<def::DefItemDef, 2> rows{};
		rows[0].id = ms::kItemIdOffset + 900;
		rows[0].type = 5;
		rows[0].attrib = 0x60000u;
		rows[0].hp = 100;
		rows[1].id = ms::kItemIdOffset + 901;
		rows[1].type = 4;
		rows[1].attrib2 = 4u;
		rows[1].hp = 100;
		def::DefItemsFile items{rows.data(), rows.size()};
		bms::File m{};
		bms::Entity low = item(/*type_id=*/900, 10 << 16, 0, 0);
		low.id = 50;
		low.team = 1;
		low.lfp_group = 1;
		bms::Entity high = item(/*type_id=*/900, 20 << 16, 0, 0);
		high.id = 51;
		high.team = 2;
		high.lfp_group = 2;
		m.items = {low, high};
		bms::Entity marker{};
		marker.type = bms::ItemType::Marker;
		marker.type_id = 901;
		marker.id = 52;
		marker.lfp_group = 2;
		marker.x = 20 << 16;
		m.markers = {marker};
		bms::Event event{};
		event.flags = bms::EventFlags::PreMission;
		event.action_index = 0;
		event.action_count = 1;
		bms::Action action{};
		action.action_type = bms::ActionType::ChangeSteamAction;
		action.param1 = 50;
		action.param2 = 2;
		m.events = {event};
		m.actions = {action};
		std::map<std::string, std::string> files;
		auto kernel = std::make_unique<ms::MissionKernel>();
		kernel->open_document(std::move(m), "synth", source_over(&files));
		kernel->set_items_table(&items);
		ms::KernelBootOptions options;
		options.playable = false;
		std::string error;
		CHECK(kernel->boot(options, error));
		const w::Entity *zone = kernel->world.registry.get(w::EntityHandle::make(1, 0));
		const w::Entity *placed = kernel->world.registry.get(w::EntityHandle::make(3, 0));
		CHECK(zone != nullptr && zone->team == 2); // the PreMission action ran
		CHECK(placed != nullptr && placed->vehicle_spawn_priority == 2);
	}

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
	// An unknown command is retail's first error "Unknown '...'" and the
	// program runs anyway, so the game's policy loads it.
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
	// Strict mode loads retail's own first errors and refuses only a literal
	// the mounted catalogs miss (no .ptl here, so every FX name misses).
	{
		std::map<std::string, std::string> files;
		files["synth.wac"] = "if never() then bogus_command(1) endif\n";
		auto kernel_box = std::make_unique<ms::MissionKernel>();
		ms::MissionKernel &kernel = *kernel_box;
		kernel.open_document(synthetic_mission(), "synth", source_over(&files));
		ms::KernelBootOptions options;
		options.wac_strict_diagnostics = true;
		std::string error;
		CHECK(kernel.boot(options, error));
		CHECK(kernel.wac_loaded);
		CHECK(!kernel.wac.program().diagnostics.empty() &&
				kernel.wac.program().diagnostics[0].message == "Unknown 'BOGUS_COMMAND'");
	}
	{
		std::map<std::string, std::string> files;
		files["synth.wac"] = "if never() then fx2tgt(nosuch_effect, 1) endif\n";
		auto kernel_box = std::make_unique<ms::MissionKernel>();
		ms::MissionKernel &kernel = *kernel_box;
		kernel.open_document(synthetic_mission(), "synth", source_over(&files));
		ms::KernelBootOptions options;
		options.wac_strict_diagnostics = true;
		std::string error;
		CHECK(!kernel.boot(options, error));
		CHECK(!kernel.wac_loaded);
		CHECK(error.find("Unknown FX") != std::string::npos);
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

	// --- the no-session objective relay ------------------------------------------
	// The bare tick has no connection for the S2C 0x3F relay, so the queue is
	// released while the local chat effect stays. [orig:
	//  Server_BroadcastEntityActionPacket @0x5080D0 — the
	//  NapiNPServer_SendFiltered call @0x508199]
	{
		std::map<std::string, std::string> files;
		files["synth.wac"] = "if never() then set(v1,1) endif\n";
		auto kernel_box = std::make_unique<ms::MissionKernel>();
		ms::MissionKernel &kernel = *kernel_box;
		kernel.open_document(synthetic_mission(), "synth", source_over(&files));
		ms::KernelBootOptions options;
		std::string error;
		CHECK(kernel.boot(options, error));
		kernel.world.show_objective_notification(2, 1, 1, 1);
		CHECK(kernel.world.out.hud_relays.size() == 1);
		CHECK(kernel.world.out.effects.count("objective") == 1);
		tick_no_net(kernel);
		CHECK(kernel.world.out.hud_relays.empty());
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

	// --- the teardown's PostMission sweep ------------------------------------------
	// The authority's mission exit destroys pools 0, 1 and 2 (the pool-3
	// markers stay), then sweeps the PostMission entries exactly once; nothing
	// sweeps them while the mission runs. So a SingleAlive trigger naming the
	// pool-1 item reads it gone, and one naming the resident pool-3 marker
	// fails too, because that row walk never covers pool 3.
	// [orig: Game_TeardownMission — Entity_Destroy over pools 0..2
	//  @0x522365..0x5223C8, EventTrigger_UpdateAllWithFlag4 @0x52266C]
	{
		bms::File m = synthetic_mission();
		bms::Entity marker{};
		marker.type = bms::ItemType::Marker;
		marker.type_id = 44;
		marker.id = 41;
		m.markers.push_back(marker);
		const auto post_event = [](int trigger_index, int trigger_count, int action_index) {
			bms::Event e{};
			e.flags = bms::EventFlags::PostMission;
			e.trigger_index = trigger_index;
			e.trigger_count = static_cast<uint8_t>(trigger_count);
			e.action_index = action_index;
			e.action_count = 1;
			return e;
		};
		const auto alive = [](int ssn) {
			bms::Trigger t{};
			t.main_type = bms::TriggerMainType::Single;
			t.sub_type = static_cast<int32_t>(bms::SingleTriggerType::SingleAlive);
			t.param1 = ssn;
			return t;
		};
		const auto increment = [](int var) {
			bms::Action a{};
			a.action_type = bms::ActionType::MisvarChange;
			a.action_sub_type = static_cast<int32_t>(bms::MissionVariableActionSubType::Increment);
			a.param1 = var;
			return a;
		};
		m.events = {post_event(0, 0, 0), post_event(0, 1, 1), post_event(1, 1, 2)};
		m.triggers = {alive(21), alive(41)};
		m.actions = {increment(9), increment(10), increment(11)};

		std::map<std::string, std::string> files;
		auto kernel_box = std::make_unique<ms::MissionKernel>();
		ms::MissionKernel &kernel = *kernel_box;
		kernel.open_document(m, "synth", source_over(&files));
		ms::KernelBootOptions options;
		std::string error;
		CHECK(kernel.boot(options, error));
		CHECK(kernel.world.registry.by_net_id(21) != nullptr);
		CHECK(kernel.world.registry.by_net_id(41) != nullptr);
		for (int i = 0; i < 70; ++i) tick_no_net(kernel);
		CHECK(kernel.world.script.vars.get_mission(9) == 0);
		CHECK(kernel.world.script.vars.get_mission(10) == 0);
		CHECK(kernel.world.script.vars.get_mission(11) == 0);

		opennova::inmatch::LocalRole role;
		role.bind(kernel);
		role.close();
		CHECK(kernel.world.script.vars.get_mission(9) == 1);  // no trigger: the sweep ran once
		CHECK(kernel.world.script.vars.get_mission(10) == 0); // the pool-1 item was destroyed first
		// The pool-3 marker is still resident (checked below), but SingleAlive
		// never sees pool 3: its row walk covers pools 0, 1 and 2 only.
		// [orig: Entity_IsAliveByBmsRef @0x43E640]
		CHECK(kernel.world.script.vars.get_mission(11) == 0);
		CHECK(kernel.world.registry.by_net_id(21) == nullptr);
		CHECK(kernel.world.registry.by_net_id(31) == nullptr);
		CHECK(kernel.world.registry.by_net_id(41) != nullptr);
	}

	if (failures == 0) std::printf("mission_kernel_lifecycle: all checks passed\n");
	return failures == 0 ? 0 : 1;
}
