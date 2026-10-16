// A host speaking Delta Force: Xtreme 2 takes DFX2's branches of the shared host code
// (docs/titles/dfx2-vs-jo-re.md §7, the §8 catalog), each checked against JO's over the same call:
//
//   DFX2-2   the S2C 0x0F flags byte's bit 0 is set with no spawn zones
//   DFX2-10  the remote admin's WEAPON SET ARMORY stores 3, the value S2C 0x66 carries
//   DFX2-11  a join leaves the slot undeployed with the loadout latch, and every deploy sets the
//            latch, whatever the mission and the pre-round timer
//   DFX2-13  a LAN join check refuses a missing (10), undecodable (11), the host's own (12) or
//            another player's (13) CDKIID; a NovaWorld session and a JO host test none
//   DFX2-14  the chat's squad-tag text is wrapped in <ch> and <co>
//   DFX2-18  the mission-data block's rules word (S2C 0x64 +44) carries 0x10000
//   the 0x42 build gates: the title's BN / VN / MBN / SOPD (codes 2, 3, 4, 8), the same values
//            for both titles
//
// [orig: NetPacket_WriteWorldStateLoad0x0F @0x502da7; CAdminServer_HandleWeaponCommand
//  @0x40482e; Server_OnPlayerJoin @0x51a6d3..0x51a6f2; Server_DeployPlayer @0x517803..0x517812;
//  Server_ValidatePlayerJoinRequest @0x512100; NapiNPServer_HandleChatMessage @0x513930;
//  CNapiGameSession_InitRandomSeedOrRequest @0x51e94b..0x51e957]
// [orig: @0x4ffeed, @0x40480e,
//  @0x517717, @0x514a55, @0x50f3e2..0x50f565, @0x510b43..0x510b6a, @0x51b89e (dfx2.exe)]

#include <base/gameprofile/gameprofile.h>
#include <net/admin/admin_server.h>
#include <net/novacrypto/cdkey.h>
#include <net/npwire/flat_tlv.h>
#include <net/npwire/ingame_decode.h>
#include <net/npwire/protocol_message.h>
#include <net/npwire/session_hello.h>
#include <net/title/title_identity.h>
#include <runtime/inmatch/admin_console.h>
#include <runtime/inmatch/loopback_channel.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_chat.h>
#include <runtime/inmatch/server_initial_state.h>
#include <runtime/inmatch/server_message_dispatch.h>
#include <runtime/inmatch/server_session.h>
#include <runtime/inmatch/server_spawn.h>
#include <runtime/inmatch/udp_session_transport.h>
#include <runtime/world/entity.h>
#include <runtime/world/world.h>

#include "host_test_setup.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace opennova;
namespace w = opennova::world;

