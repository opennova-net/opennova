// The weapon view's lower / raise interp and the mount's queue (L7-s23 FOUND): a
// cross-category mount's queued SWITCHFROM lowers the held def's view pose and a SWITCHFROM
// completion raises the committed one's, each over 31 steps (one for an Emplaced def); while
// that interp runs, a same-category mount stores the word and queues no SWITCHRANK, and the
// next SWITCHFROM / SWITCHRANK completion commits it. Outside it a same-category mount
// queues SWITCHRANK at once, preempting a draw, and a cross-category mount's SWITCHFROM is
// refused over a running draw. A draw holds at the -901 sentinel while the pending word
// names an Emplaced def.
// [orig: Player_MountWeaponSlot @0x4DFA40 - the interp test @0x4DFB8D..0x4DFB93,
//  WeaponSlot_TryQueueSwitchRank @0x4DFB96, WeaponSlot_ForceQueueSwitchFrom @0x4DFB9E,
//  Player_StartWeaponLowerRaiseInterp(0) @0x4DFBC5; WeaponAction_SwitchFrom @0x5433B0 -
//  the store @0x543475, Player_StartWeaponLowerRaiseInterp(1) @0x5434AA;
//  Player_StartWeaponLowerRaiseInterp @0x4DF960; Player_UpdatePerFrame @0x4DE4FD..0x4DE522;
//  WeaponAction_SwitchTo @0x5431D0 - @0x54321E..0x543240]
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <formats/def/def.h>
#include <runtime/world/local_player.h>
#include <runtime/world/player_loadout.h>
#include <runtime/world/player_view.h>
#include <runtime/world/player_weapon.h>
#include <runtime/world/weapon_fsm.h>
#include <runtime/world/weapon_inventory.h>
#include <runtime/world/weapon_table_build.h>
#include <runtime/world/world.h>

using namespace opennova::world;
using namespace opennova::def;

namespace {

int failures = 0;
#define CHECK(c) \
	do { \
		if (!(c)) { \
			std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); \
			++failures; \
		} \
	} while (0)

// Two fire modes of one rifle (category 3, ranks 0 and 1), a pistol (category 2) and an
// Emplaced def at the rifle's rank 2, each with timed switch rows (an untimed SWITCHFROM
// falls back to its previous action with no commit; a SWITCHTO with no delaystart seeds its
// -900 timer on its first call: a 33-tick draw).
constexpr const char *kWeaponDef =
		"weapon \"WPN_RIFLE\"\r\n"
		"\tcategory 3\r\n"
		"\trank 0\r\n"
		"\tclipsize 30\r\n"
		"\tstartrounds 90\r\n"
		"\tammoclass CLASS_556MM 1\r\n"
		"\taction \"switchto\"\r\n"
		"\t\tdelaystart 0\r\n"
		"\t\tdelayend 2\r\n"
		"\tend\r\n"
		"\taction \"switchfrom\"\r\n"
		"\t\tdelaystart 2\r\n"
		"\t\tdelayend 2\r\n"
		"\tend\r\n"
		"end\r\n"
		"weapon \"WPN_RIFLE_B\"\r\n"
		"\tcategory 3\r\n"
		"\trank 1\r\n"
		"\tclipsize 30\r\n"
		"\tstartrounds 90\r\n"
		"\tammoclass CLASS_556MM 1\r\n"
		"\taction \"switchto\"\r\n"
		"\t\tdelaystart 0\r\n"
		"\t\tdelayend 2\r\n"
		"\tend\r\n"
		"\taction \"switchfrom\"\r\n"
		"\t\tdelaystart 2\r\n"
		"\t\tdelayend 2\r\n"
		"\tend\r\n"
		"end\r\n"
		"weapon \"WPN_RIFLE_E\"\r\n"
		"\tcategory 3\r\n"
		"\trank 2\r\n"
		"\tclipsize 100\r\n"
		"\tstartrounds 100\r\n"
		"\tammoclass CLASS_762MM 1\r\n"
		"\tflags Emplaced\r\n"
		"\taction \"switchto\"\r\n"
		"\t\tdelaystart 0\r\n"
		"\t\tdelayend 2\r\n"
		"\tend\r\n"
		"\taction \"switchfrom\"\r\n"
		"\t\tdelaystart 2\r\n"
		"\t\tdelayend 2\r\n"
		"\tend\r\n"
		"end\r\n"
		"weapon \"WPN_PISTOL\"\r\n"
		"\tcategory 2\r\n"
		"\trank 0\r\n"
		"\tclipsize 15\r\n"
		"\tstartrounds 45\r\n"
		"\tammoclass CLASS_9MM 1\r\n"
		"\taction \"switchto\"\r\n"
		"\t\tdelaystart 0\r\n"
		"\t\tdelayend 2\r\n"
		"\tend\r\n"
		"\taction \"switchfrom\"\r\n"
		"\t\tdelaystart 2\r\n"
		"\t\tdelayend 2\r\n"
		"\tend\r\n"
		"end\r\n";

