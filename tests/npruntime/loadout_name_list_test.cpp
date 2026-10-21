// D-NET-143 (L4-s8 FOUND 1): a player body's equipped weapon at its spawn is the first name of
// the process's loadout name list (g_LoadoutWeaponNameList), not a fixed WPN_M4AUTO. The list is
// the image's initializer (WPN_M4AUTO first) until an armory ACCEPT copies the accepted class's
// buffer over it (every leg) or a 0x5A apply rebuilds it from the list's rows (a listen host's
// own client's included, at its next receive). So after a listen host's own ACCEPT or its own
// list's apply, every body the host spawns carries the host's first weapon in its records' equipped byte until its owner's
// uplink; a dedicated host never writes the list and keeps WPN_M4AUTO. The ACCEPT leaves the
// resident kit (the spawn buffer every other 0x2F serializes) to the host's list.
// [orig: PlayerClass_InitEntity @0x4B1100..0x4B1116; g_LoadoutWeaponNameList @0x8333F8;
//  WeaponLoadout_ApplyFromBuffer @0x565D46; NapiNPClientMsg_HandleWeaponLoadoutSync
//  @0x42920C..0x4293BF, the spawn buffer copy off the authority @0x4293D1..0x4293E9;
//  NetPacket_SendLoadoutSubmit's restrictionData g_SpawnLoadoutBuffer @0x52582F / @0x525C24 /
//  @0x431AA3]
// The world and the process words are the test's own.

#include <runtime/inmatch/client_runtime.h> // ClientProcessState
#include <runtime/inmatch/loadout_submit.h>
#include <runtime/inmatch/loopback_channel.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_loadout_grant.h>
#include <runtime/inmatch/server_spawn.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <formats/mission/bms.h>
#include <formats/playersav/player_sav.h>
#include <runtime/world/local_player.h>
#include <runtime/world/player_loadout.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/weapon_inventory.h>
#include <runtime/world/world.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "host_test_setup.h"

namespace {

using namespace opennova;
namespace inmatch = opennova::inmatch;
namespace w = opennova::world;

int failures = 0;
#define CHECK(c)                                                                               \
	do {                                                                                       \
		if (!(c)) {                                                                            \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                           \
			++failures;                                                                        \
		}                                                                                      \
	} while (0)

constexpr uint8_t kM4 = 1;
constexpr uint8_t kAk = 2;

struct Rig {
	std::unique_ptr<w::World> wp = std::make_unique<w::World>();
	w::World &world = *wp;
	std::unique_ptr<inmatch::NapiNPServerCtx> ctx = std::make_unique<inmatch::NapiNPServerCtx>();
	std::unique_ptr<inmatch::ClientProcessState> process =
			std::make_unique<inmatch::ClientProcessState>();
	replication::LoopbackChannel loop;

