// A switch's commit mounts the EquippedSlot's def (LI-s21 FOUND, V-ROLL-AL's detach edge): the
// commit stores the slot whose own def every reader takes from then on, so the walk's FSM, the
// PowerThrow branch and the charge bar read the committed slot's def from the commit on, the
// ticks after it in the same frame included, with no presenter install between. The switch's
// record says so (installed), leaving the presenter the viewmodel alone.
// [orig: WeaponAction_SwitchFrom @0x5433B0 - the store @0x543475, WeaponSlot_TryQueueSwitchTo
//  @0x5434A3; WeaponAction_ProcessAllEntities @0x542690 - the EquippedSlot read @0x5426AD;
//  WeaponAction_ProcessFrame @0x540E60 - weaponSlot->Def @0x540E71;
//  HUD_DrawPowerThrowChargeBar @0x599830 - the def's sign bit @0x59986C]
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <formats/def/def.h>
#include <runtime/world/local_player.h>
#include <runtime/world/player_loadout.h>
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

// A rifle (category 3, the spawn select's) and a PowerThrow throwable (category 6), each
// with its own draw sound and timed switch rows (an untimed SWITCHFROM falls back to its
// previous action with no commit), and an emplaced gun's def (category 9).
constexpr const char *kWeaponDef =
		"weapon \"WPN_RIFLE\"\r\n"
		"\tcategory 3\r\n"
		"\trank 0\r\n"
		"\tclipsize 30\r\n"
		"\tstartrounds 90\r\n"
		"\tammoclass CLASS_556MM 1\r\n"
		"\taction \"switchto\"\r\n"
		"\t\tdelaystart 2\r\n"
		"\t\tdelayend 2\r\n"
		"\t\tsoundset SND_RIFLE_DRAW\r\n"
		"\tend\r\n"
		"\taction \"switchfrom\"\r\n"
		"\t\tdelaystart 2\r\n"
		"\t\tdelayend 2\r\n"
		"\tend\r\n"
		"end\r\n"
		"weapon \"WPN_THROW\"\r\n"
		"\tcategory 6\r\n"
		"\trank 0\r\n"
		"\tclipsize 1\r\n"
		"\tstartrounds 3\r\n"
		"\tammoclass CLASS_FRAG 1\r\n"
		"\tflags PowerThrow\r\n"
		"\taction \"switchto\"\r\n"
		"\t\tdelaystart 2\r\n"
		"\t\tdelayend 2\r\n"
		"\t\tsoundset SND_THROW_DRAW\r\n"
		"\tend\r\n"
		"\taction \"switchfrom\"\r\n"
		"\t\tdelaystart 2\r\n"
		"\t\tdelayend 2\r\n"
		"\tend\r\n"
		"end\r\n"
		"weapon \"WPN_GUN\"\r\n"
		"\tcategory 9\r\n"
		"\trank 0\r\n"
		"\tclipsize 100\r\n"
		"\tstartrounds 100\r\n"
		"\tammoclass CLASS_762MM 1\r\n"
		"\taction \"switchfrom\"\r\n"
		"\t\tdelaystart 2\r\n"
		"\t\tdelayend 2\r\n"
		"\tend\r\n"
		"end\r\n"
		// A ForceScoped def of its own category (4), whose mount writes the scope words.
		"weapon \"WPN_FORCED\"\r\n"
		"\tcategory 4\r\n"
		"\trank 0\r\n"
		"\tclipsize 5\r\n"
		"\tstartrounds 20\r\n"
		"\tammoclass CLASS_762MM 1\r\n"
		"\tflags ForceScoped\r\n"
		"end\r\n";

// One local player on the parsed table, its kit rebuilt with the spawn select, the parse
// bound as the world's (MissionTables::weapon_defs) unless `bind_defs` is false.
struct Rig {
	std::unique_ptr<World> owned_world = std::make_unique<World>();
	World &world = *owned_world;
	std::unique_ptr<LocalPlayer> owned_local;
	DefWeaponsFile defs{};
	bool parsed = false;

