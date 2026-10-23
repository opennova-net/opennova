// The weapon view's lower / raise interp and the mount's queue (L7-s23 FOUND): a
// cross-category mount's queued SWITCHFROM lowers the held def's view pose and a SWITCHFROM
// completion raises the committed one's, each over 31 steps (one for an Emplaced def); while
// that interp runs, a same-category mount stores the word and queues no SWITCHRANK, and the
// next SWITCHFROM / SWITCHRANK completion commits it. Outside it a same-category mount
// queues SWITCHRANK at once, preempting a draw, and a cross-category mount's SWITCHFROM is
// refused over a running draw. A draw holds at the -901 sentinel while the pending word
// names an Emplaced def. The UseGun attach / swap / detach mounts take the same leg (L7-s25),
// and both interps publish the six shared view-bias words the mount request clears.
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
#include <runtime/world/ai.h>
#include <runtime/world/entity.h>
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
// -900 timer on its first call: a 33-tick draw). The classes carry 1000-round caps, as the
// shipped table's do: a loadout apply's pool writes clamp to the class's cap, which a class
// with no ammoclass_max_carry line holds at 0 (an apply would empty every slot)
// [orig: WeaponSlot_SetAmmoCount @0x540B50 - the g_AmmoClassMaxCarry clamp @0x540C32..0x540C45].
constexpr const char *kWeaponDef =
		"ammoclass_max_carry CLASS_556MM 1000\r\n"
		"ammoclass_max_carry CLASS_762MM 1000\r\n"
		"ammoclass_max_carry CLASS_9MM 1000\r\n"
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
		"end\r\n"
		// Three guns of one category for UseGun seats: two Emplaced, one not.
		"weapon \"WPN_GUN_E\"\r\n"
		"\tcategory 9\r\n"
		"\trank 0\r\n"
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
		"weapon \"WPN_GUN_F\"\r\n"
		"\tcategory 9\r\n"
		"\trank 1\r\n"
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
		"weapon \"WPN_GUN_N\"\r\n"
		"\tcategory 9\r\n"
		"\trank 2\r\n"
		"\tclipsize 100\r\n"
		"\tstartrounds 100\r\n"
		"\tammoclass CLASS_762MM 1\r\n"
		"\taction \"switchto\"\r\n"
		"\t\tdelaystart 0\r\n"
		"\t\tdelayend 2\r\n"
		"\tend\r\n"
		"\taction \"switchfrom\"\r\n"
		"\t\tdelaystart 2\r\n"
		"\t\tdelayend 2\r\n"
		"\tend\r\n"
		"end\r\n"
		// A ForceScoped optic (category 6) with distinct hip and sight poses and a 300 m
		// default zero (Def+0xA0), and an AbsorbPitch def (category 8) spanning -10..50
		// degrees (L7-s26 row 3).
		"weapon \"WPN_FORCED\"\r\n"
		"\tcategory 6\r\n"
		"\trank 0\r\n"
		"\tclipsize 5\r\n"
		"\tstartrounds 20\r\n"
		"\tammoclass CLASS_762MM 1\r\n"
		"\tflags Scoped\r\n"
		"\tflags ForceScoped\r\n"
		"\tscope_max_zero 10 25 300\r\n"
		"\tpos 1,2,3,0,0,0\r\n"
		"\ttpos 4,5,6,0,0,0\r\n"
		"\taction \"switchto\"\r\n"
		"\t\tdelaystart 0\r\n"
		"\t\tdelayend 2\r\n"
		"\tend\r\n"
		"\taction \"switchfrom\"\r\n"
		"\t\tdelaystart 2\r\n"
		"\t\tdelayend 2\r\n"
		"\tend\r\n"
		"end\r\n"
		"weapon \"WPN_MORTAR\"\r\n"
		"\tcategory 8\r\n"
		"\trank 0\r\n"
		"\tclipsize 1\r\n"
		"\tstartrounds 10\r\n"
		"\tammoclass CLASS_762MM 1\r\n"
		"\tflags AbsorbPitch\r\n"
		"\ttargetpitchmin 10\r\n"
		"\ttargetpitchmax 50\r\n"
		"\taction \"switchto\"\r\n"
		"\t\tdelaystart 0\r\n"
		"\t\tdelayend 2\r\n"
		"\tend\r\n"
		"\taction \"switchfrom\"\r\n"
		"\t\tdelaystart 2\r\n"
		"\t\tdelayend 2\r\n"
		"\tend\r\n"
		"end\r\n"
		// A ForceCrouch def (category 5) and two ForceCrouch + OnlyScoped ranks (category 7),
		// the retail mortar's pair of flags (L7-s26 row 4).
		"weapon \"WPN_CROUCHER\"\r\n"
		"\tcategory 5\r\n"
		"\trank 0\r\n"
		"\tclipsize 1\r\n"
		"\tstartrounds 10\r\n"
		"\tammoclass CLASS_762MM 1\r\n"
		"\tflags ForceCrouch\r\n"
		"end\r\n"
		"weapon \"WPN_PIT_A\"\r\n"
		"\tcategory 7\r\n"
		"\trank 0\r\n"
		"\tclipsize 1\r\n"
		"\tstartrounds 10\r\n"
		"\tammoclass CLASS_762MM 1\r\n"
		"\tflags ForceCrouch\r\n"
		"\tflags OnlyScoped\r\n"
		"end\r\n"
		"weapon \"WPN_PIT_B\"\r\n"
		"\tcategory 7\r\n"
		"\trank 1\r\n"
		"\tclipsize 1\r\n"
		"\tstartrounds 10\r\n"
		"\tammoclass CLASS_762MM 1\r\n"
		"\tflags ForceCrouch\r\n"
		"\tflags OnlyScoped\r\n"
		"end\r\n"
		// An optic (Scoped) rank and an iron-sighted one of one category, 4 (L7-s27).
		"weapon \"WPN_OPTIC\"\r\n"
		"\tcategory 4\r\n"
		"\trank 0\r\n"
		"\tclipsize 10\r\n"
		"\tstartrounds 40\r\n"
		"\tammoclass CLASS_762MM 1\r\n"
		"\tflags Scoped\r\n"
		"end\r\n"
		"weapon \"WPN_IRON\"\r\n"
		"\tcategory 4\r\n"
		"\trank 1\r\n"
		"\tclipsize 10\r\n"
		"\tstartrounds 40\r\n"
		"\tammoclass CLASS_762MM 1\r\n"
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

// --- the UseGun mounts (L7-s25 row 0) ---------------------------------------------------

