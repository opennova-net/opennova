// inmatch::HostRole over the mission kernel (ADR 0043 d3), ungated: the
// synthetic mission mission_kernel_test boots, driven through the net half
// every host embedder shares -- the SP listen bring-up (the in-process
// loopback carrying the host's own dcb-2 client, the auto-spawned local
// player, the HostClient replica fold), N listen frames over a null socket
// (the logic clock advances once per frame, the 0x0A fan folds into the local
// ClientState, the viewport seam reaches the ctx), the local C2S gameplay
// drain (a reload request is consumed by the server dispatcher while every
// other datagram stays queued for Server_TickUpdate), and the dedicated
// bring-up (no loopback client, no local player, no local fold).
#include <runtime/inmatch/host_role.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/host_session.h>
#include <runtime/inmatch/server_message_dispatch.h>
#include <runtime/world/angle.h>
#include <runtime/inmatch/napi_np_connection.h>
#include <base/gameprofile/game_type.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <runtime/mission/mission_kernel.h>

#include <runtime/inmatch/null_datagram_socket.h>

#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace opennova;
namespace ms = opennova::mission;
namespace w = opennova::world;

static int failures = 0;
#define CHECK(c)                                                                            \
	do {                                                                                    \
		if (!(c)) {                                                                         \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                        \
			++failures;                                                                     \
		}                                                                                   \
	} while (0)

namespace {

bms::Entity organic(int32_t x, int32_t y, int32_t z, uint8_t team) {
	bms::Entity e{};
	e.type = bms::ItemType::Organic;
	e.x = x;
	e.y = y;
	e.z = z;
	e.yaw = 90;
	e.team = team;
	return e;
}

bms::Entity item(int32_t type_id, int32_t x, int32_t y, int32_t z) {
	bms::Entity e{};
	e.type = bms::ItemType::Item;
	e.type_id = type_id;
	e.x = x;
	e.y = y;
	e.z = z;
	return e;
}

ms::BootFileSource source_over(const std::map<std::string, std::string> *files) {
	ms::BootFileSource s;
	s.has_file = [files](const std::string &name) {
		return files->find(name) != files->end();
	};
	s.read_file = [files](const std::string &name, std::vector<uint8_t> &out) {
		const auto it = files->find(name);
		if (it == files->end()) return false;
		out.assign(it->second.begin(), it->second.end());
		return true;
	};
	return s;
}

// The synthetic mission mission_kernel_test boots: two placed entities and
// one (empty) BMS event.
bms::File synthetic_mission() {
	bms::File m{};
	m.items.push_back(item(/*type_id=*/164, 10 << 16, 20 << 16, 3 << 16));
	m.items[0].id = 21;
	m.organics.push_back(organic(1 << 16, 1 << 16, 0, /*team=*/1));
	m.organics[0].id = 31;
	m.events.push_back(bms::Event{});
	return m;
}

uint32_t mission_game_type(const bms::File &mission) {
	return game_type::for_mission_mode(bms::selected_game_mode(
			static_cast<bms::AttribFlags>(mission.header.attrib_flags)));
}

int local_loopback_connections(const inmatch::ListenHostState &state) {
	int count = 0;
	for (const inmatch::NapiNPConnection &conn : state.host_owner.ctx.np_protocol.connection_list)
		if (conn.type == 2 && conn.link.transport == &state.host_loop) ++count;
	return count;
}

// One tick's input with the renderer viewport height a listen host wraps its
// S2C 0x68 cursor against.
inmatch::TickInput tick_input(int32_t viewport_height) {
	inmatch::TickInput in;
	in.viewport_height = viewport_height;
	return in;
}

} // namespace