constexpr int32_t kRifle = 3 * weapon_combo::kRanksPerCategory;
constexpr int32_t kRifleB = kRifle + 1;
constexpr int32_t kRifleE = kRifle + 2;
constexpr int32_t kPistol = 2 * weapon_combo::kRanksPerCategory;

// One local player on the parsed table, its kit rebuilt with the spawn select (the
// rifle's rank 0: the select plays no draw and starts no raise).
struct Rig {
	std::unique_ptr<World> owned_world = std::make_unique<World>();
	World &world = *owned_world;
	std::unique_ptr<LocalPlayer> owned_local;
	DefWeaponsFile defs{};
	bool parsed = false;

	explicit Rig(std::vector<WeaponKitEntry> kit) {
		const std::string text(kWeaponDef);
		parsed = def_parse_weapons_memory(reinterpret_cast<const uint8_t *>(text.data()),
				text.size(), &defs) == 0;
		world.tables.weapons = build_weapon_table(defs);
		world.tables.weapon_defs = &defs;
		world.registry.configure_pool(0, 4);
		world.registry.configure_pool(1, 4);
		Entity body;
		body.kind = EntityKind::Organic;
		body.alive = true;
		body.health = 100;
		body.flags = body.engine_flags = kEntityFlagPlayer;
		world.cached.local_player = world.registry.spawn(0, body);
		owned_local = std::make_unique<LocalPlayer>(world);
		world.local_player_state = owned_local.get();
		local().loadout.spawn_kit() = std::move(kit);
		local_loadout_rebuild(world, local().loadout, local().weapon, local().inventory,
				local().inventory_valid, /*select_spawn_default=*/true);
	}
	~Rig() {
		world.local_player_state = nullptr;
		world.tables.weapon_defs = nullptr;
		if (parsed) def_free_weapons(&defs);
	}

	LocalPlayer &local() { return *owned_local; }
	LocalPlayerWeapon &weapon() { return owned_local->weapon; }
	PlayerViewState &view() { return owned_local->view; }
	WeaponInventory &inventory() { return owned_local->inventory; }
	uint8_t adm(const char *name) const {
		return static_cast<uint8_t>(world.tables.weapons.index_of(name));
	}
	// The AdmDef byte the uplink carries (the body's equippedAdmIndex).
	uint8_t adm_byte() {
		const Entity *body = world.registry.get(world.cached.local_player);
		return body != nullptr ? body->equipped_adm_index : uint8_t{0xFF};
	}
	bool lower_raise_active() { return player_view_weapon_lower_raise_active(view()); }
	// A category key's walk and mount, in the frame's input phase.
	WeaponSwitchOutcome press(int32_t category) {
		WeaponInventory &inv = inventory();
		const WeaponSwitchOutcome out = weapon_switch_to_handle(world.tables.weapons, inv,
				category * weapon_combo::kRanksPerCategory,
				local_weapon_switch_gates(world, weapon(), &inv));
		handle_weapon_switch_outcome(world, weapon(), &inv, out, view());
		return out;
	}
	// One main frame past its input: Player_UpdatePerFrame's view leg, then the weapon walk.
	void tick() {
		++world.logic_tick;
		player_view_promote_tick(view());
		local().pump_local_weapon();
	}
	// Ticks until the EquippedSlot's combo changes; the tick count, or -1.
	int tick_until_commit(int limit = 200) {
		const int32_t before = inventory().equipped_combo;
		for (int t = 1; t <= limit; ++t) {
			tick();
			if (inventory().equipped_combo != before) return t;
		}
		return -1;
	}
	// Ticks until the interp is idle and the slot is idle again.
	void settle(int limit = 300) {
		for (int t = 0; t < limit; ++t) {
			if (!lower_raise_active() && weapon().slot.current == weapon_action::kIdle &&
					weapon().slot.next == weapon_action::kIdle)
				return;
			tick();
		}
		CHECK(false);
	}
};

