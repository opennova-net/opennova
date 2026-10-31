// The NovaWorld host listing's match side (inmatch/host_listing_binding.h), the one binding the
// game's listen host and opennova-serve run: the context's hooks a bind installs and an unbind
// takes back, the columns read off the bound match (the GCC the session create's CC.BIN), the
// registration onto the context (the AppIds, the arms, the login PCID), hooks left on a context
// the binding no longer reaches doing nothing, the PlayerList rows with and without the host's own
// slot, and the NWU session's facts. Socket-free: the roles' sessions are never opened.
// [orig: Lobby_UpdateServerInfo @0x4FE8C0 - byte_C87044 pushed @0x4FF29C;
//  Server_ToggleJoinLock @0x50D750 -> Lobby_UpdateServerInfo(0) @0x50D762;
//  CNapiNetwork_CheckPlayerTimeouts @0x4C8AD0 - the arm @0x4C8B88]
#include <base/gameprofile/game_type.h>
#include <net/novaworld/gate_metrics.h>
#include <net/novaworld/lobby_vars.h>
#include <net/novaworld/nwu_host_role.h>
#include <net/novaworld/nwu_lobby_session.h>
#include <runtime/inmatch/host_listing_binding.h>
#include <runtime/inmatch/napi_np_server_ctx.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace opennova;

