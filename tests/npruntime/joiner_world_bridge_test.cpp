// S10a (ADR 0028): the joiner's per-frame world<->net bridge. The pump's phase
// SEQUENCE is the witnessed retail client frame [orig: Game_ProcessMainFrame
// @0x5263f0; Client_ProcessNetworkFrame @0x42c180] — this pins the frame order
// the shell hooks observe, the ClientHello one-shot, the per-frame clock, the
// in-match spawn edge for L, and the L-gate on the weapon pump. World-effect
// depth (health folds, mount sync, mirrors) is covered by the GUT net suites
// and the S10 live A/B; this test locks the portable sequencing contract.

#include <net/npruntime/client_runtime.h>
#include <net/npruntime/joiner_world_bridge.h>

#include <runtime/world/ai.h>
#include <runtime/world/entity.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/player_loadout.h>
#include <runtime/world/player_weapon.h>
#include <runtime/world/weapon_inventory.h>
#include <runtime/world/world.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace opennova;
namespace np = opennova::np;
namespace w = opennova::world;

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return false;
}

const std::string kClientScrk = "CLIENT-BRIDGE-SCRK";
const std::string kServerScrk = "SERVER-BRIDGE-SCRK";
constexpr uint32_t kClientKey = 0x0000BEEFu;
constexpr uint32_t kSessionId = 0x0FE0E112u;

struct Harness {
	w::World world;
	w::AiSystem ai;
	w::LocalPlayerWeapon weapon;
	w::LocalPlayerLoadout loadout;
	w::WeaponInventory inventory;
	std::vector<mission::ItemSeatSpec> seat_specs;
	np::JoinerWorldBridge bridge;

	std::vector<std::string> calls;
	int sends = 0;
	int spawned_hook = 0;
	int32_t spawned_heading = -1;
	// The embedder's live inventory-valid flag. The pump reads it BY REFERENCE
	// so a mid-pump apply_authoritative_loadout() flip is visible to the
	// same-pump spawn stamp (the D-NET-194 wire-header join).
	bool inventory_valid = false;

	Harness() {
		world.ai = &ai;
		world.registry.configure_pool(0, 16);
	}

	np::JoinerWorldBridge::PumpContext ctx(np::ClientRuntime &runtime) {
		return np::JoinerWorldBridge::PumpContext{
				world, runtime, weapon, loadout, inventory,
				inventory_valid, seat_specs, /*root_motion=*/nullptr};
	}

	np::JoinerWorldBridge::PumpHooks hooks() {
		np::JoinerWorldBridge::PumpHooks h;
		h.send = [this](const std::vector<uint8_t> &) { ++sends; };
		h.deposit_inbound = [this] { calls.push_back("deposit"); };
		h.resolve_row_adm_ids = [this] { calls.push_back("adm"); };
		h.on_wire_leg_complete = [this] { calls.push_back("wire_leg"); };
		h.apply_authoritative_loadout = [this] { calls.push_back("loadout"); };
		h.reseed_kit_on_side_change = [this] {
			calls.push_back("reseed");
			return false;
		};
		h.push_loadout_kit = [this] { calls.push_back("push_kit"); };
		h.on_diagnostic_sample = [this] { calls.push_back("diag"); };
		h.on_replica_world_changed =
				[this](const netsim::ClientWorldSyncResult &) {
					calls.push_back("world_changed");
				};
		h.on_replica_world_static_ready =
				[this] { calls.push_back("static_ready"); };
		h.on_local_player_spawned = [this](int32_t heading) {
			calls.push_back("spawned");
			++spawned_hook;
			spawned_heading = heading;
		};
		h.on_local_player_redeployed =
				[this](int32_t) { calls.push_back("redeployed"); };
		h.on_mount_changed = [this] { calls.push_back("mount_changed"); };
		h.wire_collision_shape = [](uint16_t) {
			return world::ResolvedCollisionShape{};
		};
		h.apply_input_pre_tick = [this] { calls.push_back("input"); };
		h.sync_mounted_input_heading =
				[this] { calls.push_back("sync_heading"); };
		h.tick_view = [this] { calls.push_back("view"); };
		h.tick_weapon = [this] { calls.push_back("weapon"); };
		return h;
	}
};