// An EWeap gun with one UseGun seat (bone 2), its slot prepared for `weapon`.
EntityHandle spawn_gun(World &world, const char *weapon, float x) {
	Entity g;
	g.kind = EntityKind::Item;
	g.has_item_def = true;
	g.has_graphic_model = true;
	g.item_type = 6;
	g.item_attrib = kItemAttribEweap | kItemAttribPlayerControl;
	g.health = g.health_max = 1000;
	g.alive = true;
	g.position = {x, 2.0f, 0.0f};
	g.primary_weapon = weapon;
	Seat u;
	u.type = SeatType::Gunner;
	u.bone_index = 2;
	u.source_name = "UseGun";
	u.seat_local = {0.0f, -1.0f, 1.0f};
	g.seats.push_back(u);
	const EntityHandle h = world.registry.spawn(1, g);
	Entity *spawned = world.registry.get(h);
	CHECK(spawned != nullptr && world.vehicles.prepare_weapon_slot(*spawned));
	return h;
}

// The rig's body with its AI record, as a seat's attach reads it.
void give_body_an_ai(World &world) {
	AiEntity *ae = world.ai.at(world.ai.attach(world.cached.local_player));
	if (ae == nullptr) return;
	ae->health = 100;
	ae->inf.active = true;
}

WeaponSlotState &gun_slot(World &world, EntityHandle gun) {
	return world.registry.get(gun)->primary_weapon_slot;
}

// Seats L on the gun (the authority's process) and ticks until the gun's slot is the
// EquippedSlot and its draw and the raise have run out.
void seat_and_settle(Rig &rig, EntityHandle gun) {
	World &world = rig.world;
	CHECK(world.vehicles.attach_to_seat(world.cached.local_player, {gun, 0, SeatType::Gunner}));
	for (int t = 0; t < 300; ++t) {
		const WeaponSlotState &slot = gun_slot(world, gun);
		if (rig.weapon().usegun_slot_active && rig.weapon().usegun_mount == gun &&
				!rig.lower_raise_active() && slot.current == weapon_action::kIdle &&
				slot.next == weapon_action::kIdle)
			return;
		rig.tick();
	}
	CHECK(false);
}

// A UseGun attach whose relation lands over a running draw: the attach's mount is
// Player_MountWeaponSlot's, so its cross-category ForceQueueSwitchFrom is refused over the
// SWITCHTO, nothing is queued or retained, no lower starts, and the word stays stored. With
// an Emplaced gun the draw then holds at the -901 sentinel; with a plain one it runs out to
// its previous action and the player sits on the gun holding the rifle, the gun uncommitted.
// [orig: Entity_AttachToUseGunSlot @0x546B80 (@0x546C38); Player_MountWeaponSlot @0x4DFA40
//  (@0x4DFB9E..0x4DFBC5); WeaponSlot_ForceQueueSwitchFrom @0x53F170 (@0x53F17A..0x53F185);
//  WeaponAction_SwitchTo @0x5431D0 (the next = prev seed @0x543204..0x543213, the -901 hold
//  @0x54321E..0x543240)]
void test_a_usegun_attach_over_a_draw_queues_nothing() {
	for (const bool emplaced : {true, false}) {
		const auto rig = std::make_unique<Rig>(rifle_kit());
		World &world = rig->world;
		LocalPlayerWeapon &w = rig->weapon();
		give_body_an_ai(world);
		const EntityHandle gun = spawn_gun(world, emplaced ? "WPN_GUN_E" : "WPN_GUN_N", 4.0f);
		rig->tick();
		WeaponSlotState &slot = w.slot;
		slot.current = weapon_action::kSwitchTo;
		slot.next = weapon_action::kIdle;
		slot.phase = weapon_phase::kDone;
		slot.counter = 2;
		slot.switch_timer = -600;
		// A client's relation: the attach is forced, whatever the slot runs.
		world.ai.is_authority = false;
		CHECK(world.vehicles.apply_confirmed_mount(world.cached.local_player, gun, 2));
		rig->tick(); // the transition stages the attach's mount
		CHECK(w.usegun_switch == LocalUseGunSwitch::kAttach && !w.usegun_slot_active);
		CHECK(w.switch_deferred_action == -1);
		CHECK(slot.next != weapon_action::kSwitchFrom);
		CHECK(!rig->lower_raise_active());
		bool ran_from = false;
		for (int t = 0; t < 150; ++t) {
			rig->tick();
			ran_from = ran_from || slot.current == weapon_action::kSwitchFrom ||
					slot.next == weapon_action::kSwitchFrom;
		}
		CHECK(!ran_from);
		CHECK(!w.usegun_slot_active && w.usegun_switch == LocalUseGunSwitch::kAttach);
		CHECK(rig->inventory().equipped_combo == kRifle);
		CHECK(rig->adm_byte() == rig->adm("WPN_RIFLE"));
		if (emplaced) {
			CHECK(slot.current == weapon_action::kSwitchTo && slot.switch_timer == -871);
		} else {
			CHECK(slot.current == weapon_action::kIdle);
		}
	}
}

// A seat detach's switch back is the detach's own mount: inside the detach it force-queues
// SWITCHFROM on the gun's slot and lowers the view (the gun's category against the
// rifle's), and that SWITCHFROM's completion stores the rifle, draws it and raises it.
// [orig: Entity_DetachFromVehicle @0x4355F0 (+0x308 @0x43563A, Player_MountWeaponSlot
//  @0x43565F); Player_MountWeaponSlot @0x4DFA40 (@0x4DFB9E..0x4DFBC5);
//  WeaponAction_SwitchFrom @0x5433B0 (@0x543475, @0x5434A3, @0x5434AA)]
void test_a_usegun_detach_mounts_the_personal_slot_inside_the_detach() {
	const auto rig = std::make_unique<Rig>(rifle_kit());
	World &world = rig->world;
	LocalPlayerWeapon &w = rig->weapon();
	give_body_an_ai(world);
	const EntityHandle gun = spawn_gun(world, "WPN_GUN_E", 4.0f);
	rig->tick();
	seat_and_settle(*rig, gun);
	CHECK(rig->adm_byte() == rig->adm("WPN_GUN_E"));
	CHECK(world.vehicles.detach(world.cached.local_player));
	CHECK(w.usegun_switch == LocalUseGunSwitch::kDetach);
	CHECK(rig->inventory().pending_combo == kRifle);
	CHECK(gun_slot(world, gun).next == weapon_action::kSwitchFrom);
	CHECK(rig->lower_raise_active());
	CHECK(rig->view().weapon_lower_raise_interp.target.position_q16[2] == -65536.0f);
	CHECK(w.switch_deferred_action == -1);
	int committed = -1;
	for (int t = 1; t <= 60 && committed < 0; ++t) {
		rig->tick();
		if (!w.usegun_slot_active) committed = t;
	}
	CHECK(committed >= 1);
	CHECK(w.usegun_switch == LocalUseGunSwitch::kNone);
	CHECK(rig->inventory().equipped_combo == kRifle);
	CHECK(rig->adm_byte() == rig->adm("WPN_RIFLE"));
	CHECK(w.slot.next == weapon_action::kSwitchTo || w.slot.current == weapon_action::kSwitchTo);
	CHECK(rig->lower_raise_active());
	CHECK(rig->view().weapon_lower_raise_interp.target.position_q16[2] == 0.0f);
}