std::vector<WeaponKitEntry> rifle_kit() {
	return {WeaponKitEntry{"WPN_RIFLE", -1, -1, -1}, WeaponKitEntry{"WPN_RIFLE_B", -1, -1, -1},
			WeaponKitEntry{"WPN_PISTOL", -1, -1, -1}};
}

// The interp's spans from CNetPlayerInterp_Setup's per-step velocity (the Z lane's 65536.0
// over 31 steps) and the stepper's drop on the call after the last moving lane snaps: the
// lower and the raise each hold the latch through 32 steps, a raise onto an Emplaced def
// through 2. [orig: Player_StartWeaponLowerRaiseInterp @0x4DF960; CNetPlayerInterp_Setup
//  @0x4DDFD0; Player_StepFpViewBiasInterp @0x4DDD20]
void test_the_lower_and_raise_spans() {
	const auto span = [](PlayerViewState &v) {
		int steps = 0;
		while (player_view_weapon_lower_raise_active(v) && steps < 100) {
			player_view_promote_tick(v);
			++steps;
		}
		return steps;
	};
	{
		auto v = std::make_unique<PlayerViewState>();
		player_view_start_weapon_lower(*v);
		CHECK(player_view_weapon_lower_raise_active(*v));
		CHECK(v->weapon_lower_raise_interp.target.position_q16[2] == -65536.0f);
		CHECK(span(*v) == 32);
		CHECK(v->weapon_lower_raise_interp.current.position_q16[2] == -65536.0f);
	}
	{
		auto v = std::make_unique<PlayerViewState>();
		player_view_start_weapon_raise(*v, /*emplaced=*/false);
		CHECK(v->weapon_lower_raise_interp.current.position_q16[2] == -65536.0f);
		CHECK(span(*v) == 32);
		CHECK(v->weapon_lower_raise_interp.current.position_q16[2] == 0.0f);
	}
	{
		auto v = std::make_unique<PlayerViewState>();
		player_view_start_weapon_raise(*v, /*emplaced=*/true);
		CHECK(span(*v) == 2);
	}
	// The scope ease's interp is a separate record: the lower leaves it idle.
	{
		auto v = std::make_unique<PlayerViewState>();
		player_view_start_weapon_lower(*v);
		CHECK(!player_view_scope_ease_active(*v));
	}
}

// Outside the interp a fire-mode switch queues SWITCHRANK on the press, and its completion
// commits the other mode with no draw and no raise; the AdmDef byte moves at that commit.
void test_a_fire_mode_switch_outside_the_interp_queues_switchrank_at_once() {
	const auto rig = std::make_unique<Rig>(rifle_kit());
	CHECK(rig->parsed);
	LocalPlayerWeapon &w = rig->weapon();
	CHECK(rig->inventory().equipped_combo == kRifle && rig->adm_byte() == rig->adm("WPN_RIFLE"));
	CHECK(!rig->lower_raise_active()); // the spawn select starts none
	rig->tick();
	const WeaponSwitchOutcome out = rig->press(3);
	CHECK(out.kind == WeaponSwitchOutcome::kMount && out.same_category && out.combo == kRifleB);
	CHECK(w.switch_in_flight && rig->inventory().pending_combo == kRifleB);
	CHECK(w.slot.next == weapon_action::kSwitchRank && w.slot.counter == 0);
	CHECK(!rig->lower_raise_active());
	const int committed = rig->tick_until_commit();
	CHECK(committed >= 1);
	CHECK(rig->inventory().equipped_combo == kRifleB);
	CHECK(rig->adm_byte() == rig->adm("WPN_RIFLE_B"));
	CHECK(w.slot.next != weapon_action::kSwitchTo);
	CHECK(!rig->lower_raise_active());
}