	explicit Rig(inmatch::ConnectionMode mode) {
		world.registry.configure_pool(0, 8);
		world.rules.mp_session = true;
		world.tables.player.has_item_def = true;
		world.tables.player.item_type_index = 1;
		world.tables.player.item_type = 3;
		for (const char *name : {"WPN_KNIFE", "WPN_M4AUTO", "WPN_AK47AUTO"}) {
			w::WeaponTableEntry def;
			def.name = name;
			def.valid = true;
			world.tables.weapons.entries.push_back(def);
		}
		// The role's binding of the process's list (HostRole::apply_rule_words).
		world.loadout_weapon_names = &process->loadout_weapon_names;
		inmatch::GameConfig settings;
		settings.max_players = 4;
		settings.game_type = 0x10000u;
		inmatch::test::bring_up_host(*ctx, mode, inmatch::SocketMode::Socketless, 0,
				mode == inmatch::ConnectionMode::HostClient ? &loop : nullptr, settings);
		ctx->world = &world;
		ctx->is_in_session = 1;
		inmatch::Server_InitNewRoundState(*ctx);
	}
	inmatch::NapiNPConnection &add_joiner(uint32_t id) {
		inmatch::NapiNPConnection joiner;
		joiner.type = 1;
		joiner.connection_id = id;
		joiner.self_id_seen = true;
		joiner.phase = inmatch::ConnectionPhase::Joined;
		ctx->np_protocol.connection_list.push_back(joiner);
		CHECK(inmatch::Server_ProcessPendingPlayerSpawns(*ctx, world) >= 1);
		return ctx->np_protocol.connection_list.back();
	}
	inmatch::NapiNPConnection *own() {
		for (inmatch::NapiNPConnection &c : ctx->np_protocol.connection_list)
			if (c.type == inmatch::NapiNPConnection::kTypeClientSide) return &c;
		return nullptr;
	}
	uint8_t equipped(const inmatch::NapiNPConnection &c) const {
		const w::Entity *e = world.registry.get(c.link.owned_entity);
		return e != nullptr ? e->equipped_adm_index : uint8_t{0xEE};
	}
};

// A dedicated host writes the list never: its row body and every joiner's body take the
// initializer's first name, WPN_M4AUTO.
void test_a_dedicated_host_keeps_the_initializers_first_weapon() {
	Rig r(inmatch::ConnectionMode::HostOnly);
	CHECK(!r.process->loadout_weapon_names.set);
	CHECK(w::player_spawn_equipped_default(r.world) == kM4);
	const w::EntityHandle row = inmatch::Server_SpawnHostSlotBody(*r.ctx, r.world);
	const w::Entity *body = r.world.registry.get(row);
	CHECK(body != nullptr && body->equipped_adm_index == kM4);
	const inmatch::NapiNPConnection &joiner = r.add_joiner(inmatch::kFirstJoinerDcb);
	CHECK(r.equipped(joiner) == kM4);
}

// A listen host's own client applies the list its slot is sent at its next receive, the fold
// between two server ticks (retail: the next frame's client receive, ahead of its server tick),
// and rebuilds the process's list from the list's rows. So a joiner the pump adds in the list's
// own tick takes the previous list's first weapon in its records' equipped byte, and every body
// spawned after the fold the first granted weapon.
// [orig: Server_TickUpdate @0x51D7E0 - the receive pump @0x51D895 (the 0x2F accept's list send)
//  ahead of CNapiServer_ProcessPendingPlayerSpawns @0x51DBFD; Game_ProcessMainFrame @0x5263F0 -
//  Client_ProcessNetworkFrame @0x526692 -> @0x42C228 ahead of Server_TickUpdate @0x5266B6;
//  NapiNPClientMsg_HandleWeaponLoadoutSync @0x4290E0 - @0x42920C..0x4293B9]
void test_a_listen_hosts_own_list_names_every_body_after_its_fold() {
	Rig r(inmatch::ConnectionMode::HostClient);
	auto own_client = std::make_unique<inmatch::ClientRuntime>(r.loop);
	CHECK(inmatch::Server_ProcessPendingPlayerSpawns(*r.ctx, r.world) >= 1);
	CHECK(r.equipped(r.add_joiner(inmatch::kFirstJoinerDcb)) == kM4);
	inmatch::NapiNPConnection *own = r.own();
	CHECK(own != nullptr);
	if (own == nullptr) return;
	WeaponLoadout list;
	list.avatar_class = 7;
	list.slots.push_back(WeaponLoadoutSlot{kAk, 3, 0xFF, 0});
	list.slots.push_back(WeaponLoadoutSlot{kM4, 2, 0xFF, 0});
	own->reply.last_loadout_reply = encode_weapon_loadout(list);
	// The tick's receive: the list send to the own slot, its legs then the 0x5A on the own
	// connection.
	r.loop.host_send(s2c::WEAPON_LOADOUT, inmatch::current_weapon_slot_list(*own, &r.world));
	// The same tick's spawn pump: the list is the previous one.
	CHECK(!r.process->loadout_weapon_names.set);
	CHECK(r.equipped(r.add_joiner(inmatch::kFirstJoinerDcb + 1)) == kM4);
	// The own client's fold, ahead of the next tick.
	(void)own_client->Client_ProcessNetworkFrame(0);
	own_client->apply_received_effects(r.world);
	const w::LoadoutWeaponNameList &names = r.process->loadout_weapon_names;
	CHECK(names.set && names.entries.size() == 2);
	if (names.entries.size() == 2)
		CHECK(names.entries[0].name == "WPN_AK47AUTO" && names.entries[1].name == "WPN_M4AUTO");
	CHECK(r.equipped(r.add_joiner(inmatch::kFirstJoinerDcb + 2)) == kAk);
	// Bodies spawned before the fold keep their own.
	CHECK(r.equipped(r.ctx->np_protocol.connection_list[1]) == kM4);
	CHECK(r.equipped(r.ctx->np_protocol.connection_list[2]) == kM4);
	// A list of no rows empties it: the lookup's miss, 0xFF.
	own = r.own();
	if (own == nullptr) return;
	WeaponLoadout empty;
	empty.avatar_class = 7;
	own->reply.last_loadout_reply = encode_weapon_loadout(empty);
	r.loop.host_send(s2c::WEAPON_LOADOUT, inmatch::current_weapon_slot_list(*own, &r.world));
	(void)own_client->Client_ProcessNetworkFrame(0);
	own_client->apply_received_effects(r.world);
	CHECK(r.process->loadout_weapon_names.set && r.process->loadout_weapon_names.entries.empty());
	CHECK(w::player_spawn_equipped_default(r.world) == 0xFF);
}

// The ACCEPT copies the accepted kit into the list on either leg and leaves the resident kit:
// the next 0x2F that serializes the spawn buffer (the mission start's pair, the team assign's
// resubmit) still carries the pre-ACCEPT rows until the host's list reaches it.
void test_the_accept_writes_the_list_not_the_resident_kit() {
	for (const bool authority : {true, false}) {
		Rig r(inmatch::ConnectionMode::HostOnly);
		auto local = std::make_unique<w::LocalPlayer>(r.world);
		local->loadout.spawn_kit() = {w::WeaponKitEntry{"WPN_M4AUTO", 3, -1, -1}};
		const std::vector<w::WeaponKitEntry> accepted = {w::WeaponKitEntry{"WPN_AK47AUTO", 4, -1, -1},
				w::WeaponKitEntry{"WPN_KNIFE", -1, -1, -1}};
		(void)inmatch::armory_accept_in_session(r.world, *local, accepted, 6, authority,
				bms::MissionType{});
		const w::LoadoutWeaponNameList &names = r.process->loadout_weapon_names;
		CHECK(names.set && names.entries.size() == 2);
		if (!names.entries.empty()) CHECK(names.entries[0].name == "WPN_AK47AUTO");
		CHECK(w::player_spawn_equipped_default(r.world) == kAk);
		CHECK(local->loadout.spawn_kit().size() == 1 &&
				local->loadout.spawn_kit()[0].name == "WPN_M4AUTO");
		inmatch::JoinerConnection::LoadoutKit kit;
		inmatch::build_joiner_loadout_kit(r.world, playersav::Record{}, 1, local->loadout, -1,
				nullptr, kit);
		CHECK(kit.rows.size() == 1);
		if (!kit.rows.empty()) CHECK(kit.rows[0].adm_index == kM4 && kit.rows[0].ammo_primary == 3);
	}
}

// An out-of-session ACCEPT (a single player's armory) copies its whole selection into the
// process's loadout name list too, ahead of the session test, so a process that ACCEPTs in single
// player and then hosts spawns every body with that selection's first weapon; the 0x5A grant's
// apply (its rebuild is the 0x5A's own) writes nothing here.
// [orig: WeaponLoadout_ApplyFromBuffer @0x565CD0 (@0x565D27..0x565D4B ahead of the session test
//  @0x565D3A, its jz @0x565D4D)]
void test_an_out_of_session_accept_writes_the_list() {
	Rig r(inmatch::ConnectionMode::HostOnly);
	r.world.rules.mp_session = false;
	auto local = std::make_unique<w::LocalPlayer>(r.world);
	const std::vector<w::WeaponKitEntry> selection = {w::WeaponKitEntry{"WPN_AK47AUTO", 4, -1, -1},
			w::WeaponKitEntry{"WPN_KNIFE", -1, -1, -1}};
	CHECK(w::local_loadout_apply_accept(r.world, local->loadout, local->weapon, local->inventory,
			local->inventory_valid, selection, 6, /*validate_banned=*/true));
	const w::LoadoutWeaponNameList &names = r.process->loadout_weapon_names;
	CHECK(names.set && names.entries.size() == 2);
	if (!names.entries.empty()) CHECK(names.entries[0].name == "WPN_AK47AUTO");
	CHECK(w::player_spawn_equipped_default(r.world) == kAk);
	// The grant's apply leaves the list.
	Rig g(inmatch::ConnectionMode::HostOnly);
	auto joiner_local = std::make_unique<w::LocalPlayer>(g.world);
	CHECK(w::local_loadout_apply_accept(g.world, joiner_local->loadout, joiner_local->weapon,
			joiner_local->inventory, joiner_local->inventory_valid, selection, 6,
			/*validate_banned=*/false));
	CHECK(!g.process->loadout_weapon_names.set);
}

// The single player's fill of the spawn buffer at its mission start and restart: the current
// profile's single-player page when the mission's own kit chunk promoted none and the page's
// first name is not empty; else the static page, which the fill writes (no profile, an empty
// page). Each start's fill writes the buffer over what the process left. Never in a session.
// [orig: Game_ApplySessionSettingsToGlobals @0x551500 - @0x5519A6..0x5519E4 (the static page
//  @0x5519DA); Game_StartMission @0x524683..0x5246C8 (@0x5246BE); Mission_LoadBMSFile
//  @0x40F6FF..0x40F77C]
void test_the_single_players_page_fills_the_spawn_buffer() {
	playersav::Record profile;
	profile.single_player.entries = {playersav::KitEntry{"WPN_AK47AUTO", 4, -1, -1},
			playersav::KitEntry{"WPN_KNIFE", -1, -1, -1}};
	{
		Rig r(inmatch::ConnectionMode::HostOnly);
		r.world.rules.mp_session = false;
		w::LocalPlayerLoadout loadout;
		CHECK(inmatch::seed_single_player_kit_from_profile(r.world, &profile, loadout));
		CHECK(loadout.spawn_kit().size() == 2);
		if (!loadout.spawn_kit().empty())
			CHECK(loadout.spawn_kit()[0].name == "WPN_AK47AUTO" &&
					loadout.spawn_kit()[0].ammo_primary == 4);
		// The next start's fill writes the buffer again.
		playersav::Record other = profile;
		other.single_player.entries[0].name = "WPN_M4AUTO";
		CHECK(inmatch::seed_single_player_kit_from_profile(r.world, &other, loadout));
		CHECK(!loadout.spawn_kit().empty() && loadout.spawn_kit()[0].name == "WPN_M4AUTO" &&
				loadout.spawn_kit()[0].ammo_primary == 4);
	}
	{
		// The mission's own kit promoted: it stands.
		Rig r(inmatch::ConnectionMode::HostOnly);
		r.world.rules.mp_session = false;
		w::LocalPlayerLoadout loadout;
		w::local_loadout_set_spawn_kit(r.world, loadout, {w::WeaponKitEntry{"WPN_M4AUTO", 2, -1, -1}},
				/*filter_by_availability=*/false);
		CHECK(loadout.mission_kit_promoted);
		CHECK(!inmatch::seed_single_player_kit_from_profile(r.world, &profile, loadout));
		CHECK(loadout.spawn_kit().size() == 1 && loadout.spawn_kit()[0].name == "WPN_M4AUTO" &&
				loadout.spawn_kit()[0].ammo_primary == 2);
	}
	{
		// An empty page, or no profile: the fill writes the static page, M4AUTO first.
		Rig r(inmatch::ConnectionMode::HostOnly);
		r.world.rules.mp_session = false;
		playersav::Record empty;
		empty.single_player.entries.clear();
		w::LocalPlayerLoadout loadout;
		CHECK(inmatch::seed_single_player_kit_from_profile(r.world, &empty, loadout));
		const std::vector<w::WeaponKitEntry> page = w::weapon_kit_default(r.world.rules.title);
		CHECK(loadout.spawn_kit().size() == page.size() && !page.empty() &&
				loadout.spawn_kit()[0].name == "WPN_M4AUTO");
		w::LocalPlayerLoadout none;
		CHECK(inmatch::seed_single_player_kit_from_profile(r.world, nullptr, none));
		CHECK(none.spawn_kit().size() == page.size());
	}
	{
		// In a session the side's page is the copy (seed_session_kit_from_profile): the fill
		// writes nothing.
		Rig r(inmatch::ConnectionMode::HostOnly);
		w::LocalPlayerLoadout loadout;
		CHECK(!inmatch::seed_single_player_kit_from_profile(r.world, &profile, loadout));
		CHECK(loadout.spawn_kit().empty());
	}
}

} // namespace

int main() {
	test_a_dedicated_host_keeps_the_initializers_first_weapon();
	test_a_listen_hosts_own_list_names_every_body_after_its_fold();
	test_the_accept_writes_the_list_not_the_resident_kit();
	test_the_single_players_page_fills_the_spawn_buffer();
	test_an_out_of_session_accept_writes_the_list();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("PASS npruntime_loadout_name_list\n");
	return 0;
}