// A gun-to-gun move: the process's detach mounts the rifle's slot first (SWITCHFROM on gun
// E's slot, the lower), then the attach mounts gun F's over it; F's category is E's, and
// inside the lower that mount stores the word and queues no SWITCHRANK. E's SWITCHFROM
// then commits F, draws it and raises it.
// [orig: Entity_ProcessVehicleAttach @0x435AA0 (the requester's detach @0x435BCE ahead of
//  the attach); Entity_DetachFromVehicle @0x4355F0 (@0x43565F); Entity_AttachToUseGunSlot
//  @0x546B80 (@0x546C38); Player_MountWeaponSlot @0x4DFA40 (@0x4DFB8D..0x4DFB96);
//  WeaponAction_SwitchFrom @0x5433B0 (@0x543475, @0x5434A3, @0x5434AA)]
void test_a_gun_to_gun_move_inside_the_lower_commits_at_the_switchfrom() {
	const auto rig = std::make_unique<Rig>(rifle_kit());
	World &world = rig->world;
	LocalPlayerWeapon &w = rig->weapon();
	give_body_an_ai(world);
	const EntityHandle gun_e = spawn_gun(world, "WPN_GUN_E", 4.0f);
	const EntityHandle gun_f = spawn_gun(world, "WPN_GUN_F", -4.0f);
	rig->tick();
	seat_and_settle(*rig, gun_e);
	CHECK(world.vehicles.attach_to_seat(world.cached.local_player, {gun_f, 0, SeatType::Gunner}));
	CHECK(rig->lower_raise_active()); // the detach's lower
	CHECK(gun_slot(world, gun_e).next == weapon_action::kSwitchFrom);
	bool ran_rank = false;
	int committed = -1;
	for (int t = 1; t <= 60 && committed < 0; ++t) {
		rig->tick();
		const WeaponSlotState &e_slot = gun_slot(world, gun_e);
		ran_rank = ran_rank || e_slot.current == weapon_action::kSwitchRank ||
				e_slot.next == weapon_action::kSwitchRank;
		if (w.usegun_mount == gun_f && w.usegun_switch == LocalUseGunSwitch::kNone) committed = t;
	}
	CHECK(!ran_rank);
	CHECK(committed >= 1);
	CHECK(w.usegun_slot_active);
	CHECK(rig->adm_byte() == rig->adm("WPN_GUN_F"));
	const WeaponSlotState &f_slot = gun_slot(world, gun_f);
	CHECK(f_slot.next == weapon_action::kSwitchTo || f_slot.current == weapon_action::kSwitchTo);
	CHECK(rig->lower_raise_active()); // the raise onto F (one step)
	CHECK(rig->view().weapon_lower_raise_interp.target.position_q16[2] == 0.0f);
}

// --- the published view bias (L7-s25 row 1) ---------------------------------------------

// Both interps write the same six words, the lower / raise last: a frame both step
// publishes the lower's (its X lane 0 where the scope's is 21), the lowered Z's first step
// ftol(-65536 / 31) = -2114, and the viewmodel's position reads those words.
// [orig: Player_UpdatePerFrame @0x4DE4C9 / @0x4DE4FD..0x4DE522; Player_StepFpViewBiasInterp
//  @0x4DDF42..0x4DDFC3; Player_UpdateFirstPersonCamera @0x4DD4CE..0x4DD4DA]
void test_the_last_interp_step_publishes_the_view_bias() {
	auto v = std::make_unique<PlayerViewState>();
	v->weapon_pose_bound = true;
	v->weapon_ads_pose = {{315.0f, 0.0f, 0.0f}, {0u, 0u, 0u}};
	CHECK(player_view_set_engaged(*v, true, false));
	player_view_promote_tick(*v);
	CHECK(v->view_bias.position_q16[0] == 21); // the scope's step alone
	player_view_start_weapon_lower(*v);
	player_view_promote_tick(*v);
	CHECK(v->view_bias.position_q16[0] == 0);
	CHECK(v->view_bias.position_q16[2] == -2114);
	const float pos[3] = {10.0f, 20.0f, 30.0f};
	float out[3];
	player_view_bias_units(*v, pos, out);
	CHECK(out[0] == 10.0f && out[2] == 30.0f - 2114.0f / kWeaponDefPosScale);
	// The lower runs out at the full drop, its last moving step the last write.
	for (int t = 0; t < 40; ++t) player_view_promote_tick(*v);
	CHECK(!player_view_weapon_lower_raise_active(*v));
	CHECK(v->view_bias.position_q16[2] == -65536);
}

// The mount request clears the six words; the commit that stores the slot later clears
// none, so the lowered words hold across the SWITCHFROM's commit until the raise's first
// step publishes. A gated mount inside the raise queues nothing and still clears them.
// [orig: Player_MountWeaponSlot @0x4DFA40 (the clear @0x4DFBCF..0x4DFBE8);
//  WeaponAction_SwitchFrom @0x5433B0 / WeaponAction_SwitchRank @0x543500 write none]
void test_the_mount_request_clears_the_view_bias_and_the_commit_does_not() {
	const auto rig = std::make_unique<Rig>(rifle_kit());
	LocalPlayerWeapon &w = rig->weapon();
	rig->tick();
	rig->press(2);
	CHECK(rig->lower_raise_active());
	CHECK(rig->view().view_bias.position_q16[2] == 0);
	// One main frame at a time, the view leg ahead of the walk, until the commit.
	bool held = false;
	for (int t = 0; t < 200 && !held; ++t) {
		++rig->world.logic_tick;
		player_view_promote_tick(rig->view());
		const PlayerViewBias before = rig->view().view_bias;
		const int32_t combo = rig->inventory().equipped_combo;
		rig->local().pump_local_weapon();
		if (rig->inventory().equipped_combo != combo) {
			held = true;
			CHECK(before.position_q16[2] < 0);
			for (int i = 0; i < 3; ++i)
				CHECK(rig->view().view_bias.position_q16[i] == before.position_q16[i] &&
						rig->view().view_bias.rotation_bam[i] == before.rotation_bam[i]);
		}
	}
	CHECK(held && rig->inventory().equipped_combo == kPistol);
	// The raise's first step publishes from the lowered pose.
	rig->tick();
	CHECK(rig->view().view_bias.position_q16[2] < 0);
	CHECK(rig->lower_raise_active());
	// Back to the rifle, then a fire-mode press inside its raise: gated, it queues nothing
	// and still clears the words, which the raise's next step publishes again.
	rig->settle();
	rig->press(3);
	CHECK(rig->tick_until_commit() >= 1);
	for (int t = 0; t < 3; ++t) rig->tick();
	CHECK(rig->lower_raise_active() && rig->view().view_bias.position_q16[2] < 0);
	const WeaponSwitchOutcome gated = rig->press(3);
	CHECK(gated.kind == WeaponSwitchOutcome::kMount && gated.same_category);
	CHECK(w.slot.next != weapon_action::kSwitchRank);
	for (int i = 0; i < 3; ++i)
		CHECK(rig->view().view_bias.position_q16[i] == 0 &&
				rig->view().view_bias.rotation_bam[i] == 0);
	rig->tick();
	CHECK(rig->view().view_bias.position_q16[2] < 0);
}