// A pre-match pump runs the shell hooks in the exact witnessed order, ships the
// one-shot ClientHello, samples the tripwire on tick 0, and never pumps the
// weapon FSM (L does not exist).
bool run_pre_match_frame_order() {
	Harness h;
	np::ClientRuntime runtime("BridgeJoiner", [] { return uint64_t(0); });
	const auto hooks = h.hooks();

	h.bridge.pump(h.ctx(runtime), hooks);
	const std::vector<std::string> expected = {
			"adm", "deposit", "wire_leg", "loadout", "reseed", "diag",
			"input", "sync_heading", "view"};
	if (!expect(h.calls == expected, "pre-match pump: exact hook order")) {
		for (const std::string &c : h.calls) std::fprintf(stderr, "  %s\n", c.c_str());
		return false;
	}
	if (!expect(h.sends == 1, "pre-match pump: the ClientHello shipped once")) return false;
	if (!expect(h.bridge.started(), "pre-match pump: the hello latch armed")) return false;
	if (!expect(!h.bridge.local_spawned(), "pre-match pump: no L")) return false;
	if (!expect(h.bridge.now_tick() == 1, "pre-match pump: the clock advanced")) return false;

	// Frame 2: no second hello, no tripwire sample (1 % 62 != 0), still no weapon.
	h.calls.clear();
	h.bridge.pump(h.ctx(runtime), hooks);
	const std::vector<std::string> expected2 = {
			"adm", "deposit", "wire_leg", "loadout", "reseed",
			"input", "sync_heading", "view"};
	if (!expect(h.calls == expected2, "frame 2: order sans hello/diag")) return false;
	if (!expect(h.sends == 1, "frame 2: hello is a one-shot")) return false;
	return expect(h.bridge.now_tick() == 2, "frame 2: the clock advanced again");
}

// A seeded in-match runtime spawns L on the first pump (the in-match edge),
// fires the spawn hook with the pose heading, and opens the weapon-pump gate
// in that same frame (the gate reads the latch set earlier in the sequence).
bool run_in_match_spawn_edge() {
	Harness h;
	np::ClientRuntime runtime("BridgeJoiner", [] { return uint64_t(0); });
	const auto hooks = h.hooks();
	// Production order: the pre-load preload pump ships the ClientHello (the
	// bridge latch arms there), the session establishes later. runtime.start()
	// RESETS a session, so the latch must already be armed when the in-match
	// pump runs — exactly what poll_join_preload guarantees.
	h.bridge.send_hello_once(runtime,
			[&h](const std::vector<uint8_t> &) { ++h.sends; });
	if (!expect(h.bridge.started() && h.sends == 1,
			"in-match: the preload hello armed the latch")) return false;
	runtime.seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                     1, 0, /*self_handle=*/0x0005, w::kPlayerInfantryTypeId);
	if (!expect(runtime.in_match(), "in-match: seeded runtime is InMatch")) return false;

	h.bridge.pump(h.ctx(runtime), hooks);
	if (!expect(h.bridge.local_spawned(), "in-match pump: L spawned on the edge")) return false;
	if (!expect(h.sends == 1, "in-match pump: no second hello on the seeded session")) return false;
	if (!expect(h.world.cached.local_player.valid(),
			"in-match pump: cached.local_player published")) return false;
	if (!expect(h.spawned_hook == 1, "in-match pump: spawn hook fired once")) return false;
	if (!expect(h.bridge.self_wire_handle() == 0x0005,
			"in-match pump: H latched from the runtime")) return false;
	if (!expect(!h.calls.empty() && h.calls.back() == "weapon",
			"in-match pump: the weapon pump runs last, same frame")) return false;

	// The join-wait fire latches died at the spawn edge.
	if (!expect(!h.weapon.fire_held && !h.weapon.fire_pressed &&
					!h.weapon.reload_pressed,
			"in-match pump: join-wait weapon latches cleared")) return false;

	// Frame 2: the edge is a one-shot; the weapon gate stays open.
	h.calls.clear();
	h.bridge.pump(h.ctx(runtime), hooks);
	if (!expect(h.spawned_hook == 1, "frame 2: no re-spawn")) return false;
	return expect(!h.calls.empty() && h.calls.back() == "weapon",
			"frame 2: weapon pump still gated open");
}

