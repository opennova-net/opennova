// D-NET-693: the C2S 0x2F accept's availability gate. After the class and type masks the
// accept reads g_ArmoryWeaponAvailability at the entry's AdmDef index and keeps the entry
// only when it is nonzero, and for the armory-only value (2 in Jointops.exe) only while
// the requester's entity Flags holds 0x400000 [orig: NapiNPServerMsg_HandlePlayerLoadout
// @0x515790 (the read @0x515A3F, the zero test @0x515A46..0x515A48, `cmp eax, 2`
// @0x515A4A, the Flags test @0x515A4F..0x515A58)]; a refused entry is not kept
// (@0x515A5A..0x515A7E): no slot, no pool, no loadout-buffer entry. The host's table is
// NapiNPServerCtx::weapon_restrictions (an absent index reads 1). The weapons and the
// table are a model of the test's own.

#include <runtime/inmatch/host_session.h>
#include <runtime/inmatch/loopback_channel.h>
#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_initial_state.h>
#include <runtime/inmatch/server_message_dispatch.h>
#include <base/gameprofile/gameprofile.h>
#include <formats/mission/bms.h>
#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/protocol_message.h>
#include <net/protocol/protocol.h>
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
constexpr uint8_t kMortarAdm = 6;
constexpr uint16_t kRifleCombo = 3 * 65 + 2;
constexpr uint16_t kMortarCombo = 7 * 65 + 0;

void make_weapon(w::WeaponTableEntry &def, const char *name, int32_t category, int32_t rank) {
	def.name = name;
	def.valid = true;
	def.category = category;
	def.rank = rank;
	def.clipsize = 10;
	def.startrounds = 10;
	def.ammo_class_id = 0;
	def.ammo_class_count = 1;
	def.ammo_index = 1;
	def.teamfilter = 0x03;
	def.charfilter = 0x1F;
	w::weapon_fsm_bake(nullptr, 0, nullptr, nullptr, nullptr, def.action_fsm);
}

struct Rig {
	std::unique_ptr<w::World> wp = std::make_unique<w::World>();
	w::World &world = *wp;
	std::unique_ptr<inmatch::NapiNPServerCtx> ctx = std::make_unique<inmatch::NapiNPServerCtx>();
	std::vector<inmatch::NapiNPConnection> roster;
	w::EntityHandle player;

	Rig() {
		world.registry.configure_pool(0, 4);
		world.rules.mp_session = true;
		world.rules.projectile_authority = true;
		world.ai.is_authority = true;
		w::WeaponTable &table = world.tables.weapons;
		table.ammo_class_names = {"CLASS_556MM"};
		table.ammo_class_caps = {1000};
		table.entries.resize(kMortarAdm + 1);
		make_weapon(table.entries[kRifleAdm], "WPN_RIFLE", 3, 2);
		make_weapon(table.entries[kMortarAdm], "WPN_MORTAR", 7, 0);
		w::PlayerSpawn spawn;
		spawn.net_id = 0xFFF1;
		player = w::spawn_remote_player(world, spawn);
		roster.push_back(make_seeded_conn(2, 1, nullptr, ns::TransportMode::Client, player, true));
		// The first submit after the deploy passes the armory window (the latch).
		roster[0].link.preround_loadout_latch = true;
		roster[0].assigned_team = 1;
	}
	w::Entity &entity() { return *world.registry.get(player); }
	std::vector<ProtocolMessage> replies;
	void submit() {
		LoadoutSubmit req;
		req.team = 1;
		req.player_class = 8;
		req.weapon_slot_index = kRifleCombo;
		req.entries.push_back(LoadoutSubmitEntry{kRifleAdm, 20, 0, 0});
		req.entries.push_back(LoadoutSubmitEntry{kMortarAdm, 20, 0, 0});
		req.terminated = true;
		inmatch::ServerDispatchInputs inputs;
		inputs.server_ctx = ctx.get();
		replies = inmatch::dispatch_session_replies(inmatch::GameConfig{}, roster[0],
				{make_protocol_message(c2s::LOADOUT_SUBMIT, encode_loadout_submit(req))}, 100,
				roster, &world, inputs);
	}
	// The accept's S2C 0x5A slot list names the weapon [orig:
	// Server_SendWeaponSlotListToPlayer @0x515F95].
	bool listed(uint8_t adm) const {
		for (const ProtocolMessage &m : replies) {
			if (m.tag != s2c::WEAPON_LOADOUT) continue;
			WeaponLoadout list;
			if (!decode_weapon_loadout(m.payload.data(), m.payload.size(), list)) continue;
			for (const WeaponLoadoutSlot &slot : list.slots)
				if (slot.type_id == adm) return true;
		}
		return false;
	}
	bool replied() const {
		for (const ProtocolMessage &m : replies)
			if (m.tag == s2c::WEAPON_LOADOUT) return true;
		return false;
	}
	bool kept(uint8_t adm) const {
		for (const LoadoutSubmitEntry &e : roster[0].reply.loadout_buffer.entries)
			if (e.adm_index == adm) return true;
		return false;
	}
};