// --- the claimant's held-ready EquippedSlot (L7-s26 row 1) ------------------------------

// A PlayerControl carrier with a drvrx seat (bone 3, slot 8) and a sitex seat (bone 4,
// slot 0).
EntityHandle spawn_car(World &world, float x) {
	Entity v;
	v.kind = EntityKind::Item;
	v.has_item_def = true;
	v.has_graphic_model = true;
	v.item_attrib = kItemAttribPlayerControl;
	v.health = v.health_max = 2000;
	v.alive = true;
	v.position = {x, -6.0f, 0.0f};
	Seat driver;
	driver.type = SeatType::Driver;
	driver.bone_index = 3;
	driver.retail_slot = 8;
	driver.source_name = "drvrx00";
	v.seats.push_back(driver);
	Seat passenger;
	passenger.type = SeatType::Passenger;
	passenger.bone_index = 4;
	passenger.retail_slot = 0;
	passenger.source_name = "sitex00";
	v.seats.push_back(passenger);
	return world.registry.spawn(1, v);
}

// The detach clears the carrier's occupant word and, for the local player who held it,
// stamps the EquippedSlot's phase 64 after the detach's own mount. A UseGun seat's is the
// gun's slot, whose queued switch back the pump then enters by the held variant (the phase
// re-stamped 64 past the entered handler); the handler after it consumes the byte at the
// begin-active shim (ACTIVE), and the switch back runs out and commits the rifle. A Driver's
// is the personal slot (no mount); a passenger held no word and its slot keeps its phase.
// [orig: Entity_DetachFromVehicle @0x4355F0 (the mount @0x43565F, the word's clear
//  @0x43577C, the local test @0x43578C, EquippedSlot->phase = 64 @0x435794);
//  WeaponAction_ProcessFrame @0x540E60 (the held variant @0x541453..0x54148E);
//  ActionSlot_BeginActivePhase @0x53F830 (@0x53F88B..0x53F891)]
void test_a_claimants_detach_holds_its_equipped_slot_ready() {
	{
		const auto rig = std::make_unique<Rig>(rifle_kit());
		World &world = rig->world;
		LocalPlayerWeapon &w = rig->weapon();
		give_body_an_ai(world);
		const EntityHandle gun = spawn_gun(world, "WPN_GUN_N", 4.0f);
		rig->tick();
		seat_and_settle(*rig, gun);
		CHECK(world.registry.get(gun)->primary_occupant == world.cached.local_player);
		WeaponSlotState &slot = gun_slot(world, gun);
		CHECK(slot.phase != weapon_phase::kHeld);
		CHECK(world.vehicles.detach(world.cached.local_player));
		CHECK(!world.registry.get(gun)->primary_occupant.valid());
		CHECK(slot.next == weapon_action::kSwitchFrom && slot.current == weapon_action::kIdle);
		CHECK(slot.phase == weapon_phase::kHeld);
		CHECK(w.usegun_slot_active && w.usegun_switch == LocalUseGunSwitch::kDetach);
		rig->tick(); // the held variant enters SWITCHFROM and stamps 64 again
		CHECK(slot.current == weapon_action::kSwitchFrom && slot.phase == weapon_phase::kHeld);
		rig->tick(); // the begin-active shim consumes it
		CHECK(slot.current == weapon_action::kSwitchFrom && slot.phase == weapon_phase::kActive);
		int committed = -1;
		for (int t = 1; t <= 60 && committed < 0; ++t) {
			rig->tick();
			if (!w.usegun_slot_active) committed = t;
		}
		CHECK(committed >= 1);
		CHECK(rig->inventory().equipped_combo == kRifle);
		CHECK(rig->adm_byte() == rig->adm("WPN_RIFLE"));
	}
	for (const SeatType seat : {SeatType::Driver, SeatType::Passenger}) {
		const auto rig = std::make_unique<Rig>(rifle_kit());
		World &world = rig->world;
		LocalPlayerWeapon &w = rig->weapon();
		give_body_an_ai(world);
		const EntityHandle car = spawn_car(world, 4.0f);
		rig->tick();
		rig->settle();
		const int seat_index = seat == SeatType::Driver ? 0 : 1;
		CHECK(world.vehicles.attach_to_seat(world.cached.local_player, {car, seat_index, seat}));
		CHECK((world.registry.get(car)->primary_occupant == world.cached.local_player) ==
				(seat == SeatType::Driver));
		for (int t = 0; t < 3; ++t) rig->tick();
		const uint8_t before = w.slot.phase;
		CHECK(before != weapon_phase::kHeld);
		CHECK(world.vehicles.detach(world.cached.local_player));
		CHECK(!w.usegun_slot_active && w.usegun_switch == LocalUseGunSwitch::kNone);
		if (seat == SeatType::Driver) {
			CHECK(w.slot.phase == weapon_phase::kHeld);
		} else {
			CHECK(w.slot.phase == before);
		}
	}
}

// --- the loadout applies' mount (L7-s26 row 2) ------------------------------------------

// Whether a record from `first` on names a switch (a commit's mount).
bool switch_recorded_since(const LocalPlayerWeapon &w, size_t first) {
	for (size_t i = first; i < w.events.size(); ++i)
		if (!w.events[i].switch_to_weapon.empty()) return true;
	return false;
}