// reset_for_join re-arms the per-session latches for a fresh dial.
bool run_reset_for_join() {
	Harness h;
	np::ClientRuntime runtime("BridgeJoiner", [] { return uint64_t(0); });
	const auto hooks = h.hooks();
	h.bridge.pump(h.ctx(runtime), hooks);
	if (!expect(h.bridge.started(), "reset: latch armed before")) return false;
	h.bridge.reset_for_join();
	if (!expect(!h.bridge.started() && !h.bridge.local_spawned() &&
					h.bridge.self_wire_handle() == 0 &&
					h.bridge.flat_seconds() == 0 &&
					!h.bridge.freeze_suspected(),
			"reset: per-session latches cleared")) return false;
	// The clock deliberately survives (retail's per-frame tick is process-scoped).
	return expect(h.bridge.now_tick() == 1, "reset: the clock is not a session latch");
}

// The D-NET-194 wire-header joiner: no pre-load kit, so inventory_valid is
// false at pump entry. The first S2C 0x5A grant folds and
// apply_authoritative_loadout() flips the live flag + arms the equipped slot
// INSIDE run_client_net_frame — i.e. BEFORE spawn_and_arm the SAME pump. The
// spawn edge must see that live flip (ctx.inventory_valid is a reference, not a
// pump-entry snapshot) and stamp L's equipped_adm_index; otherwise the entity
// carries the default adm on the C2S 0x0C uplink until the next respawn.
bool run_spawn_stamps_equipped_adm_from_midpump_grant() {
	Harness h;
	np::ClientRuntime runtime("BridgeJoiner", [] { return uint64_t(0); });
	auto hooks = h.hooks();
	// Model retail's mid-pump apply: flip the live inventory-valid flag and arm
	// the equipped combo, exactly as apply_authoritative_loadout ->
	// local_loadout_rebuild does before the spawn block runs.
	constexpr int16_t kGrantedAdm = 16; // WPN_M16BURST-shaped grant
	hooks.apply_authoritative_loadout = [&h, kGrantedAdm] {
		h.calls.push_back("loadout");
		h.inventory_valid = true;
		h.inventory.equipped_combo = 0;
		h.inventory.slots[0].adm_index = kGrantedAdm;
	};

	h.bridge.send_hello_once(runtime,
			[&h](const std::vector<uint8_t> &) { ++h.sends; });
	runtime.seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                     1, 0, /*self_handle=*/0x0005, w::kPlayerInfantryTypeId);
	if (!expect(runtime.in_match(), "midpump grant: seeded runtime InMatch"))
		return false;

	h.bridge.pump(h.ctx(runtime), hooks);
	if (!expect(h.bridge.local_spawned(), "midpump grant: L spawned")) return false;
	const w::Entity *L = h.world.registry.get(h.world.cached.local_player);
	if (!expect(L != nullptr, "midpump grant: L resolvable")) return false;
	return expect(L->equipped_adm_index == kGrantedAdm,
			"midpump grant: the same-pump grant stamped L's equipped adm "
			"(regression: a by-value inventory_valid froze it at pump entry)");
}

} // namespace

int main() {
	bool ok = true;
	ok &= run_pre_match_frame_order();
	ok &= run_in_match_spawn_edge();
	ok &= run_spawn_stamps_equipped_adm_from_midpump_grant();
	ok &= run_reset_for_join();
	if (!ok) return 1;
	std::printf("joiner_world_bridge_test: OK\n");
	return 0;
}