// A cross-category mount lowers at the press, and the SWITCHFROM's completion raises.
void test_a_category_switch_lowers_then_raises() {
	const auto rig = std::make_unique<Rig>(rifle_kit());
	LocalPlayerWeapon &w = rig->weapon();
	rig->tick();
	const WeaponSwitchOutcome out = rig->press(2);
	CHECK(out.kind == WeaponSwitchOutcome::kMount && !out.same_category);
	CHECK(w.slot.next == weapon_action::kSwitchFrom);
	CHECK(rig->lower_raise_active());
	CHECK(rig->view().weapon_lower_raise_interp.target.position_q16[2] == -65536.0f);
	CHECK(rig->tick_until_commit() >= 1);
	CHECK(rig->inventory().equipped_combo == kPistol);
	CHECK(rig->adm_byte() == rig->adm("WPN_PISTOL"));
	// The commit's tick: the draw queued, the raise running toward the hip pose.
	CHECK(w.slot.next == weapon_action::kSwitchTo);
	CHECK(rig->lower_raise_active());
	CHECK(rig->view().weapon_lower_raise_interp.target.position_q16[2] == 0.0f);
}

// Inside the raise after a draw, a fire-mode switch stores the word and queues nothing:
// the draw runs out, the rifle and its AdmDef byte stay, no SWITCHRANK runs. Once the
// interp is idle the same press queues SWITCHRANK at once.
void test_a_fire_mode_switch_inside_the_raise_stores_the_word() {
	const auto rig = std::make_unique<Rig>(rifle_kit());
	LocalPlayerWeapon &w = rig->weapon();
	rig->tick();
	rig->press(2);
	CHECK(rig->tick_until_commit() >= 1);
	rig->settle();
	CHECK(rig->inventory().equipped_combo == kPistol);
	// Back to the rifle: its SWITCHFROM's completion draws it and raises.
	const WeaponSwitchOutcome back = rig->press(3);
	CHECK(back.kind == WeaponSwitchOutcome::kMount && !back.same_category &&
			back.combo == kRifle);
	CHECK(rig->tick_until_commit() >= 1);
	CHECK(rig->inventory().equipped_combo == kRifle);
	for (int t = 0; t < 3; ++t) rig->tick();
	CHECK(rig->lower_raise_active());
	CHECK(w.slot.current == weapon_action::kSwitchTo);
	const WeaponSwitchOutcome mode = rig->press(3);
	CHECK(mode.kind == WeaponSwitchOutcome::kMount && mode.same_category &&
			mode.combo == kRifleB);
	CHECK(w.switch_in_flight && rig->inventory().pending_combo == kRifleB);
	CHECK(w.slot.current == weapon_action::kSwitchTo && w.slot.next != weapon_action::kSwitchRank);
	bool ran_rank = false;
	for (int t = 0; t < 120; ++t) {
		rig->tick();
		ran_rank = ran_rank || w.slot.current == weapon_action::kSwitchRank;
		CHECK(rig->inventory().equipped_combo == kRifle);
		CHECK(rig->adm_byte() == rig->adm("WPN_RIFLE"));
	}
	CHECK(!ran_rank);
	CHECK(!rig->lower_raise_active());
	CHECK(w.slot.current == weapon_action::kIdle);
	CHECK(w.switch_in_flight); // the word stays stored
	// The interp idle, the press queues SWITCHRANK and its completion commits.
	rig->press(3);
	CHECK(w.slot.next == weapon_action::kSwitchRank && w.slot.counter == 0);
	CHECK(rig->tick_until_commit() >= 1);
	CHECK(rig->inventory().equipped_combo == kRifleB);
	CHECK(rig->adm_byte() == rig->adm("WPN_RIFLE_B"));
}

