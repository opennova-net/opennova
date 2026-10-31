// D-NET-695 / D-NET-479: on the authority the local player's weapon table IS its slot's.
// Player_InitPlayer points the local table (g_WeaponSlotArrayBase) at the local player's
// slot table [orig: Player_InitPlayer @0x4E15F0 - @0x4E18A8..0x4E18C1], so the own slot's
// 0x2F accept, its deploy's rebuild and its round init's rebuild all write the local table
// (world::LocalPlayer::inventory), never a second one; every weapon-slot list sent to the
// own slot copies its loadout buffer into the spawn buffer and its class into its side's
// profile class, and for the slot whose entity is the local player selects the slot its
// equipped weapon names and mounts the EquippedSlot [orig: Server_SendWeaponSlotListToPlayer
// @0x502550 - @0x5025EC..0x50264B, @0x5028B8..0x50290E]; the round init seeds the own
// slot's buffer from the spawn buffer [orig: Server_InitAllPlayerEntitiesForRound
// @0x516BC4..0x516BE3]; out of a session the ACCEPT copies its kit into that slot's buffer
// [orig: WeaponLoadout_ApplyFromBuffer @0x565DE4]. The weapons are a model of the test's own;
// every value follows from
// the cited arithmetic (pool = min(count, maxclips) * clipsize).

#include <net/protocol/protocol.h>
#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_loadout_grant.h>
#include <runtime/inmatch/loadout_submit.h>
#include <runtime/inmatch/server_message_dispatch.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/protocol_message.h>
#include <runtime/world/local_player.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/weapon_fsm.h>
#include <runtime/world/world.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "conn_fixture.h"

namespace {

using namespace opennova;
namespace inmatch = opennova::inmatch;
namespace ns = opennova::replication;
namespace w = opennova::world;
using conn_fixture::make_seeded_conn;

int failures = 0;
#define CHECK(c)                                                                               \
	do {                                                                                       \
		if (!(c)) {                                                                            \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                           \
			++failures;                                                                        \
		}                                                                                      \
	} while (0)

constexpr uint8_t kRifleAdm = 5;
constexpr uint8_t kPistolAdm = 6;
constexpr int32_t kRifleCombo = 3 * 65 + 2;
constexpr int32_t kPistolCombo = 2 * 65 + 0;

void make_weapon(w::WeaponTableEntry &def, const char *name, int32_t category, int32_t rank,
		int32_t ammo_class) {
	def.name = name;
	def.valid = true;
	def.category = category;
	def.rank = rank;
	def.clipsize = 10;
	def.maxclips = 5;
	def.startrounds = 10;
	def.ammo_class_id = ammo_class;
	def.ammo_class_count = 1;
	def.ammo_index = 1;
	def.teamfilter = 0x03;
	def.charfilter = 0x1F;
	w::weapon_fsm_bake(nullptr, 0, nullptr, nullptr, nullptr, def.action_fsm);
}

struct Rig {
	std::unique_ptr<w::World> wp = std::make_unique<w::World>();
	w::World &world = *wp;
	std::unique_ptr<w::LocalPlayer> local = std::make_unique<w::LocalPlayer>(world);
	std::unique_ptr<inmatch::NapiNPServerCtx> ctx = std::make_unique<inmatch::NapiNPServerCtx>();
	std::vector<inmatch::NapiNPConnection> roster;
	w::EntityHandle player;