namespace {

int failures = 0;
#define CHECK(c)                                                                                   \
	do {                                                                                           \
		if (!(c)) {                                                                                \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                               \
			++failures;                                                                            \
		}                                                                                          \
	} while (0)

std::string gametext(const char *section, const char *key, const char *fallback) {
	if (std::strcmp(section, "GateTypeAbbrev") == 0 && std::strcmp(key, "CTF") == 0) return "Flag";
	return fallback;
}

// A role over a session that never opens: Idle, no GSID, no ticket rule.
struct Listing {
	NwuLobbySession lobby{NwuLobbySession::Hooks{}, NwuLobbySession::Environment{}};
	NwuHostRole role{lobby, NwuHostRole::Hooks{}};
};

inmatch::NapiNPConnection added(uint8_t type, uint8_t slot, const char *name, int32_t team) {
	inmatch::NapiNPConnection c;
	c.type = type;
	c.phase = inmatch::ConnectionPhase::PlayerAdded;
	c.reply.player_slot = slot;
	c.player_name = name;
	c.player_list_team = team;
	c.player_list_type = 3;
	return c;
}

bool hooks_installed(const inmatch::NapiNPServerCtx &ctx) {
	return ctx.on_player_enter_request && ctx.on_update_server_info && ctx.on_lan_listing_address &&
			ctx.gate_metrics.host && ctx.gate_metrics.send && ctx.gate_metrics.game_text;
}

bool hooks_cleared(const inmatch::NapiNPServerCtx &ctx) {
	return !ctx.on_player_enter_request && !ctx.on_update_server_info &&
			!ctx.on_lan_listing_address && !ctx.gate_metrics.host && !ctx.gate_metrics.send;
}

// Bound, the context's hooks reach the session and each role reads the match's columns; unbound,
// both go with the registration the context carried.
void test_bind_and_unbind() {
	Listing first;
	inmatch::NapiNPServerCtx ctx;
	ctx.config.server_name = "Live Name";
	ctx.config.custom_text = "live message";
	ctx.config.game_type = game_type::kCaptureTheFlag;
	ctx.cc_bin = "us";
	inmatch::HostListingBinding binding;
	HostRegistration r;
	r.server_name = "Registered";
	CHECK(!binding.bound());
	CHECK(!binding.read_columns(r) && r.server_name == "Registered");

	binding.bind([&ctx] { return &ctx; }, first.role, gametext);
	CHECK(binding.bound());
	CHECK(hooks_installed(ctx));
	// The columns are the live session's, the GCC the CC.BIN the session create read.
	CHECK(binding.read_columns(r));
	CHECK(r.server_name == "Live Name" && r.server_message == "live message");
	CHECK(r.game_type == "Flag");
	CHECK(r.gcc == "us");
	// The gate reports' strings are the session's; its game text the binding's.
	CHECK(ctx.gate_metrics.game_text("GateTypeAbbrev", "CTF", "") == "Flag");
	// A server-info update and a LAN address with no session open send nothing and hold nothing.
	ctx.on_update_server_info();
	ctx.on_lan_listing_address(0, 0x0100007Fu, 32768u);
	inmatch::NapiNPServerCtx::PlayerEnterRequest request;
	request.listing = 1; // the second listing's, which there is none of
	ctx.on_player_enter_request(request);

	// The registration: no session, so no GSID, AppId or ticket rule; the login PCID on the
	// listen host's own connection.
	ctx.np_protocol.connection_list.push_back(
			added(inmatch::NapiNPConnection::kTypeClientSide, 0, "Host", 1));
	ctx.novaworld_gsid = "stale";
	ctx.novaworld_join_tickets_armed = true;
	binding.sync_registration(ctx, "1234");
	CHECK(ctx.novaworld_gsid.empty() && ctx.novaworld_app_id == 0u);
	CHECK(!ctx.novaworld_join_tickets_armed);
	CHECK(ctx.np_protocol.connection_list[0].account.pcid == "1234");
	CHECK(ctx.novaworld_second_app_id == 0u && !ctx.novaworld_second_join_tickets_armed);

	ctx.novaworld_gsid = "gsid";
	ctx.novaworld_app_id = 7;
	ctx.novaworld_second_app_id = 9;
	ctx.novaworld_join_tickets_armed = true;
	ctx.novaworld_second_join_tickets_armed = true;
	binding.unbind();
	CHECK(!binding.bound());
	CHECK(hooks_cleared(ctx));
	CHECK(ctx.novaworld_gsid.empty() && ctx.novaworld_app_id == 0u);
	CHECK(ctx.novaworld_second_app_id == 0u);
	CHECK(!ctx.novaworld_join_tickets_armed && !ctx.novaworld_second_join_tickets_armed);
	CHECK(!binding.read_columns(r));
	// A second unbind has nothing to take back.
	binding.unbind();
}

// A map change binds again over the kept context: the hooks stand, the second listing set before
// stays set across the rebind and an unbind.
void test_rebind_keeps_the_second() {
	Listing first, second;
	inmatch::NapiNPServerCtx ctx;
	inmatch::HostListingBinding binding;
	binding.set_second(&second.role); // unbound: kept for the bind
	binding.bind([&ctx] { return &ctx; }, first.role, gametext);
	CHECK(hooks_installed(ctx));
	binding.bind([&ctx] { return &ctx; }, first.role, gametext);
	CHECK(binding.bound() && hooks_installed(ctx));
	// The second listing hosts nothing: no AppId or arm of its on the context.
	binding.sync_registration(ctx, std::string());
	CHECK(ctx.novaworld_second_app_id == 0u && !ctx.novaworld_second_join_tickets_armed);
	// A joiner of the second listing reaches its role (nothing open: nothing sent).
	inmatch::NapiNPServerCtx::PlayerEnterRequest request;
	request.listing = 1;
	ctx.on_player_enter_request(request);
	ctx.on_lan_listing_address(1, 0x0100007Fu, 32768u);
	binding.unbind();
	CHECK(hooks_cleared(ctx));
	binding.bind([&ctx] { return &ctx; }, first.role, gametext);
	CHECK(hooks_installed(ctx));
	binding.set_second(nullptr);
	binding.unbind();
}

// A context the match no longer resolves to at the unbind (the game's simulation gone ahead of
// the session) keeps its hooks, which reach no role from there; so do the hooks a destroyed
// binding left.
void test_stale_hooks_do_nothing() {
	Listing first;
	inmatch::NapiNPServerCtx ctx;
	ctx.config.server_name = "Live";
	bool reachable = true;
	inmatch::HostListingBinding binding;
	binding.bind(
			[&ctx, &reachable]() -> inmatch::NapiNPServerCtx * {
				return reachable ? &ctx : nullptr;
			},
			first.role, gametext);
	HostRegistration r;
	reachable = false;
	// No match (a map change's load): the last read stands.
	CHECK(!binding.read_columns(r) && r.server_name.empty());
	binding.unbind();
	CHECK(hooks_installed(ctx));
	ctx.on_update_server_info();
	ctx.on_lan_listing_address(0, 1u, 2u);
	CHECK(ctx.gate_metrics.host().label.empty());
	ctx.gate_metrics.send("ID");
	inmatch::NapiNPServerCtx::PlayerEnterRequest request;
	ctx.on_player_enter_request(request);

	inmatch::NapiNPServerCtx other;
	{
		inmatch::HostListingBinding scoped;
		scoped.bind([&other] { return &other; }, first.role, gametext);
		CHECK(hooks_installed(other));
	}
	other.on_update_server_info();
	other.on_lan_listing_address(0, 1u, 2u);
	CHECK(other.gate_metrics.host().label.empty());
	other.gate_metrics.send("ID");
}

// The PlayerList: every added player at its slot; the host's own connection only with an own
// slot, which takes the match's PlayerTeam / PlayerType.
// [orig: Server_PlayerAdd @0x51CBC0 - the five vars @0x51D441..0x51D4AA;
//  CNapiGameSession_CreateSession @0x4C9B73]
void test_roster() {
	inmatch::NapiNPServerCtx ctx;
	ctx.np_protocol.connection_list.push_back(
			added(inmatch::NapiNPConnection::kTypeClientSide, 0, "Host", 2));
	ctx.np_protocol.connection_list.push_back(
			added(inmatch::NapiNPConnection::kTypeServerSide, 1, "Joiner", 1));
	inmatch::NapiNPConnection validating =
			added(inmatch::NapiNPConnection::kTypeServerSide, 2, "Late", 1);
	validating.phase = inmatch::ConnectionPhase::New;
	ctx.np_protocol.connection_list.push_back(validating);

	const std::vector<HostPlayerSlot> serve_only = inmatch::host_listing_roster(ctx, nullptr);
	CHECK(serve_only.size() == 1);
	CHECK(serve_only.size() == 1 && serve_only[0].slot == 1 &&
			serve_only[0].player_name == "Joiner" && serve_only[0].team == "1" &&
			serve_only[0].type == "3");

	HostPlayerSlot own;
	own.slot = 0;
	own.player_name = "Listen Host";
	own.pcid = "777";
	own.team = "0";
	own.type = "0";
	own.local = true;
	const std::vector<HostPlayerSlot> listen = inmatch::host_listing_roster(ctx, &own);
	CHECK(listen.size() == 2);
	CHECK(listen.size() == 2 && listen[0].slot == 0 && listen[0].player_name == "Listen Host" &&
			listen[0].pcid == "777" && listen[0].local && listen[0].team == "2" &&
			listen[0].type == "3");
}

// The NWU session's facts: the in-use and hosting words, the flags' low byte, and an exit store
// only when the session set one.
void test_match_facts() {
	inmatch::NapiNPServerCtx ctx;
	ctx.mission_exit_reason = 4;
	NwuLobbySession::MatchFacts facts;
	facts.in_use = true;
	facts.flags = 0x1234u;
	facts.role = 6;
	inmatch::set_nwu_match_facts(ctx, facts);
	CHECK(ctx.nwu_in_use && ctx.nwu_session_role == 6 && ctx.nwu_session_flags == 0x34u);
	CHECK(ctx.mission_exit_reason == 4);
	facts.exit_reason = 9;
	inmatch::set_nwu_match_facts(ctx, facts);
	CHECK(ctx.mission_exit_reason == 9);
}

} // namespace

int main() {
	test_bind_and_unbind();
	test_rebind_keeps_the_second();
	test_stale_hooks_do_nothing();
	test_roster();
	test_match_facts();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("host_listing_binding: ok\n");
	return 0;
}