int main() {
	std::map<std::string, std::string> files;

	// --- the SP listen bring-up and the per-tick frame ----------------------
	{
		ms::MissionKernel kernel;
		inmatch::HostRole role;
		role.bind(kernel);
		inmatch::ListenHostState &host = role.state;
		kernel.open_document(synthetic_mission(), "synth", source_over(&files));
		ms::KernelBootOptions options;
		options.game_type = mission_game_type(kernel.mission);
		options.bringup_net_session = [&] { role.bring_up_singleplayer(); };
		std::string error;
		CHECK(kernel.boot(options, error));
		CHECK(error.empty());

		// The bring-up residue: the np host owner over the kernel's world and
		// mission, serve-and-play over the loopback, the SINGLEPLAYERGAME
		// config, the HostClient runtime folding that loopback, and exactly
		// one dcb-2 connection (the host's own client) whose transport is the
		// loopback. start_host_session spawned the host's own player, so the
		// kernel's own spawn step found one already.
		CHECK(host.host_owner.serve_and_play);
		CHECK(host.host_owner.host_loopback == &host.host_loop);
		CHECK(host.host_owner.ctx.world == &kernel.world);
		CHECK(host.host_owner.ctx.mission == &kernel.mission);
		CHECK(host.host_owner.ctx.config.server_name == "SINGLEPLAYERGAME");
		CHECK(host.host_owner.ctx.config.max_players == 1u);
		// The SP launcher's literal attribute word, not the cfg default 0x3A02
		// [orig: SinglePlayer_StartMission @0x561bb7 -> @0x561cdb].
		CHECK(host.host_owner.ctx.config.mp_attributes == 0x3A06u);
		CHECK(host.host_owner.ctx.config.game_type == options.game_type);
		CHECK(host.client_runtime != nullptr);
		if (host.client_runtime) {
			CHECK(host.client_runtime->role() == inmatch::ClientRuntime::Role::HostClient);
			CHECK(host.client_runtime->is_authority());
		}
		CHECK(local_loopback_connections(host) == 1);
		CHECK(kernel.local.has_local_player());
		CHECK(kernel.local.player_ai() != nullptr);

		// N listen frames over the null wire: one logic tick and one owner
		// tick per frame; the per-tick 0x0A fan reaches the local ClientState
		// through the loopback fold; a headless embedder leaves the viewport
		// seam at 0 and a windowed one stamps its height.
		opennova::inmatch::NullDatagramSocket socket;
		role.set_socket(&socket);
		const uint32_t tick0 = kernel.world.logic_tick;
		const uint32_t now0 = host.host_owner.now_tick;
		// Pin the single-owner invariant at one frame, then across a short run.
		role.run_tick(tick_input(0));
		CHECK(kernel.world.logic_tick == tick0 + 1);
		CHECK(host.host_owner.now_tick == now0 + 1);
		for (int i = 0; i < 7; ++i)
			role.run_tick(tick_input(0));
		CHECK(kernel.world.logic_tick == tick0 + 8);
		CHECK(host.host_owner.now_tick == now0 + 8);
		CHECK(host.host_owner.ctx.loaded_model_viewport_height == 0u);
		if (host.client_runtime) {
			CHECK(host.client_runtime->state().frames_applied > 0);
			CHECK(!host.client_runtime->state().entities.empty());
		}
		role.run_tick(tick_input(768));
		CHECK(host.host_owner.ctx.loaded_model_viewport_height == 768u);
		CHECK(kernel.world.logic_tick == tick0 + 9);

		// The local C2S gameplay drain: only the witnessed local reload
		// producer reaches the per-message server dispatcher here; every
		// other datagram is preserved in order for Server_TickUpdate's drain.
		const size_t s2c_before = host.host_loop.s2c_pending();
		host.host_loop.client_send(c2s::MOUNTED_WEAPON_SLOT_SELECT, std::vector<uint8_t>{0});
		WeaponReload reload;
		reload.entity_handle = kernel.local.player()->handle.packed;
		reload.reload_param = 0;
		host.host_loop.client_send(c2s::WEAPON_RELOAD_REQUEST, encode_weapon_reload(reload));
		CHECK(host.host_loop.c2s_pending() == 2);
		role.drain_host_client_gameplay_requests();
		CHECK(host.host_loop.c2s_pending() == 1);
		replication::Datagram preserved;
		CHECK(host.host_loop.host_recv(preserved));
		CHECK(preserved.tag == c2s::MOUNTED_WEAPON_SLOT_SELECT);
		CHECK(!host.host_loop.host_recv(preserved));
		std::printf("host_role: the reload drain staged %zu S2C reply datagram(s)\n",
				host.host_loop.s2c_pending() - s2c_before);
		// A drain with nothing queued is a no-op.
		role.drain_host_client_gameplay_requests();
		CHECK(host.host_loop.c2s_pending() == 0);
		// The next frame's Server_TickUpdate drains what the local drain left.
		host.host_loop.client_send(c2s::MOUNTED_WEAPON_SLOT_SELECT, std::vector<uint8_t>{0});
		role.run_tick(tick_input(0));
		CHECK(host.host_loop.c2s_pending() == 0);
		// Recreate the local replica and repeat an equal point award. Both the
		// sender's comparison cache and the client's score epoch must restart.
		const w::EntityHandle scorer = kernel.world.cached.local_player;
		CHECK(kernel.world.match.player(scorer) != nullptr);
		kernel.capture_baseline();
		for (int attempt = 0; attempt < 2; ++attempt) {
			kernel.world.match.player(scorer)->stats[w::MatchStats::kPoints] = 17;
			for (int tick = 0; tick < 62; ++tick) role.run_tick(tick_input(0));
			const auto &feedback = host.client_runtime->state().score_feedback;
			CHECK(feedback.updates == 1);
			CHECK(feedback.score == 17 && feedback.delta == 17);
			inmatch::SessionError reset_error;
			CHECK(role.reset_to_baseline(reset_error));
			CHECK(kernel.world.match.player(scorer)->stats[w::MatchStats::kPoints] == 0);
			CHECK(host.client_runtime->state().score_feedback.updates == 0);
		}
	}

	// --- the dedicated (HostOnly) bring-up -----------------------------------
	{
		ms::MissionKernel kernel;
		inmatch::HostRole role;
		role.bind(kernel);
		inmatch::ListenHostState &host = role.state;
		kernel.open_document(synthetic_mission(), "synth", source_over(&files));
		ms::KernelBootOptions options;
		options.playable = false;
        options.mp_session = true; // a dedicated host has no player of its own
		options.game_type = mission_game_type(kernel.mission);
		options.bringup_net_session = [&] {
			inmatch::HostConfig cfg;
			cfg.config.server_name = "listen_host_test";
			cfg.config.max_players = 4;
			cfg.config.game_type = options.game_type;
			cfg.socket_mode = inmatch::SocketMode::Lan;
			cfg.serve_and_play = true; // the dedicated bring-up forces this OFF
			role.bring_up_dedicated(cfg);
		};
		std::string error;
		CHECK(kernel.boot(options, error));
		CHECK(error.empty());

		CHECK(!host.host_owner.serve_and_play);
		CHECK(host.host_owner.host_loopback == nullptr);
		CHECK(host.client_runtime == nullptr);
		CHECK(host.host_owner.ctx.world == &kernel.world);
		CHECK(host.host_owner.ctx.mission == &kernel.mission);
		CHECK(host.host_owner.ctx.config.server_name == "listen_host_test");
		CHECK(host.host_owner.ctx.config.max_players == 4u);
		CHECK(local_loopback_connections(host) == 0);
		CHECK(host.host_owner.ctx.np_protocol.connection_list.empty());
		CHECK(!kernel.local.has_local_player());

		// The same frame drives a dedicated host: the logic clock and the
		// owner tick advance together, and the drain has no local connection
		// to serve.
		opennova::inmatch::NullDatagramSocket socket;
		const uint32_t tick0 = kernel.world.logic_tick;
		const uint32_t now0 = host.host_owner.now_tick;
		for (int i = 0; i < 4; ++i)
			role.run_tick(tick_input(0));
		CHECK(kernel.world.logic_tick == tick0 + 4);
		CHECK(host.host_owner.now_tick == now0 + 4);
		role.drain_host_client_gameplay_requests();
		CHECK(host.host_loop.c2s_pending() == 0);
		CHECK(!kernel.local.has_local_player());
	}

    // The shared deployment transaction resets local view state immediately,
    // even when the host never presents an intervening dead frame.
    {
        ms::MissionKernel kernel;
        inmatch::HostRole role;
        role.bind(kernel);
        kernel.open_document(synthetic_mission(), "respawn", source_over(&files));
        ms::KernelBootOptions options;
        options.game_type = mission_game_type(kernel.mission);
        options.bringup_net_session = [&] { role.bring_up_singleplayer(); };
        std::string error;
        CHECK(kernel.boot(options, error));
        auto &lp = kernel.local;
        auto &owner = role.state.host_owner;
        auto &conn = owner.ctx.np_protocol.connection_list.front();
        CHECK(lp.request_stance(2));
        lp.input.look_pitch = 12345;
        lp.input.forward = true;
        lp.view.binoculars_requested = lp.view.binoculars_raised = true;
        lp.view.binoculars_view_active = true;
        lp.view.scope_engaged = true;
        lp.view.scope_settled = true;
        lp.view.nvg_active = true;
        lp.view.nvg_gain = 7;
        lp.view.shake = {30, 111, 222, 333};
        lp.view.camera_mode = 4;
        lp.view.lookahead_q16[0] = 700;
        lp.hud_map_control.mode = 3;
        lp.hud_map_control.zoom_q16 = 32768;
        lp.hud_map_control.big_zoom_q16 = 262144;
        lp.weapon.power_throw_start_tick = 99;
        kernel.world.weather.core.hit_dim.arm(true, false);
        kernel.world.script.waypoints.current = 3;
        role.state.client_runtime->state().local_medic_reviving = true;
        const auto revision = lp.round_reset_revision;
        const auto resets = kernel.world.out.effects.count("local_round_reset");
        lp.player()->alive = false;
        lp.player()->health = 0;
        lp.player()->flags |= w::kEntityFlagDead;
        const auto replies = inmatch::Server_ReleasePlayerDeployment(
                owner.ctx.config, conn, kernel.world, {});
        CHECK(!replies.empty());
        CHECK(lp.player()->alive && lp.player()->health > 0);
        CHECK(lp.round_reset_revision == revision + 1);
        CHECK(kernel.world.out.effects.count("local_round_reset") == resets + 1);
        CHECK(!lp.view.binoculars_requested && !lp.view.binoculars_raised &&
                !lp.view.binoculars_view_active && !lp.view.scope_engaged &&
                !lp.view.scope_settled);
        CHECK(lp.stance_latch() == 0 && !lp.input.prone && !lp.input.crouch);
        CHECK(lp.input.look_heading == w::bam_heading_from_mission_yaw_deg(lp.player()->yaw));
        CHECK(lp.input.look_pitch == 12345 && lp.input.forward);
        CHECK(lp.view.shake.counter == 0 && lp.view.shake.roll == 111 &&
                lp.view.shake.pitch == 222 && lp.view.shake.yaw == 333);
        CHECK(lp.view.nvg_active && lp.view.nvg_gain == 7);
        CHECK(lp.view.camera_mode == 0 && lp.view.tp_anchor_valid && lp.view.lookahead_q16[0] == 0);
        CHECK(lp.hud_map_control.mode == 0 && lp.hud_map_control.zoom_q16 == 32768 &&
                lp.hud_map_control.big_zoom_q16 == 262144);
        CHECK(lp.weapon.power_throw_start_tick == 0);
        CHECK(kernel.world.weather.core.hit_dim.intensity == 0 &&
                kernel.world.weather.core.hit_dim.fade_rate == 0);
        CHECK(kernel.world.script.waypoints.current == -1);
        role.run_tick(tick_input(0));
        CHECK(!role.state.client_runtime->state().local_medic_reviving);
        CHECK(lp.round_reset_revision == revision + 1);

        // A remote player's deployment cannot reset the local camera or map.
        inmatch::NapiNPConnection remote;
        remote.link.owned_entity = w::EntityHandle::make(0, 0);
        CHECK(remote.link.owned_entity != kernel.world.cached.local_player);
        lp.hud_map_control.mode = 2;
        inmatch::Server_ReleasePlayerDeployment(owner.ctx.config, remote, kernel.world, {});
        CHECK(lp.hud_map_control.mode == 2 && lp.round_reset_revision == revision + 1);
    }

	if (failures == 0) std::printf("host_role: all checks passed\n");
	return failures == 0 ? 0 : 1;
}
