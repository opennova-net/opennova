// D-NET-693: the C2S 0x2F accept's availability gate. After the class and type masks the
// accept reads g_ArmoryWeaponAvailability at the entry's AdmDef index and keeps the entry
// only when it is nonzero, and for the armory-only value (2 in Jointops.exe) only while
// the requester's entity Flags holds 0x400000 [orig: NapiNPServerMsg_HandlePlayerLoadout
// @0x515790 (the read @0x515A3F, the zero test @0x515A46..0x515A48, `cmp eax, 2`
// @0x515A4A, the Flags test @0x515A4F..0x515A58)]; a refused entry is not kept
// (@0x515A5A..0x515A7E): no slot, no pool, no loadout-buffer entry. The host's table is
// NapiNPServerCtx::weapon_restrictions (an absent index reads 1). The weapons and the
// table are a model of the test's own.

#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_message_dispatch.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/protocol_message.h>
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
		(void)inmatch::dispatch_session_replies(inmatch::GameConfig{}, roster[0],
				{make_protocol_message(c2s::LOADOUT_SUBMIT, encode_loadout_submit(req))}, 100,
				roster, &world, inputs);
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

} // namespace

int main() {
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