void test_an_unrestricted_table_keeps_every_entry() {
	Rig r;
	r.submit();
	CHECK(r.kept(kRifleAdm) && r.kept(kMortarAdm));
	CHECK(r.entity().weapon_slots.count(kMortarCombo) == 1);
}

void test_a_banned_entry_is_not_kept() {
	Rig r;
	r.ctx->weapon_restrictions = {{kMortarAdm, 0}};
	r.submit();
	CHECK(r.kept(kRifleAdm));
	CHECK(!r.kept(kMortarAdm));
	CHECK(r.entity().weapon_slots.count(kRifleCombo) == 1);
	CHECK(r.entity().weapon_slots.count(kMortarCombo) == 0);
	// The wire half: the accept's 0x5A lists the kept rifle and no row for the mortar.
	CHECK(r.replied());
	CHECK(r.listed(kRifleAdm));
	CHECK(!r.listed(kMortarAdm));
}

void test_an_armory_only_entry_needs_the_zone() {
	{
		Rig r;
		r.ctx->weapon_restrictions = {{kMortarAdm, 2}};
		r.submit();
		CHECK(!r.kept(kMortarAdm));
		CHECK(r.entity().weapon_slots.count(kMortarCombo) == 0);
	}
	{
		Rig r;
		r.ctx->weapon_restrictions = {{kMortarAdm, 2}};
		r.entity().flags |= w::kEntityFlagArmoryZone;
		r.submit();
		CHECK(r.kept(kMortarAdm));
		CHECK(r.entity().weapon_slots.count(kMortarCombo) == 1);
	}
}

// The first mission's tables on the wire: the joiner's player-sync tail carries the S2C 0x66
// (count, then each (index, value)) and the 0x76 (the class mask, u16 LE).
// [orig: NetPacket_SerializeWeaponRestrictionTable @0x5102C0; NetPacket_WriteClassAllowMask
//  @0x510350; Server_SendPlayerSlotStateFrame @0x51BBA0]
bool tail_bodies(inmatch::NapiNPServerCtx &ctx, w::World &world, std::vector<uint8_t> &table,
		std::vector<uint8_t> &mask) {
	inmatch::NapiNPConnection &joiner = ctx.np_protocol.connection_list.emplace_back();
	joiner.type = 1;
	joiner.connection_id = inmatch::kFirstJoinerDcb;
	joiner.phase = inmatch::ConnectionPhase::PlayerAdded;
	joiner.slot_state = inmatch::PlayerSlotState::Added;
	inmatch::ServerDispatchInputs inputs;
	inputs.server_ctx = &ctx;
	(void)inmatch::dispatch_session_replies(ctx.config, joiner,
			{make_protocol_message(c2s::CHECKSUM_RESPONSE, {})}, 1,
			ctx.np_protocol.connection_list, &world, inputs);
	for (int i = 0; i < 64; ++i) {
		const inmatch::InitialStateStep step =
				inmatch::Server_SendInitialGameStateToPlayer(ctx, joiner, 1);
		for (const inmatch::InitialStateMessage &m : step.messages) {
			if (m.tag == s2c::WEAPON_RESTRICTIONS) table = m.body;
			if (m.tag == s2c::CLASS_ALLOW_MASK) mask = m.body;
		}
		if (!table.empty() && !mask.empty()) return true;
	}
	return false;
}