// Inside the lower, a press of the held weapon's category stores its other mode and
// queues nothing: the running SWITCHFROM's completion commits that word (not the pistol),
// draws it and raises, and the AdmDef byte moves at that completion.
void test_a_fire_mode_switch_inside_the_lower_commits_at_the_switchfrom() {
	const auto rig = std::make_unique<Rig>(rifle_kit());
	LocalPlayerWeapon &w = rig->weapon();
	rig->tick();
	rig->press(2);
	for (int t = 0; t < 3; ++t) rig->tick();
	CHECK(w.slot.current == weapon_action::kSwitchFrom);
	CHECK(rig->lower_raise_active());
	const WeaponSwitchOutcome mode = rig->press(3);
	CHECK(mode.kind == WeaponSwitchOutcome::kMount && mode.same_category &&
			mode.combo == kRifleB);
	CHECK(w.slot.current == weapon_action::kSwitchFrom &&
			w.slot.next != weapon_action::kSwitchRank);
	int committed = -1;
	for (int t = 1; t <= 200 && committed < 0; ++t) {
		rig->tick();
		CHECK(w.slot.current != weapon_action::kSwitchRank);
		if (rig->inventory().equipped_combo != kRifle) committed = t;
		else CHECK(rig->adm_byte() == rig->adm("WPN_RIFLE"));
	}
	CHECK(committed >= 1);
	CHECK(rig->inventory().equipped_combo == kRifleB);
	CHECK(rig->adm_byte() == rig->adm("WPN_RIFLE_B"));
	CHECK(w.slot.next == weapon_action::kSwitchTo);
	CHECK(rig->lower_raise_active());
}

// Over a running draw with the interp idle: a same-category press preempts the draw with
// SWITCHRANK (TryQueueSwitchRank zeroes the counter and ProcessFrame's transition takes it on
// the next tick); a cross-category press is refused (the word stays stored, nothing queues,
// no lower), and the draw runs out on the held weapon.
void test_a_press_over_a_draw_outside_the_interp() {
	const auto make_drawing = [](Rig &rig) {
		WeaponSlotState &slot = rig.weapon().slot;
		slot.current = weapon_action::kSwitchTo;
		slot.next = weapon_action::kIdle;
		slot.phase = weapon_phase::kDone;
		slot.counter = 2;
		slot.switch_timer = -600;
	};
	{
		const auto rig = std::make_unique<Rig>(rifle_kit());
		LocalPlayerWeapon &w = rig->weapon();
		rig->tick();
		make_drawing(*rig);
		CHECK(!rig->lower_raise_active());
		rig->press(3);
		CHECK(w.slot.next == weapon_action::kSwitchRank && w.slot.counter == 0);
		// The next tick's transition enters SWITCHRANK over the draw, and its completion
		// commits the other mode in that tick.
		rig->tick();
		CHECK(rig->inventory().equipped_combo == kRifleB);
		CHECK(rig->adm_byte() == rig->adm("WPN_RIFLE_B"));
	}
	{
		const auto rig = std::make_unique<Rig>(rifle_kit());
		LocalPlayerWeapon &w = rig->weapon();
		rig->tick();
		make_drawing(*rig);
		const WeaponSwitchOutcome out = rig->press(2);
		CHECK(out.kind == WeaponSwitchOutcome::kMount && !out.same_category);
		CHECK(w.slot.current == weapon_action::kSwitchTo && w.slot.next == weapon_action::kIdle);
		CHECK(w.slot.counter == 2);
		CHECK(!rig->lower_raise_active());
		CHECK(w.switch_in_flight && rig->inventory().pending_combo == kPistol);
		for (int t = 0; t < 120; ++t) rig->tick();
		CHECK(w.slot.current == weapon_action::kIdle);
		CHECK(rig->inventory().equipped_combo == kRifle);
		CHECK(rig->adm_byte() == rig->adm("WPN_RIFLE"));
	}
}