	Rig(std::vector<WeaponKitEntry> kit, bool bind_defs = true) {
		const std::string text(kWeaponDef);
		parsed = def_parse_weapons_memory(reinterpret_cast<const uint8_t *>(text.data()),
				text.size(), &defs) == 0;
		world.tables.weapons = build_weapon_table(defs);
		if (bind_defs) world.tables.weapon_defs = &defs;
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
	uint8_t adm(const char *name) const {
		return static_cast<uint8_t>(world.tables.weapons.index_of(name));
	}
	// One tick of the local player's slot.
	void pump() {
		++world.logic_tick;
		local().pump_local_weapon();
	}
	// An emplaced gun whose own slot a borrow holds (an EWeap item's embedded slot).
	EntityHandle spawn_gun() {
		Entity gun;
		gun.kind = EntityKind::Item;
		gun.has_item_def = true;
		gun.item_attrib = kItemAttribEweap;
		gun.primary_weapon_slot_adm = adm("WPN_GUN");
		gun.primary_weapon_slot.clip = 100;
		return world.registry.spawn(1, gun);
	}
};

// The spawn select's commit mounts the selected def itself and draws nothing (a select
// plays no switch action, D-WPN-21).
void test_the_select_mounts_its_def_without_a_draw() {
	const auto rig = std::make_unique<Rig>(std::vector<WeaponKitEntry>{
			WeaponKitEntry{"WPN_RIFLE", -1, -1, -1}, WeaponKitEntry{"WPN_THROW", -1, -1, -1}});
	CHECK(rig->parsed);
	LocalPlayerWeapon &w = rig->weapon();
	CHECK(rig->local().inventory_valid);
	CHECK(w.active && w.def_name == "WPN_RIFLE");
	CHECK(w.slot.next != weapon_action::kSwitchTo && !w.start_in_switchto);
	CHECK(w.slot.clip > 0);
	CHECK(!w.events.empty() && w.events.back().switch_to_weapon == "WPN_RIFLE" &&
			w.events.back().installed);
}

// A frame of two ticks with a category switch's commit on the first: the commit mounts the
// new def and its fresh slot with the draw queued, and the second tick pumps that slot with
// that def (its SWITCHTO starts on the new def's own row).
void test_the_tick_after_the_commit_runs_the_new_def() {
	const auto rig = std::make_unique<Rig>(std::vector<WeaponKitEntry>{
			WeaponKitEntry{"WPN_RIFLE", -1, -1, -1}, WeaponKitEntry{"WPN_THROW", -1, -1, -1}});
	LocalPlayerWeapon &w = rig->weapon();
	WeaponInventory &inventory = rig->local().inventory;
	rig->pump();
	const WeaponSwitchOutcome out = weapon_switch_to_handle(rig->world.tables.weapons, inventory,
			6 * weapon_combo::kRanksPerCategory,
			local_weapon_switch_gates(rig->world, w, &inventory));
	CHECK(out.kind == WeaponSwitchOutcome::kMount && !out.same_category);
	handle_weapon_switch_outcome(rig->world, w, &inventory, out, rig->local().view);
	CHECK(w.switch_in_flight);
	const int32_t throw_combo = 6 * weapon_combo::kRanksPerCategory;
	bool committed = false;
	for (int tick = 0; tick < 200 && !committed; ++tick) {
		w.events.clear();
		rig->pump();
		committed = inventory.equipped_combo == throw_combo;
	}
	CHECK(committed);
	// The commit's tick ends with the new def mounted, no presenter between.
	CHECK(w.def_name == "WPN_THROW");
	CHECK((w.def.flags & weapon_flag::kPowerThrow) != 0);
	CHECK(w.slot.next == weapon_action::kSwitchTo);
	bool switch_record = false;
	for (const WeaponPresentationEvent &event : w.events)
		if (event.switch_to_weapon == "WPN_THROW")
			switch_record = event.installed && !event.preserve_slot_state;
	CHECK(switch_record);
	// The frame's second tick: the slot's SWITCHTO starts on the throwable's row.
	w.events.clear();
	rig->pump();
	CHECK(w.slot.current == weapon_action::kSwitchTo);
	bool drew_the_throwable = false;
	for (const WeaponPresentationEvent &event : w.events)
		if (event.action_started == weapon_action::kSwitchTo)
			drew_the_throwable = event.action_soundset == "SND_THROW_DRAW";
	CHECK(drew_the_throwable);
}

// A route leg's EquippedSlot store mounts the gun's def at once, and a detach's switch back
// mounts the personal def with its store: a PowerThrow windup carried across the borrow
// draws no bar over the gun's slot and draws again from the detach's commit on, in the
// commit's own tick (V-ROLL-AL's edge).
void test_the_borrow_and_its_detach_mount_their_defs() {
	const auto rig = std::make_unique<Rig>(std::vector<WeaponKitEntry>{
			WeaponKitEntry{"WPN_THROW", -1, -1, -1}});
	LocalPlayerWeapon &w = rig->weapon();
	WeaponInventory &inventory = rig->local().inventory;
	World &world = rig->world;
	// The throwable is the only slot: the select takes it.
	CHECK(w.def_name == "WPN_THROW");
	rig->pump();
	CHECK(local_weapon_power_throw_press(world, w));
	const uint32_t started = w.power_throw_start_tick;
	CHECK(local_player_weapon_view(world, w, inventory).windup_active);

	Entity *gun = world.registry.get(rig->spawn_gun());
	CHECK(gun != nullptr);
	if (gun == nullptr) return;
	w.events.clear();
	local_usegun_store_equipped_slot(world, w, *gun);
	CHECK(w.usegun_slot_active && w.def_name == "WPN_GUN");
	CHECK((w.def.flags & weapon_flag::kPowerThrow) == 0);
	CHECK(!w.events.empty() && w.events.back().switch_to_weapon == "WPN_GUN" &&
			w.events.back().installed && w.events.back().preserve_slot_state);
	CHECK(w.power_throw_start_tick == started); // the store writes no windup
	CHECK(!local_player_weapon_view(world, w, inventory).windup_active);

	// The body holds no seat on the gun, so the walk's transition stages the switch back
	// (a detach) on the gun's slot; its SWITCHFROM's completion commits the personal slot,
	// g_PendingWeaponSlot as the spawn select left it, with its def, inside that tick.
	bool committed = false;
	for (int tick = 0; tick < 200 && !committed; ++tick) {
		w.events.clear();
		rig->pump();
		committed = !w.usegun_slot_active;
		if (!committed) CHECK(!local_player_weapon_view(world, w, inventory).windup_active);
	}
	CHECK(committed);
	CHECK(w.def_name == "WPN_THROW");
	bool switch_back = false;
	for (const WeaponPresentationEvent &event : w.events)
		if (event.switch_to_weapon == "WPN_THROW")
			switch_back = event.installed && event.preserve_slot_state;
	CHECK(switch_back);
	// The bar draws again from the commit's tick on, the windup's ticks held throughout.
	const LocalPlayerWeaponView back = local_player_weapon_view(world, w, inventory);
	CHECK(back.windup_active);
	CHECK(back.windup_held_ticks == static_cast<int32_t>(world.logic_tick - started));
}

// With no weapon.def parse bound the commit mounts nothing and its record leaves the mount
// to the presenter.
void test_no_parse_leaves_the_mount_to_the_presenter() {
	const auto rig = std::make_unique<Rig>(std::vector<WeaponKitEntry>{
			WeaponKitEntry{"WPN_RIFLE", -1, -1, -1}}, /*bind_defs=*/false);
	LocalPlayerWeapon &w = rig->weapon();
	CHECK(!w.active && w.def_name.empty());
	CHECK(!w.events.empty() && w.events.back().switch_to_weapon == "WPN_RIFLE" &&
			!w.events.back().installed);
	CHECK(!local_weapon_install_by_name(rig->world, w, "WPN_RIFLE", false, false,
			&rig->local().inventory, rig->local().view));
}

// The spawn's sync mounts with none of the mount's view words (L7-s28 FOUND, L7-s29): the
// shell's sync of the EquippedSlot the spawn settled installs the def, a fresh slot, and leaves
// the ForceScoped settle and range seed and the fov target as they stood, as retail's spawn
// writes none; the by-name mount of the same def writes them.
// [orig: Player_InitPlayer @0x4E15F0 - Player_SelectWeaponSlot (the call @0x4E1995), the walk
//  (the call @0x4E19A1, its Flags test @0x4E0265 on a body born hidden); Player_MountWeaponSlot
//  @0x4DFA40 - ForceScoped @0x4DFB23..0x4DFB44, g_CameraFovTargetQ16 @0x4DFB66]
void test_the_spawn_sync_mounts_without_view_words() {
	for (const bool spawn : {true, false}) {
		const auto rig = std::make_unique<Rig>(std::vector<WeaponKitEntry>{
				WeaponKitEntry{"WPN_RIFLE", -1, -1, -1}});
		LocalPlayerWeapon &w = rig->weapon();
		PlayerViewState &view = rig->local().view;
		int32_t &fov = rig->world.weather.core.scalar_channels.camera_fov_target_fp;
		fov = 33 << 16;
		w.aim_range_q16 = 7;
		CHECK(!view.scope_settled);
		const bool installed = spawn
				? local_weapon_install_spawn_by_name(rig->world, w, "WPN_FORCED",
						&rig->local().inventory, view)
				: local_weapon_install_by_name(rig->world, w, "WPN_FORCED", false, false,
						&rig->local().inventory, view);
		CHECK(installed);
		CHECK(w.active && w.def_name == "WPN_FORCED");
		if (spawn) {
			CHECK(!view.scope_settled);
			CHECK(w.aim_range_q16 == 7);
		} else {
			CHECK(view.scope_settled);
			CHECK(w.aim_range_q16 != 7);
		}
		CHECK(fov == (33 << 16)); // ForceScoped keeps the target either way
	}
}

} // namespace

int main() {
	test_the_select_mounts_its_def_without_a_draw();
	test_the_tick_after_the_commit_runs_the_new_def();
	test_the_borrow_and_its_detach_mount_their_defs();
	test_no_parse_leaves_the_mount_to_the_presenter();
	test_the_spawn_sync_mounts_without_view_words();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("commit_mount_test OK\n");
	return 0;
}