// The table's source: the session's first mission builds it from the cfg's per-weapon words,
// each 3 (Jointops.exe's mission value) taking the mission's own word from its
// item_availability chunk through the name-list mode (unnamed entries 1, -1 -> 3), then the
// template mode, where a parent's sub-entries keep the parent's value whatever their own
// word; the 0 and armory-only (2) entries are the table the S2C 0x66 and the 0x2F accept
// read. A session-less (single player) host builds none.
// [orig: Game_ApplySessionSettingsToGlobals @0x551500 (@0x55179C..0x5517A7, @0x5517AD..0x5517B3, the
//  merge @0x5517C5..0x55181A, the build @0x551831); WeaponDef_BuildItemRestrictionTable
//  @0x54DDB0 (the name list @0x54DDC3..0x54DE6C, the template @0x54DE7E..0x54DF3D)]
void test_the_first_mission_builds_the_table() {
	for (const bool session : {true, false}) {
		auto world = std::make_unique<w::World>();
		w::WeaponTable &table = world->tables.weapons;
		table.ammo_class_names = {"CLASS_556MM"};
		table.ammo_class_caps = {1000};
		table.entries.resize(kMortarAdm + 1);
		table.entries[0].name = "null";
		table.entries[0].valid = true;
		make_weapon(table.entries[1], "WPN_PARENT", 3, 0);
		table.entries[1].loadout_subclasses = 1;
		make_weapon(table.entries[2], "WPN_PARENT_SUB", 3, 1);
		make_weapon(table.entries[3], "WPN_PISTOL", 2, 0);
		make_weapon(table.entries[4], "WPN_SMG", 4, 0);
		make_weapon(table.entries[kRifleAdm], "WPN_RIFLE", 3, 2);
		make_weapon(table.entries[kMortarAdm], "WPN_MORTAR", 7, 0);
		auto mission = std::make_unique<bms::File>();
		mission->item_availability = {
				{"wpn_parent", 0},   // banned (the name match ignores case)
				{"WPN_MORTAR", 2},   // armory only
				{"WPN_SMG", 0xFF},   // -1: the mission allows it (3)
				{"WPN_RIFLE", 0},    // banned by the mission, but the cfg word is not 3
		};
		auto owner = std::make_unique<inmatch::HostOwner>();
		replication::LoopbackChannel loop;
		owner->host_loopback = &loop;
		owner->ctx.world = world.get();
		owner->ctx.mission = mission.get();
		inmatch::HostConfig cfg;
		cfg.title = opennova::title_protocol("jo");
		cfg.config.server_name = "availability";
		cfg.config.max_players = 4;
		cfg.socket_mode = session ? inmatch::SocketMode::Lan : inmatch::SocketMode::Socketless;
		cfg.serve_and_play = !session;
		// The cfg's words: the stock 3s, the sub-entry's own word 1 (its parent's value
		// wins), the pistol banned outright, the rifle allowed outright.
		cfg.config.weapon_availability[2] = 1;
		cfg.config.weapon_availability[3] = 0;
		cfg.config.weapon_availability[kRifleAdm] = 1;
		// The class words: class 1 off, class 2 on through the mission entry's mask (2),
		// the rest the stock 1.
		cfg.config.class_availability[1] = 0;
		cfg.config.class_availability[2] = 2;
		world->registry.configure_pool(0, 4);
		inmatch::start_host_session(*owner, cfg);
		// The session onto its World (the create reads none): its rules, the own add.
		inmatch::bind_host_session_world(*owner, cfg);
		const std::vector<std::pair<uint8_t, uint8_t>> expected = session
				? std::vector<std::pair<uint8_t, uint8_t>>{{1, 0}, {2, 0}, {3, 0}, {kMortarAdm, 2}}
				: std::vector<std::pair<uint8_t, uint8_t>>{};
		CHECK(owner->ctx.weapon_restrictions == expected);
		CHECK(owner->ctx.config.class_allow_mask == (session ? 0x03FDu : 0x03FFu));
		if (!session) continue;
		std::vector<uint8_t> table_body, mask_body;
		CHECK(tail_bodies(owner->ctx, *world, table_body, mask_body));
		CHECK(table_body == std::vector<uint8_t>({4, 1, 0, 2, 0, 3, 0, kMortarAdm, 2}));
		CHECK(mask_body == std::vector<uint8_t>({0xFD, 0x03}));
	}
}

} // namespace

int main() {
	test_the_first_mission_builds_the_table();
	test_an_unrestricted_table_keeps_every_entry();
	test_a_banned_entry_is_not_kept();
	test_an_armory_only_entry_needs_the_zone();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("PASS npruntime_loadout_availability\n");
	return 0;
}