// A loadout apply ends on its select and then the mount of the EquippedSlot that select
// stored: the slot's def is its own category, so the mount stores the word, queues
// SWITCHRANK on the slot and clears the six view-bias words. The completion stores the
// same slot again, which mounts nothing and draws nothing: the slot runs on in its
// SWITCHRANK, no SWITCHTO, no raise, the rifle and its AdmDef byte kept. Inside a raise
// the mount stores the word and queues nothing, and the word stays stored.
// [orig: NapiNPClientMsg_HandleWeaponLoadoutSync @0x4290E0 (the select @0x4296E3, the mount
//  @0x4296E8..0x4296F4); WeaponLoadout_ApplyFromBuffer @0x565CD0 (@0x566285, @0x566297);
//  Player_MountWeaponSlot @0x4DFA40 (@0x4DFB16, @0x4DFB8B..0x4DFB96, @0x4DFBCF..0x4DFBE8);
//  WeaponAction_SwitchRank @0x543500 (@0x543539)]
void test_a_loadout_apply_mounts_its_selected_slot() {
	{
		const auto rig = std::make_unique<Rig>(rifle_kit());
		LocalPlayerWeapon &w = rig->weapon();
		rig->tick();
		rig->settle();
		rig->view().view_bias.position_q16[2] = -100;
		CHECK(local_loadout_apply_accept(rig->world, rig->local().loadout, w, rig->inventory(),
				rig->local().inventory_valid, rifle_kit(), 0, false));
		CHECK(rig->inventory().equipped_combo == kRifle);
		CHECK(w.switch_in_flight && rig->inventory().pending_combo == kRifle);
		CHECK(w.slot.next == weapon_action::kSwitchRank && w.slot.counter == 0);
		CHECK(rig->view().view_bias.position_q16[2] == 0);
		CHECK(!rig->lower_raise_active());
		const size_t applied = w.events.size();
		rig->tick(); // the untimed SWITCHRANK enters and completes
		CHECK(!w.switch_in_flight);
		CHECK(w.slot.current == weapon_action::kSwitchRank && w.slot.phase == weapon_phase::kDone);
		CHECK(!switch_recorded_since(w, applied));
		bool drew = false;
		for (int t = 0; t < 20; ++t) {
			rig->tick();
			drew = drew || w.slot.current == weapon_action::kSwitchTo ||
					w.slot.next == weapon_action::kSwitchTo;
		}
		CHECK(!drew);
		CHECK(!rig->lower_raise_active());
		CHECK(w.slot.current == weapon_action::kIdle);
		CHECK(rig->inventory().equipped_combo == kRifle);
		CHECK(rig->adm_byte() == rig->adm("WPN_RIFLE"));
	}
	{
		const auto rig = std::make_unique<Rig>(rifle_kit());
		LocalPlayerWeapon &w = rig->weapon();
		rig->tick();
		rig->press(2);
		CHECK(rig->tick_until_commit() >= 1);
		CHECK(rig->lower_raise_active()); // the pistol's raise
		CHECK(local_loadout_apply_accept(rig->world, rig->local().loadout, w, rig->inventory(),
				rig->local().inventory_valid, rifle_kit(), 0, false));
		CHECK(rig->inventory().equipped_combo == kPistol);
		CHECK(w.switch_in_flight && rig->inventory().pending_combo == kPistol);
		CHECK(w.slot.next != weapon_action::kSwitchRank);
		bool ran_rank = false;
		for (int t = 0; t < 60; ++t) {
			rig->tick();
			ran_rank = ran_rank || w.slot.current == weapon_action::kSwitchRank;
		}
		CHECK(!ran_rank);
		CHECK(!rig->lower_raise_active());
		CHECK(w.switch_in_flight); // the word stays stored
		CHECK(rig->inventory().equipped_combo == kPistol);
	}
}

// --- the mount request's view words (L7-s26 row 3) --------------------------------------

// The mount writes its view words at the request, the key press, and the commit that later
// stores the slot writes none. A cross-category press clears the promoted scope byte and
// resets the fov target at once, a refused one over a running draw included (nothing
// queues, nothing commits, the byte is gone all the same); a same-category press keeps the
// byte. A ForceScoped def raises the byte, seeds the rangefinder word with its +0xA0 and,
// optic, engages its scope: the interp binds the mounted slot (the step's reference its hip
// pose) and sets up one step to its sight pose, the held def's poses untouched until the
// commit. An AbsorbPitch def levels the look and takes its elevation span as the offset; a
// mount of any other def zeroes the offset.
// [orig: Player_MountWeaponSlot @0x4DFA40 - AbsorbPitch @0x4DFA7A..0x4DFA86, Pitch = 0
//  @0x4DFAA9, the offset @0x4DFAB3..0x4DFAB7, else 0 @0x4DFABE; ForceScoped @0x4DFB23..0x4DFB2F,
//  g_WeaponScopeActive = 1 @0x4DFB31, dword_B76808 = Def+0xA0 @0x4DFB3B..0x4DFB44,
//  g_WeaponScopeActive = 0 @0x4DFB60, g_CameraFovTargetQ16 @0x4DFB66, the bind and engage
//  @0x4DFC35..0x4DFC9A; WeaponAction_SwitchFrom @0x5433B0 / WeaponAction_SwitchRank
//  @0x543500 write none of them]
void test_the_mount_request_writes_the_view_words() {
	const auto fov = [](Rig &rig) -> int32_t & {
		return rig.world.weather.core.scalar_channels.camera_fov_target_fp;
	};
	{
		const auto rig = std::make_unique<Rig>(rifle_kit());
		rig->tick();
		rig->view().scope_settled = true;
		rig->press(2); // the key's camera reset keeps the byte; the mount request clears it
		CHECK(!rig->view().scope_settled);
		CHECK(fov(*rig) == (80 << 16));
		CHECK(rig->inventory().equipped_combo == kRifle); // not committed yet
		fov(*rig) = 20 << 16;
		CHECK(rig->tick_until_commit() >= 1);
		CHECK(rig->inventory().equipped_combo == kPistol);
		CHECK(fov(*rig) == (20 << 16)); // the commit writes none
		CHECK(!rig->view().scope_settled);
	}
	{
		const auto rig = std::make_unique<Rig>(rifle_kit());
		LocalPlayerWeapon &w = rig->weapon();
		rig->tick();
		w.slot.current = weapon_action::kSwitchTo;
		w.slot.next = weapon_action::kIdle;
		w.slot.phase = weapon_phase::kDone;
		w.slot.counter = 2;
		w.slot.switch_timer = -600;
		rig->view().scope_settled = true;
		rig->press(2); // refused over the draw: the word stays, nothing queues
		CHECK(w.slot.current == weapon_action::kSwitchTo && w.slot.next == weapon_action::kIdle);
		CHECK(!rig->view().scope_settled);
	}
	{
		const auto rig = std::make_unique<Rig>(rifle_kit());
		rig->tick();
		rig->view().scope_settled = true;
		rig->press(3); // the other fire mode: one category (the key's camera reset keeps it too)
		CHECK(rig->view().scope_settled);
	}
	{
		const auto rig = std::make_unique<Rig>(std::vector<WeaponKitEntry>{
				WeaponKitEntry{"WPN_RIFLE", -1, -1, -1}, WeaponKitEntry{"WPN_FORCED", -1, -1, -1}});
		LocalPlayerWeapon &w = rig->weapon();
		rig->tick();
		CHECK(!rig->view().scope_settled && !rig->view().weapon_pose_bound);
		w.aim_range_q16 = 0;
		const WeaponSwitchOutcome out = rig->press(6);
		CHECK(out.kind == WeaponSwitchOutcome::kMount && !out.same_category);
		CHECK(rig->view().scope_settled);
		CHECK(w.aim_range_q16 == 300);
		CHECK(rig->view().weapon_pose_bound && rig->view().scope_engaged &&
				!rig->view().scope_hipfire);
		CHECK(rig->view().weapon_pose_bound_hip.position_q16[0] == 256.0f);
		CHECK(rig->view().weapon_pose_interp.active);
		CHECK(rig->view().weapon_pose_interp.target.position_q16[0] == 1024.0f);
		CHECK(rig->view().weapon_hip_pose.position_q16[0] == 0.0f); // the rifle's, held
		CHECK(rig->tick_until_commit() >= 1);
		CHECK(rig->view().weapon_hip_pose.position_q16[0] == 256.0f);
	}
	{
		const auto rig = std::make_unique<Rig>(std::vector<WeaponKitEntry>{
				WeaponKitEntry{"WPN_RIFLE", -1, -1, -1}, WeaponKitEntry{"WPN_MORTAR", -1, -1, -1}});
		LocalPlayerWeapon &w = rig->weapon();
		rig->tick();
		CHECK(w.pitch_offset_bam == 0);
		rig->local().input.look_pitch = 1234;
		rig->press(8);
		CHECK(rig->local().input.look_pitch == 0);
		CHECK(w.pitch_offset_bam == 60 * 11930464); // 50 - (-10) degrees in truncated BAM
		CHECK(rig->tick_until_commit() >= 1);
		CHECK(w.pitch_offset_bam == 60 * 11930464);
		rig->settle();
		rig->press(3);
		CHECK(w.pitch_offset_bam == 0); // at the request, the mortar still held
		CHECK(rig->inventory().equipped_combo == 8 * weapon_combo::kRanksPerCategory);
	}
}