// WeaponAction_SwitchTo's hold: for the local player with a pending word naming an Emplaced
// def, every call sets -901 and adds 30 back (-871), so the draw never completes; a remote
// owner's draw, or one with no such word, runs out.
void test_the_switchto_hold_reads_the_pending_word() {
	WeaponFsmActionRow row;
	std::snprintf(row.name, sizeof(row.name), "%s", "switchto");
	row.delaystart = 0;
	row.delayend = 1;
	WeaponFsmDef def;
	weapon_fsm_bake(&row, 1, nullptr, nullptr, nullptr, def);
	const auto run = [&](bool is_local, bool pending_emplaced, int ticks, WeaponSlotState &slot) {
		slot = WeaponSlotState{};
		weapon_fsm_try_queue_switch_to(slot);
		WeaponFsmInputs in;
		in.is_local = is_local;
		in.pending_emplaced = pending_emplaced;
		for (int t = 0; t < ticks; ++t) {
			WeaponFsmEvents ev;
			weapon_fsm_tick(def, slot, in, ev);
		}
	};
	WeaponSlotState held;
	run(true, true, 200, held);
	CHECK(held.current == weapon_action::kSwitchTo);
	CHECK(held.switch_timer == -871);
	WeaponSlotState remote;
	run(false, true, 200, remote);
	CHECK(remote.current != weapon_action::kSwitchTo);
	WeaponSlotState plain;
	run(true, false, 200, plain);
	CHECK(plain.current != weapon_action::kSwitchTo);
}

// The hold under the gate: inside the raise a press of the drawn rifle's category stores
// its Emplaced rank, and the draw holds while that word stays; a later store of a word
// with no Emplaced def (a refused cross-category press) releases it, and the draw runs out
// on the rifle.
void test_a_gated_emplaced_word_holds_the_draw() {
	const auto rig = std::make_unique<Rig>(std::vector<WeaponKitEntry>{
			WeaponKitEntry{"WPN_RIFLE", -1, -1, -1}, WeaponKitEntry{"WPN_RIFLE_E", -1, -1, -1},
			WeaponKitEntry{"WPN_PISTOL", -1, -1, -1}});
	LocalPlayerWeapon &w = rig->weapon();
	rig->tick();
	rig->press(2);
	CHECK(rig->tick_until_commit() >= 1);
	rig->settle();
	rig->press(3);
	CHECK(rig->tick_until_commit() >= 1);
	CHECK(rig->inventory().equipped_combo == kRifle);
	for (int t = 0; t < 3; ++t) rig->tick();
	CHECK(rig->lower_raise_active() && w.slot.current == weapon_action::kSwitchTo);
	const WeaponSwitchOutcome mode = rig->press(3);
	CHECK(mode.kind == WeaponSwitchOutcome::kMount && mode.same_category &&
			mode.combo == kRifleE);
	for (int t = 0; t < 150; ++t) rig->tick();
	CHECK(w.slot.current == weapon_action::kSwitchTo);
	CHECK(w.slot.switch_timer == -871);
	CHECK(rig->inventory().equipped_combo == kRifle);
	CHECK(rig->adm_byte() == rig->adm("WPN_RIFLE"));
	// The pistol's key: refused over the draw, it stores the pistol's word.
	rig->press(2);
	CHECK(rig->inventory().pending_combo == kPistol);
	for (int t = 0; t < 60; ++t) rig->tick();
	CHECK(w.slot.current == weapon_action::kIdle);
	CHECK(rig->inventory().equipped_combo == kRifle);
	CHECK(rig->adm_byte() == rig->adm("WPN_RIFLE"));
}

} // namespace

int main() {
	test_the_lower_and_raise_spans();
	test_a_fire_mode_switch_outside_the_interp_queues_switchrank_at_once();
	test_a_category_switch_lowers_then_raises();
	test_a_fire_mode_switch_inside_the_raise_stores_the_word();
	test_a_fire_mode_switch_inside_the_lower_commits_at_the_switchfrom();
	test_a_press_over_a_draw_outside_the_interp();
	test_the_switchto_hold_reads_the_pending_word();
	test_a_gated_emplaced_word_holds_the_draw();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("weapon_lower_raise_test OK\n");
	return 0;
}