namespace {

int g_failures = 0;

void expect(bool cond, const char *what) {
	if (!cond) {
		std::fprintf(stderr, "FAIL: %s\n", what);
		++g_failures;
	}
}

const TitleIdentity *jo() { return title_identity(gameprofile::GAME_JO); }
const TitleIdentity *dfx2() { return title_identity(gameprofile::GAME_DFX2); }

// A listen host on a mission with one start marker and no spawn zone, speaking `title`: its own
// player joined through the round init and the pending spawns, the 0x0F of its burst decoded.
struct JoinedHost {
	w::World world;
	replication::LoopbackChannel loopback;
	inmatch::NapiNPServerCtx ctx;
	WorldStateLoad state;
	bool decoded = false;
};

void join_own_player(JoinedHost &host, const TitleIdentity *title) {
	host.world.registry.configure_pool(0, 16);
	host.world.registry.configure_pool(3, 16);
	w::Entity start;
	start.kind = w::EntityKind::Marker;
	start.item_id = 6002;
	start.position = {1.0f, 20.0f, 1.0f};
	start.item_type_index = 1;
	start.has_item_def = true;
	host.world.registry.spawn_at(w::EntityHandle::make(3, 0), start);
	host.world.rules.mp_session = true;
	inmatch::test::bring_up_host(host.ctx, inmatch::ConnectionMode::HostClient,
			inmatch::SocketMode::Socketless, /*host_key=*/0, &host.loopback);
	host.ctx.np_protocol.identity = title;
	host.ctx.world = &host.world;
	inmatch::Server_InitNewRoundState(host.ctx);
	inmatch::Server_ProcessPendingPlayerSpawns(host.ctx, host.world);
	inmatch::NapiNPConnection &conn = host.ctx.np_protocol.connection_list[0];
	std::vector<uint8_t> body;
	bool reached = false;
	for (int i = 0; i < 64 && !reached; ++i) {
		inmatch::InitialStateStep step =
				inmatch::Server_SendInitialGameStateToPlayer(host.ctx, conn, /*now_tick=*/1);
		if (!step.advanced) break;
		for (const inmatch::InitialStateMessage &m : step.messages)
			if (m.tag == 0x0F) body = m.body;
		reached = step.reached_in_game;
	}
	host.decoded = reached && !body.empty() &&
			decode_world_state_load(body.data(), body.size(), host.state);
}

void world_state_flags_and_join_state() {
	for (const TitleIdentity *title : {jo(), dfx2()}) {
		const bool is_dfx2 = title == dfx2();
		JoinedHost host;
		join_own_player(host, title);
		expect(host.decoded, "the burst carries a decodable 0x0F");
		expect(!host.world.zones.has_spawn_zone(), "the mission has no spawn zone");
		expect((host.state.game_flags & 0x01u) == (is_dfx2 ? 0x01u : 0x00u),
		       is_dfx2 ? "DFX2-2: a DFX2 host's 0x0F sets bit 0 with no spawn zone"
		               : "a JO host's 0x0F leaves bit 0 clear with no spawn zone");
		inmatch::NapiNPConnection &conn = host.ctx.np_protocol.connection_list[0];
		expect(conn.link.respawn_pending == is_dfx2 && conn.link.preround_loadout_latch == is_dfx2,
		       is_dfx2 ? "DFX2-11: a DFX2 join is undeployed with the loadout latch (0x13)"
		               : "a JO join with no spawn zone or pre-round is deployed, unlatched");
		conn.link.preround_loadout_latch = false;
		(void)inmatch::Server_ReleasePlayerDeployment(host.ctx.config, conn, host.world, {}, 0xFFFF,
				nullptr, title);
		expect(conn.link.preround_loadout_latch == is_dfx2,
		       is_dfx2 ? "DFX2-11: a DFX2 deploy sets the loadout latch (3)"
		               : "a JO deploy outside the pre-round leaves it clear");
	}
}

// The admin console's WEAPON SET: ARMORY stores the title's armory-only value, which the host's
// restriction table keeps (the 0x66 entries).
void admin_weapon_armory() {
	for (const TitleIdentity *title : {jo(), dfx2()}) {
		inmatch::NapiNPServerCtx ctx;
		inmatch::set_connection_mode(ctx, inmatch::ConnectionMode::HostOnly);
		ctx.np_protocol.identity = title;
		inmatch::AdminConsole console(ctx, inmatch::AdminConsole::Seams{});
		AdminSession session;
		session.rights = 0xFFFFFFFFu;
		std::vector<std::string> replies;
		console.dispatch(session, "WEAPON SET 5 ARMORY", replies);
		console.dispatch(session, "WEAPON SET 6 NEVER", replies);
		const uint8_t armory = title == dfx2() ? 3 : 2;
		expect(ctx.weapon_restrictions ==
		               std::vector<std::pair<uint8_t, uint8_t>>{{5, armory}, {6, 0}},
		       title == dfx2() ? "DFX2-10: a DFX2 host's ARMORY is 3" : "a JO host's ARMORY is 2");
	}
}

// The game-layer JOIN through the dispatcher on a LAN session; the refusal's DPC, 0 admitted.
uint32_t join_code(inmatch::NapiNPServerCtx &ctx, const std::string &cdkiid,
		const std::vector<cdkey::WireKeys> &others = {}) {
	replication::UdpSessionTransport transport(replication::UdpSessionTransport::Role::Host);
	std::vector<inmatch::NapiNPConnection> roster(1 + others.size());
	inmatch::NapiNPConnection &conn = roster[0];
	conn.type = inmatch::NapiNPConnection::kTypeServerSide;
	conn.link.mode = replication::TransportMode::Client;
	conn.link.transport = &transport;
	conn.phase = inmatch::ConnectionPhase::Joined;
	conn.admission_stage = inmatch::GameAdmissionStage::AwaitJoinRequest;
	conn.join_environment = {0, 2, 1, 0, 20042002, 180};
	for (std::size_t i = 0; i < others.size(); ++i) roster[1 + i].cd_keys = others[i];
	std::vector<uint8_t> payload;
	const auto append_string = [&payload](const char *name, const std::string &value) {
		std::string with_nul = value;
		with_nul.push_back('\0');
		append_flat_tlv(payload, name, reinterpret_cast<const uint8_t *>(with_nul.data()),
				static_cast<uint16_t>(with_nul.size()));
	};
	if (!cdkiid.empty()) append_string("CDKIID", cdkiid);
	append_string("VERSIONCRCSTRING", "0");
	inmatch::ServerDispatchInputs inputs;
	inputs.server_ctx = &ctx;
	ctx.config.expansion.clear();
	(void)inmatch::dispatch_session_replies(ctx.config, roster[0],
			{make_protocol_message(0x00, payload)}, 5, roster, nullptr, inputs);
	expect(roster[0].cd_keys.cdkiid == cdkiid, "the JOIN's CDKIID lands on the connection");
	replication::Datagram staged;
	if (!transport.pop_outbound(staged)) return 0;
	DisconnectEvent event;
	if (!parse_disconnect_event(staged.body.data(), staged.body.size(), event)) return 0;
	return event.dpc;
}

void lan_cd_key_check() {
	const std::string key = cdkey::make_wire_key("OPENNOVA-0001");
	const std::string other_key = cdkey::make_wire_key("OPENNOVA-0002");
	inmatch::NapiNPServerCtx ctx;
	inmatch::set_connection_mode(ctx, inmatch::ConnectionMode::HostOnly);
	ctx.is_in_session = 1;
	ctx.transport_mode = inmatch::NetworkType::Lan;
	ctx.np_protocol.identity = dfx2();
	expect(join_code(ctx, key) == 0, "DFX2-13: a decodable CDKIID of its own joins");
	expect(join_code(ctx, "") == 10, "DFX2-13: no CDKIID is refused 10 (GDC010)");
	expect(join_code(ctx, "QQQQ") == 11, "DFX2-13: an undecodable CDKIID is refused 11");
	ctx.host_cd_keys.cdkiid = key;
	expect(join_code(ctx, key) == 12, "DFX2-13: the host's own CDKIID is refused 12");
	ctx.host_cd_keys.cdkiid.clear();
	expect(join_code(ctx, key, {cdkey::WireKeys{other_key, ""}, cdkey::WireKeys{key, ""}}) == 13,
	       "DFX2-13: another player's CDKIID is refused 13");
	ctx.transport_mode = inmatch::NetworkType::NovaWorld;
	expect(join_code(ctx, "") == 0, "a DFX2 NovaWorld session runs no CD-key leg");
	ctx.transport_mode = inmatch::NetworkType::Lan;
	ctx.np_protocol.identity = jo();
	expect(join_code(ctx, "") == 0, "a JO LAN host has no CD-key leg");
}

// The 0x42 build gates read the title's values: the same four for JO and DFX2.
void join_build_values() {
	for (const TitleIdentity *title : {jo(), dfx2()}) {
		const TitleJoinBuild &b = title_join_build(title);
		expect(b.bn == 1 && b.vn == 2 && b.mbn == 20042002 && b.sopd == 180,
		       "BN 1, VN 2, MBN 20042002, SOPD 180 in both titles");
	}
	expect(&title_join_build(nullptr) == &title_join_build(jo()), "no title reads JO's");
}

void chat_squad_tag() {
	expect(inmatch::chat_squad_tag_text("OPN", title_in_match(jo()).chat_squad_tag_wrapped) == "OPN",
	       "JO's relay appends the squad tag bare");
	expect(inmatch::chat_squad_tag_text("OPN", title_in_match(dfx2()).chat_squad_tag_wrapped) ==
	               "<ch>OPN<co>",
	       "DFX2-14: DFX2's wraps it in <ch> and <co>");
}

void rules_word() {
	inmatch::GameConfig config;
	config.mp_attributes = 0x3A06u;
	const auto word = [&config](const TitleIdentity *title) {
		const inmatch::MissionMetadataBlob blob =
				inmatch::build_mission_metadata_blob(config, /*mp_session_peer=*/true, title);
		return static_cast<uint32_t>(blob[44]) | (static_cast<uint32_t>(blob[45]) << 8) |
				(static_cast<uint32_t>(blob[46]) << 16) | (static_cast<uint32_t>(blob[47]) << 24);
	};
	expect(word(jo()) == 0x3A06u && word(nullptr) == 0x3A06u, "a JO block's +44 is mpattrib");
	expect(word(dfx2()) == 0x13A06u, "DFX2-18: a DFX2 block's +44 carries 0x10000 (the capture's 0x13A06)");
}

} // namespace

int main() {
	world_state_flags_and_join_state();
	admin_weapon_armory();
	lan_cd_key_check();
	join_build_values();
	chat_squad_tag();
	rules_word();
	if (g_failures == 0) {
		std::printf("npruntime_dfx2_host: all checks passed\n");
		return 0;
	}
	std::printf("npruntime_dfx2_host: %d check(s) failed\n", g_failures);
	return 1;
}