// --- the mount request's crouch (L7-s26 row 4) ------------------------------------------

// A ForceCrouch def's mount request raises the stance key's crouch, the reliable C2S 0x1D of
// action id 0xA9, through the seat-flag query: an OnlyScoped def's only while the promoted
// scope byte, as the request wrote it, is set (the mounted personal slot's owner is the local
// player). A cross-category mount clears the byte ahead of the query, so a mortar mounted from
// another category raises none; a same-category one under a raised byte does.
// [orig: Player_MountWeaponSlot @0x4DFA40 - g_WeaponScopeActive = 0 @0x4DFB60,
//  Entity_CheckWeaponSeatFlags(slot, 0x40000) @0x4DFBF2..0x4DFC02, NetPacket_WriteInt16C(0xA9)
//  @0x4DFC04..0x4DFC13, CNapiNetwork_QueueReliableMessage(0x1D, 1, 0) @0x4DFC1B..0x4DFC30;
//  Entity_CheckWeaponSeatFlags @0x540D00 (@0x540D19..0x540D35)]
void test_a_force_crouch_mount_requests_the_crouch() {
	const std::vector<uint16_t> crouch{0xA9};
	{
		const auto rig = std::make_unique<Rig>(std::vector<WeaponKitEntry>{
				WeaponKitEntry{"WPN_RIFLE", -1, -1, -1}, WeaponKitEntry{"WPN_CROUCHER", -1, -1, -1}});
		rig->tick();
		rig->world.out.stance_requests.clear();
		rig->press(5);
		CHECK(rig->world.out.stance_requests == crouch);
	}
	{
		const auto rig = std::make_unique<Rig>(std::vector<WeaponKitEntry>{
				WeaponKitEntry{"WPN_RIFLE", -1, -1, -1}, WeaponKitEntry{"WPN_PIT_A", -1, -1, -1}});
		rig->tick();
		rig->view().scope_settled = true;
		rig->world.out.stance_requests.clear();
		rig->press(7); // another category: the byte drops ahead of the query
		CHECK(!rig->view().scope_settled);
		CHECK(rig->world.out.stance_requests.empty());
	}
	{
		const auto rig = std::make_unique<Rig>(std::vector<WeaponKitEntry>{
				WeaponKitEntry{"WPN_PIT_A", -1, -1, -1}, WeaponKitEntry{"WPN_PIT_B", -1, -1, -1}});
		rig->tick();
		CHECK(rig->inventory().equipped_combo == 7 * weapon_combo::kRanksPerCategory);
		rig->view().scope_settled = true;
		rig->world.out.stance_requests.clear();
		rig->press(7); // the other rank, one category: the byte stands
		CHECK(rig->world.out.stance_requests == crouch);
		rig->view().scope_settled = false;
		rig->world.out.stance_requests.clear();
		rig->press(7);
		CHECK(rig->world.out.stance_requests.empty());
	}
}

// --- the SWITCHRANK completion's scope byte (L7-s27 row 0) ------------------------------