	explicit Rig(bool own) {
		world.registry.configure_pool(0, 4);
		world.rules.mp_session = true;
		world.rules.projectile_authority = true;
		world.ai.is_authority = true;
		world.local_player_state = local.get();
		ctx->np_protocol.identity = opennova::title_protocol("jo");
		w::WeaponTable &table = world.tables.weapons;
		table.ammo_class_names = {"CLASS_556MM", "CLASS_9MM"};
		table.ammo_class_caps = {1000, 1000};
		table.entries.resize(kPistolAdm + 1);
		make_weapon(table.entries[kRifleAdm], "WPN_RIFLE", 3, 2, 0);
		make_weapon(table.entries[kPistolAdm], "WPN_PISTOL", 2, 0, 1);
		w::PlayerSpawn spawn;
		spawn.net_id = 0xFFF1;
		spawn.team = 1;
		// The listen host's own body is the local player; a joiner's is not.
		player = own ? w::spawn_player(world, spawn) : w::spawn_remote_player(world, spawn);
		roster.push_back(make_seeded_conn(2, own ? 2 : 1, nullptr, ns::TransportMode::Client,
				player, true));
		roster[0].link.preround_loadout_latch = true;
		roster[0].assigned_team = 1;
		roster[0].assigned_team_valid = true;
		roster[0].slot_class = 6;
	}
	w::Entity &entity() { return *world.registry.get(player); }
	inmatch::NapiNPConnection &conn() { return roster[0]; }
	std::vector<ProtocolMessage> submit(uint8_t player_class, uint32_t slot) {
		LoadoutSubmit req;
		req.team = 1;
		req.player_class = player_class;
		req.weapon_slot_index = slot;
		req.entries.push_back(LoadoutSubmitEntry{kRifleAdm, 3, 0xFF, 0});
		req.entries.push_back(LoadoutSubmitEntry{kPistolAdm, 2, 0xFF, 0});
		req.terminated = true;
		inmatch::ServerDispatchInputs inputs;
		inputs.server_ctx = ctx.get();
		return inmatch::dispatch_session_replies(inmatch::GameConfig{}, roster[0],
				{make_protocol_message(c2s::LOADOUT_SUBMIT, encode_loadout_submit(req))}, 100,
				roster, &world, inputs);
	}
	const w::WeaponInventorySlot &local_slot(int32_t combo) const {
		return *local->inventory.slot(combo);
	}
};

// The own slot's accept grants into the local table: its slots and pools are the local
// player's (rifle 3 clips: 30 rounds, pistol 2 clips: 20, each less the clip drawn), the
// body's own rows and the slot's pool image stay empty; the accepted class is the slot's.
// [orig: NapiNPServerMsg_HandlePlayerLoadout @0x515790 - the grant @0x515DB5..0x515F4D
//  into slot+0x1D0, the local table on the authority]
void test_the_own_accept_grants_into_the_local_table() {
	Rig r(/*own=*/true);
	CHECK(r.world.cached.local_player == r.player);
	r.submit(8, static_cast<uint32_t>(kRifleCombo));
	CHECK(r.local->inventory_valid);
	CHECK(r.local_slot(kRifleCombo).adm_index == kRifleAdm);
	CHECK(r.local_slot(kPistolCombo).adm_index == kPistolAdm);
	CHECK(r.local_slot(kRifleCombo).clip == 10 && r.local_slot(kPistolCombo).clip == 10);
	CHECK(r.local->inventory.pools[0] == 20 && r.local->inventory.pools[1] == 10);
	CHECK(r.entity().weapon_slots.empty());
	CHECK(r.conn().reply.ammo_pools[0] == 0 && r.conn().reply.ammo_pools[1] == 0);
	CHECK(r.conn().slot_class == 8 && r.conn().player_list_type == 8);
	// A joiner's accept fills its body's rows and its slot's pools, as before.
	Rig j(/*own=*/false);
	j.submit(8, static_cast<uint32_t>(kRifleCombo));
	CHECK(j.entity().weapon_slots.count(static_cast<uint16_t>(kRifleCombo)) == 1);
	CHECK(j.conn().reply.ammo_pools[0] == 20);
	CHECK(!j.local->inventory_valid);
}

// The list's own legs: the slot's buffer becomes the spawn buffer (each kept weapon by
// name with its counts read signed), the slot's class its side's profile class (team 1:
// side A), and the local select takes the slot the body's equipped weapon names: the
// accept pointed EquippedSlot at the submitted pistol, whose index the select keeps.
// [orig: Server_SendWeaponSlotListToPlayer @0x502617..0x502627, @0x502629..0x50264B,
//  @0x5028CD..0x5028FC]
void test_the_own_list_legs() {
	Rig r(/*own=*/true);
	r.entity().equipped_adm_index = kPistolAdm;
	r.submit(7, static_cast<uint32_t>(kPistolCombo));
	const w::LocalPlayerLoadout &kit = r.local->loadout;
	CHECK(kit.spawn_kit().size() == 2);
	if (kit.spawn_kit().size() == 2) {
		CHECK(kit.spawn_kit()[0].name == "WPN_RIFLE" && kit.spawn_kit()[0].ammo_primary == 3 &&
				kit.spawn_kit()[0].ammo_secondary == -1);
		CHECK(kit.spawn_kit()[1].name == "WPN_PISTOL" && kit.spawn_kit()[1].ammo_primary == 2);
	}
	CHECK(r.conn().char_vars.char_class[0] == 7);
	CHECK(r.local->inventory.equipped_combo == kPistolCombo);
	// The rifle's slot is selected when the body's equipped weapon names it.
	r.entity().equipped_adm_index = kRifleAdm;
	(void)inmatch::current_weapon_slot_list(r.conn(), &r.world);
	CHECK(r.local->inventory.equipped_combo == kRifleCombo);
	// A joiner's list touches neither the spawn buffer nor the profile classes.
	Rig j(/*own=*/false);
	j.conn().char_vars.char_class[0] = 5;
	j.submit(7, static_cast<uint32_t>(kRifleCombo));
	CHECK(j.local->loadout.spawn_kit().empty());
	CHECK(j.conn().char_vars.char_class[0] == 5);
}

// The deploy's rebuild of the own slot rebuilds the local table from the slot's buffer:
// an emptied slot and a spent pool come back (D-NET-479).
// [orig: Server_DeployPlayer @0x5178E1 -> PlayerSlot_InitWeaponsFromLoadout @0x515550]
void test_the_deploy_rebuilds_the_local_table() {
	Rig r(/*own=*/true);
	r.submit(8, static_cast<uint32_t>(kRifleCombo));
	r.local->inventory.slots[static_cast<size_t>(kPistolCombo)] = w::WeaponInventorySlot{};
	r.local->inventory.pools[0] = 0;
	inmatch::PlayerSlot_InitWeaponsFromLoadout(r.conn(), r.world, r.entity());
	CHECK(r.local_slot(kPistolCombo).adm_index == kPistolAdm);
	CHECK(r.local->inventory.pools[0] == 20 && r.local->inventory.pools[1] == 10);
	CHECK(r.entity().weapon_slots.empty());
}

// The round init seeds the own slot's buffer from the spawn buffer, which the rebuild
// then reads; a joiner's slot keeps its own.
// [orig: Server_InitAllPlayerEntitiesForRound @0x516BC4..0x516BE3, @0x516BF3..0x516C20]
void test_the_round_init_seeds_the_own_buffer() {
	Rig r(/*own=*/true);
	r.local->loadout.spawn_kit() = {w::WeaponKitEntry{"WPN_PISTOL", 4, -1, -1},
			w::WeaponKitEntry{"WPN_NONE_SUCH", 1, -1, -1}};
	inmatch::PlayerSlot_SeedOwnLoadoutBuffer(r.conn(), r.world);
	const LoadoutSubmit &buffer = r.conn().reply.loadout_buffer;
	CHECK(buffer.entries.size() == 1);
	if (buffer.entries.size() == 1)
		CHECK(buffer.entries[0].adm_index == kPistolAdm && buffer.entries[0].ammo_primary == 4);
	inmatch::PlayerSlot_RebuildKitForRound(r.conn(), r.world, r.entity());
	CHECK(r.local_slot(kPistolCombo).adm_index == kPistolAdm);
	CHECK(r.local_slot(kRifleCombo).adm_index < 0);
	Rig j(/*own=*/false);
	j.local->loadout.spawn_kit() = r.local->loadout.spawn_kit();
	inmatch::PlayerSlot_SeedOwnLoadoutBuffer(j.conn(), j.world);
	CHECK(j.conn().reply.loadout_buffer.entries.empty());
}

// A listen host's in-session armory ACCEPT submits the armory's kit, its class, the body's
// team and the live slot as its own client's 0x2F and applies nothing itself; the own
// slot's accept grants it into the local table.
// [orig: WeaponLoadout_ApplyFromBuffer @0x565CD0 (the session test @0x565D3A) -> NetPacket_SendLoadoutSubmit @0x565D94
//  (the team `movsx ecx, byte [eax+162h]` @0x565D8C); the name gate @0x42cf0b]
void test_the_armory_submit_feeds_the_own_accept() {
	Rig r(/*own=*/true);
	r.submit(8, static_cast<uint32_t>(kRifleCombo));
	const LoadoutSubmit armory = inmatch::armory_accept_in_session(r.world, *r.local,
			opennova::title_protocol("jo"), {w::WeaponKitEntry{"WPN_PISTOL", 4, -1, -1}, w::WeaponKitEntry{"WPN_NONE_SUCH", 1, -1, -1}},
			6, /*authority=*/true, bms::MissionType{});
	CHECK(armory.team == 1 && armory.player_class == 6);
	CHECK(armory.weapon_slot_index == static_cast<uint32_t>(kRifleCombo));
	CHECK(armory.entries.size() == 1);
	if (armory.entries.size() == 1)
		CHECK(armory.entries[0].adm_index == kPistolAdm && armory.entries[0].ammo_primary == 4);
	// The second submit of a live body passes the armory window inside an armory zone
	// [orig: NapiNPServerMsg_HandlePlayerLoadout @0x51581C..0x51583E]; no cooldown.
	r.entity().flags |= w::kEntityFlagArmoryZone;
	r.conn().link.armory_reuse_seconds = 0;
	inmatch::ServerDispatchInputs inputs;
	inputs.server_ctx = r.ctx.get();
	(void)inmatch::dispatch_session_replies(inmatch::GameConfig{}, r.conn(),
			{make_protocol_message(c2s::LOADOUT_SUBMIT, encode_loadout_submit(armory))}, 101,
			r.roster, &r.world, inputs);
	CHECK(r.conn().slot_class == 6);
	CHECK(r.local_slot(kPistolCombo).adm_index == kPistolAdm);
	CHECK(r.local_slot(kRifleCombo).adm_index < 0);
	CHECK(r.local->inventory.pools[1] == 30);
}

// Out of a session the ACCEPT (and the DEATH screen's close-leg apply, the same apply) copies
// its kit into the loadout buffer of the slot holding the local player, so the next deploy
// rebuilds that kit: a socketless single-player host's round-init kit K1 (the rifle), then
// the offline ACCEPT of K2 (the pistol, 4 clips), then a deploy, keeps K2 in the local table.
// The slot walk finds the slot holding the local player and no other.
// [orig: WeaponLoadout_ApplyFromBuffer @0x565CD0 - Entity_ValidatePtr @0x565DAF, the copy into
//  slot+0x170C8 @0x565DE4; Server_DeployPlayer -> PlayerSlot_InitWeaponsFromLoadout @0x5178E1]
// [orig: WeaponLoadout_ApplyFromBuffer @0x564C30 - @0x564D14, @0x564D44;
//  PlayerSlot_InitWeaponsFromLoadout called @0x514B22 (dfx2.exe)]
void test_the_offline_accept_keeps_its_kit_for_the_next_deploy() {
	Rig r(/*own=*/true);
	r.world.rules.mp_session = false;
	CHECK(inmatch::local_player_slot(r.roster, r.world) == &r.conn());
	r.local->loadout.spawn_kit() = {w::WeaponKitEntry{"WPN_RIFLE", 3, -1, -1}};
	inmatch::PlayerSlot_SeedOwnLoadoutBuffer(r.conn(), r.world);
	inmatch::PlayerSlot_RebuildKitForRound(r.conn(), r.world, r.entity());
	CHECK(r.local_slot(kRifleCombo).adm_index == kRifleAdm);
	CHECK(inmatch::armory_accept_offline(r.world, *r.local,
			inmatch::local_player_slot(r.roster, r.world),
			{w::WeaponKitEntry{"WPN_PISTOL", 4, -1, -1}}, 8));
	const LoadoutSubmit &buffer = r.conn().reply.loadout_buffer;
	CHECK(buffer.entries.size() == 1);
	if (buffer.entries.size() == 1)
		CHECK(buffer.entries[0].adm_index == kPistolAdm && buffer.entries[0].ammo_primary == 4);
	CHECK(r.local_slot(kPistolCombo).adm_index == kPistolAdm);
	CHECK(r.local_slot(kRifleCombo).adm_index < 0);
	// The deploy's rebuild reads the buffer: the pistol, its four clips less the one drawn.
	inmatch::PlayerSlot_InitWeaponsFromLoadout(r.conn(), r.world, r.entity());
	CHECK(r.local_slot(kPistolCombo).adm_index == kPistolAdm);
	CHECK(r.local_slot(kRifleCombo).adm_index < 0);
	CHECK(r.local->inventory.pools[1] == 30);
	// No slot (no session context): the local apply alone.
	Rig n(/*own=*/true);
	n.world.rules.mp_session = false;
	CHECK(inmatch::armory_accept_offline(n.world, *n.local, nullptr,
			{w::WeaponKitEntry{"WPN_PISTOL", 4, -1, -1}}, 8));
	CHECK(n.local_slot(kPistolCombo).adm_index == kPistolAdm);
	CHECK(n.conn().reply.loadout_buffer.entries.empty());
	// A joiner's slot never holds the local player.
	Rig j(/*own=*/false);
	CHECK(inmatch::local_player_slot(j.roster, j.world) == nullptr);
}

} // namespace

int main() {
	test_the_own_accept_grants_into_the_local_table();
	test_the_own_list_legs();
	test_the_deploy_rebuilds_the_local_table();
	test_the_round_init_seeds_the_own_buffer();
	test_the_armory_submit_feeds_the_own_accept();
	test_the_offline_accept_keeps_its_kit_for_the_next_deploy();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("PASS npruntime_own_slot_table\n");
	return 0;
}