// A local SWITCHRANK completion clears the promoted scope byte, past its store, when the
// def it stored has neither Scoped nor Sighted; an optic def keeps it. The mount request
// keeps the byte (one category), so the clear is the completion's own: the switch back to
// the optic binds it with the byte clear and engages nothing. Over a byte raised on the iron
// sight the switch to the optic engages at its request and its completion keeps the byte.
// [orig: WeaponAction_SwitchRank @0x543500 - the store @0x543539, the EquippedSlot def's
//  Flags & 3 @0x543564..0x54357A, g_WeaponScopeActive = 0 @0x54357C; Player_MountWeaponSlot
//  @0x4DFA40 - the category compare @0x4DFB4C..0x4DFB5E, the bind and engage
//  @0x4DFC35..0x4DFC9A]
void test_a_switchrank_completion_clears_the_scope_byte_for_a_def_with_no_optic() {
	constexpr int32_t kOptic = 4 * weapon_combo::kRanksPerCategory;
	const auto rig = std::make_unique<Rig>(std::vector<WeaponKitEntry>{
			WeaponKitEntry{"WPN_OPTIC", -1, -1, -1}, WeaponKitEntry{"WPN_IRON", -1, -1, -1}});
	CHECK(rig->inventory().equipped_combo == kOptic);
	rig->tick();
	rig->view().scope_settled = true;
	const WeaponSwitchOutcome out = rig->press(4);
	CHECK(out.kind == WeaponSwitchOutcome::kMount && out.same_category && out.combo == kOptic + 1);
	CHECK(rig->view().scope_settled); // one category: the request keeps it
	CHECK(rig->tick_until_commit() >= 1);
	CHECK(rig->inventory().equipped_combo == kOptic + 1);
	CHECK(!rig->view().scope_settled); // the iron sight's SWITCHRANK cleared it
	rig->settle();
	rig->press(4); // back to the optic: bound, the byte clear, nothing engaged
	CHECK(rig->view().weapon_pose_bound && !rig->view().scope_engaged);
	CHECK(rig->tick_until_commit() >= 1);
	CHECK(rig->inventory().equipped_combo == kOptic);
	CHECK(!rig->view().scope_settled);
	rig->settle();
	rig->press(4);
	CHECK(rig->tick_until_commit() >= 1);
	rig->settle();
	CHECK(rig->inventory().equipped_combo == kOptic + 1);
	rig->view().scope_settled = true;
	rig->press(4); // the optic over a raised byte: engaged at the request
	CHECK(rig->view().weapon_pose_bound && rig->view().scope_engaged);
	CHECK(rig->tick_until_commit() >= 1);
	CHECK(rig->inventory().equipped_combo == kOptic);
	CHECK(rig->view().scope_settled); // an optic's SWITCHRANK keeps it
}

// --- a loadout apply's select writes no view word (L7-s27 row 1) ------------------------

// A loadout apply's select stores the slot and writes none of the mount's view words; the
// mount request that follows on the slot it stored compares that slot with itself, one
// category: no byte clear, no fov write, and the bind engages an optic over a raised byte.
// Here the apply's kit drops the held pistol, so the select falls through to the optic of
// another category: the byte and the fov target stand, and the optic engages.
// [orig: Player_SelectWeaponSlot @0x4DD680 (the stores @0x4DD704 / @0x4DD727, the latch
//  @0x4DD730, nothing else); WeaponLoadout_ApplyFromBuffer @0x565CD0 (the select @0x566285,
//  the mount @0x56628A..0x566297); Player_MountWeaponSlot @0x4DFA40 (the category compare
//  @0x4DFB4C..0x4DFB5E, the bind and engage @0x4DFC35..0x4DFC9A)]
void test_a_loadout_apply_writes_its_view_words_at_its_mount() {
	constexpr int32_t kOptic = 4 * weapon_combo::kRanksPerCategory;
	const auto rig = std::make_unique<Rig>(rifle_kit());
	LocalPlayerWeapon &w = rig->weapon();
	rig->tick();
	rig->press(2);
	CHECK(rig->tick_until_commit() >= 1);
	rig->settle();
	CHECK(rig->inventory().equipped_combo == kPistol);
	int32_t &fov = rig->world.weather.core.scalar_channels.camera_fov_target_fp;
	fov = 20 << 16;
	rig->view().scope_settled = true;
	CHECK(local_loadout_apply_accept(rig->world, rig->local().loadout, w, rig->inventory(),
			rig->local().inventory_valid,
			std::vector<WeaponKitEntry>{WeaponKitEntry{"WPN_OPTIC", -1, -1, -1}}, 0, false));
	CHECK(rig->inventory().equipped_combo == kOptic);
	CHECK(w.slot.next == weapon_action::kSwitchRank); // the mount's, on the slot itself
	CHECK(rig->view().scope_settled);
	CHECK(fov == (20 << 16));
	CHECK(rig->view().weapon_pose_bound && rig->view().scope_engaged);
	rig->settle(); // the SWITCHRANK on the slot itself runs out
	CHECK(rig->inventory().equipped_combo == kOptic);
	CHECK(rig->view().scope_settled); // the optic's SWITCHRANK keeps it
}

// --- Player_InitPlayer's view words (L7-s28 row 0, V-ROLL-AV D1, D-WPN-21) --------------

// Player_InitPlayer's select writes none of the mount's view words: the ForceScoped optic it
// selects (through the global scan, category 3 being empty) raises no scope byte and seeds no
// rangefinder word. Its walk's camera reset then binds the optic with the interp cleared,
// hipfire and nothing engaged; the walk queues no switch action (the mission start's body is
// hidden at its walk, D-WPN-21).
// [orig: Player_InitPlayer @0x4E15F0 - the memset @0x4E1602, the select @0x4E1995, the walk
//  @0x4E19A1; Player_SelectWeaponSlot @0x4DD680; Player_SwitchToWeaponByHandle @0x4E0170 -
//  Player_ResetCameraAndMovementState @0x4E01FC, the Flags test @0x4E0265;
//  Player_ResetCameraAndMovementState @0x4DE1F0 - the bind @0x4DE287..0x4DE2A7]
void test_the_spawn_select_writes_no_view_word_and_its_walk_resets_the_camera() {
	const auto rig = std::make_unique<Rig>(
			std::vector<WeaponKitEntry>{WeaponKitEntry{"WPN_FORCED", -1, -1, -1}});
	LocalPlayerWeapon &w = rig->weapon();
	PlayerViewState &v = rig->view();
	CHECK(rig->inventory().equipped_combo == 6 * weapon_combo::kRanksPerCategory);
	CHECK(rig->inventory().current_slot == weapon_combo::kDefaultSpawnCombo);
	CHECK(w.active && w.def_name == "WPN_FORCED");
	CHECK(!v.scope_settled);    // no ForceScoped byte
	CHECK(w.aim_range_q16 == 0); // no Def+0xA0 seed
	CHECK(v.weapon_pose_bound);  // the reset's optic bind
	CHECK(!v.scope_engaged && v.scope_hipfire && !v.weapon_pose_interp.active);
	CHECK(w.slot.current == weapon_action::kIdle && w.slot.next == weapon_action::kIdle);
}

// A re-run of Player_InitPlayer (a team assign's, a round init's) clears the words its memset
// and its two interp clears name, whatever the last weapon left: the scope byte, the
// rangefinder word, the AbsorbPitch offset, the six view-bias words, a running lower / raise
// and the PowerThrow windup. Its select of an AbsorbPitch def levels no pitch, and its walk's
// reset writes the fov target 80 and drops the binoculars request.
// [orig: Player_InitPlayer @0x4E15F0 - memset(&g_LocalPlayerEntity, 0, 0x3160) @0x4E1602, the
//  lower / raise interp @0x4E164D..0x4E16D8 and @0x4E1704..0x4E1722, the select @0x4E1995, the
//  walk @0x4E19A1; Player_ResetCameraAndMovementState @0x4DE1F0 - g_CameraFovTargetQ16
//  @0x4DE202, g_BinocularsToggle @0x4DE2B3]
void test_a_player_init_rerun_clears_the_view_words() {
	const auto rig = std::make_unique<Rig>(rifle_kit());
	LocalPlayerWeapon &w = rig->weapon();
	PlayerViewState &v = rig->view();
	rig->tick();
	v.scope_settled = true;
	w.aim_range_q16 = 777;
	w.pitch_offset_bam = 55;
	v.view_bias.position_q16[2] = -4096;
	v.view_bias.rotation_bam[0] = 12;
	player_view_start_weapon_lower(v);
	CHECK(rig->lower_raise_active());
	w.power_throw_start_tick = 9;
	v.binoculars_requested = true;
	rig->local().input.look_pitch = 1234;
	int32_t &fov = rig->world.weather.core.scalar_channels.camera_fov_target_fp;
	fov = 20 << 16;
	rig->local().loadout.spawn_kit() = {WeaponKitEntry{"WPN_MORTAR", -1, -1, -1}};
	local_loadout_rebuild(rig->world, rig->local().loadout, w, rig->inventory(),
			rig->local().inventory_valid, /*select_spawn_default=*/true);
	CHECK(rig->inventory().equipped_combo == 8 * weapon_combo::kRanksPerCategory);
	CHECK(!v.scope_settled && w.aim_range_q16 == 0 && w.pitch_offset_bam == 0);
	CHECK(v.view_bias.position_q16[2] == 0 && v.view_bias.rotation_bam[0] == 0);
	CHECK(!rig->lower_raise_active());
	CHECK(w.power_throw_start_tick == 0);
	CHECK(rig->local().input.look_pitch == 1234); // the select levels nothing
	CHECK(fov == (80 << 16));
	CHECK(!v.binoculars_requested);
	CHECK(!v.weapon_pose_bound); // no optic to bind
	CHECK(w.slot.next == weapon_action::kIdle);
}

// A re-run on a body in a Controller, Gunner or Driver seat walks nothing: the walk's seat gate
// skips its camera reset, so the fov target and the binoculars request stand and an optic
// stays unbound past the memset's interp clear; the memset's own clears still run.
// [orig: Player_SwitchToWeaponByHandle @0x4E0170 - the seat gate @0x4E017D..0x4E0192;
//  Player_InitPlayer @0x4E15F0 - the memset @0x4E1602, g_FpCameraInterp.entitySlotPtr = 0
//  @0x4E16F8]
void test_a_seated_player_init_rerun_resets_no_camera() {
	const auto rig = std::make_unique<Rig>(rifle_kit());
	LocalPlayerWeapon &w = rig->weapon();
	PlayerViewState &v = rig->view();
	rig->tick();
	Entity *body = rig->world.registry.get(rig->world.cached.local_player);
	CHECK(body != nullptr);
	if (body == nullptr) return;
	body->mounted = true;
	body->mount_type = SeatType::Driver;
	v.scope_settled = true;
	v.binoculars_requested = true;
	w.power_throw_start_tick = 9;
	int32_t &fov = rig->world.weather.core.scalar_channels.camera_fov_target_fp;
	fov = 20 << 16;
	rig->local().loadout.spawn_kit() = {WeaponKitEntry{"WPN_OPTIC", -1, -1, -1}};
	local_loadout_rebuild(rig->world, rig->local().loadout, w, rig->inventory(),
			rig->local().inventory_valid, /*select_spawn_default=*/true);
	CHECK(rig->inventory().equipped_combo == 4 * weapon_combo::kRanksPerCategory);
	CHECK(!v.scope_settled && w.power_throw_start_tick == 0);
	CHECK(fov == (20 << 16));
	CHECK(v.binoculars_requested);
	CHECK(!v.weapon_pose_bound);
}

// The spawn kit's rebuild past the mission start's environment snap and script init (a profile
// page the shell hands the sim after its load) keeps the fov target they left, as retail's
// Player_InitPlayer precedes both; the rest is Player_InitPlayer's leg: the optic it selects is
// bound and the byte cleared.
// [orig: Game_StartMission @0x524360 - Player_InitPlayer @0x525BBC,
//  Environment_SnapStateToTargets @0x525CAE, WacScript_InitAndLoad @0x525CB3]
void test_the_rebuild_after_the_start_keeps_the_fov() {
	const auto rig = std::make_unique<Rig>(rifle_kit());
	PlayerViewState &v = rig->view();
	int32_t &fov = rig->world.weather.core.scalar_channels.camera_fov_target_fp;
	fov = 33 << 16;
	v.scope_settled = true;
	rig->local().loadout.spawn_kit() = {WeaponKitEntry{"WPN_OPTIC", -1, -1, -1}};
	local_loadout_rebuild_after_start(rig->world, rig->local().loadout, rig->weapon(),
			rig->inventory(), rig->local().inventory_valid);
	CHECK(rig->inventory().equipped_combo == 4 * weapon_combo::kRanksPerCategory);
	CHECK(fov == (33 << 16));
	CHECK(v.weapon_pose_bound && !v.scope_settled);
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
	test_a_usegun_attach_over_a_draw_queues_nothing();
	test_a_usegun_detach_mounts_the_personal_slot_inside_the_detach();
	test_a_gun_to_gun_move_inside_the_lower_commits_at_the_switchfrom();
	test_the_last_interp_step_publishes_the_view_bias();
	test_the_mount_request_clears_the_view_bias_and_the_commit_does_not();
	test_a_claimants_detach_holds_its_equipped_slot_ready();
	test_a_loadout_apply_mounts_its_selected_slot();
	test_the_mount_request_writes_the_view_words();
	test_a_force_crouch_mount_requests_the_crouch();
	test_a_switchrank_completion_clears_the_scope_byte_for_a_def_with_no_optic();
	test_a_loadout_apply_writes_its_view_words_at_its_mount();
	test_the_spawn_select_writes_no_view_word_and_its_walk_resets_the_camera();
	test_a_player_init_rerun_clears_the_view_words();
	test_a_seated_player_init_rerun_resets_no_camera();
	test_the_rebuild_after_the_start_keeps_the_fov();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("weapon_lower_raise_test OK\n");
	return 0;
}
