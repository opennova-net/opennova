// inmatch::HostRole over the mission kernel (ADR 0043 d3), ungated: the
// synthetic mission mission_kernel_test boots, driven through the net half
// every host embedder shares -- the SP listen bring-up (the in-process
// loopback carrying the host's own dcb-2 client, the auto-spawned local
// player, the HostClient replica fold), N listen frames over a null socket
// (the logic clock advances once per frame, the 0x0A fan folds into the local
// ClientState, the viewport seam reaches the ctx), the local C2S gameplay
// drain (reload, mounted-slot, medic and squad requests reach the server
// dispatcher while movement stays queued for Server_TickUpdate), and the dedicated
// bring-up (no loopback client, no local player, no local fold).
#include <runtime/inmatch/host_role.h>
#include <runtime/inmatch/local_role.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/host_session.h>
#include <runtime/inmatch/server_message_dispatch.h>
#include <runtime/inmatch/server_tick.h>
#include <runtime/world/angle.h>
#include <runtime/inmatch/napi_np_connection.h>
#include <base/gameprofile/game_type.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/squad_messages.h>
#include <runtime/mission/mission_kernel.h>

#include <runtime/inmatch/null_datagram_socket.h>

#include "../common/boot_file_source.h"
#include "../common/synthetic_mission.h"

#include <cstdio>
#include <map>
#include <memory>
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

using test_mission::two_entity_mission;
using test_boot::source_over;

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
		kernel.open_document(two_entity_mission(), "synth", source_over(&files));
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
		// The config's unlimited_vehicles word, stock 1, reaches the world:
		// destroyed hulls respawn. [orig: Config_SetDefaults @0x54D352;
		//  Client_BuildMissionDataRequestBlock @0x51E8C5..0x51E8CB]
		CHECK(host.host_owner.ctx.config.unlimited_vehicles);
		CHECK(kernel.world.rules.vehicle_respawns);
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
		// The SP listen server folds a loopback replica, yet the view arbiter's
		// session word stays clear: the SP launch sets network type 0, so
		// retail's is_in_session reads 0 for the whole mission.
		// [orig: SinglePlayer_StartMission -> CNapiNetwork_SetNetworkType(0)
		//  @0x561bce; the +0x58 store @0x4c4a85]
		CHECK(!kernel.world.rules.mp_session);
		CHECK(!kernel.local.view_session_inputs.in_session);
		CHECK(host.host_owner.ctx.loaded_model_viewport_height == 0u);
		if (host.client_runtime) {
			CHECK(host.client_runtime->state().frames_applied > 0);
			CHECK(!host.client_runtime->state().entities.empty());
		}
		role.run_tick(tick_input(768));
		CHECK(host.host_owner.ctx.loaded_model_viewport_height == 768u);
		CHECK(kernel.world.logic_tick == tick0 + 9);

		// The local gameplay drain consumes reloads while preserving movement
		// for Server_TickUpdate's dedicated uplink decoder.
		const size_t s2c_before = host.host_loop.s2c_pending();
		host.host_loop.client_send(c2s::ENTITY_UPLINK, std::vector<uint8_t>{0});
		WeaponReload reload;
		reload.entity_handle = kernel.local.player()->handle.packed;
		reload.reload_param = 0;
		host.host_loop.client_send(c2s::WEAPON_RELOAD_REQUEST, encode_weapon_reload(reload));
		CHECK(host.host_loop.c2s_pending() == 2);
		role.drain_host_client_gameplay_requests();
		CHECK(host.host_loop.c2s_pending() == 1);
		replication::Datagram preserved;
		CHECK(host.host_loop.host_recv(preserved));
		CHECK(preserved.tag == c2s::ENTITY_UPLINK);
		CHECK(!host.host_loop.host_recv(preserved));
		std::printf("host_role: the reload drain staged %zu S2C reply datagram(s)\n",
				host.host_loop.s2c_pending() - s2c_before);
		// A drain with nothing queued is a no-op.
		role.drain_host_client_gameplay_requests();
		CHECK(host.host_loop.c2s_pending() == 0);
		// The host's own medic call (action 217 -> C2S 0x2E) rides the same
		// queue, sent by its client frame, and must reach the dispatcher, not
		// the movement-only drain. [orig: Input_HandleActionBinding @0x49B4B4..0x49B50C]
		CHECK(role.send_medic_request());
		CHECK(host.host_loop.c2s_pending() == 0);
		host.client_runtime->Client_ProcessNetworkFrame();
		CHECK(host.host_loop.c2s_pending() == 1);
		role.drain_host_client_gameplay_requests();
		CHECK(host.host_loop.c2s_pending() == 0);
		// The host player's command-map sends reach its own server's squad
		// handlers the same way: a C2S 0x45 naming its own slot lands the
		// fireteam there. [orig: NetPacket_SendWeaponAction @0x42dceb (0x45)
		// -> CNapiNetwork_QueueReliableMessage @0x4c4fa0;
		// NapiNPServerMsg_0x045_HandleTeamAssignment @0x510c00]
		inmatch::NapiNPConnection *self = nullptr;
		for (inmatch::NapiNPConnection &conn : host.host_owner.ctx.np_protocol.connection_list)
			if (conn.type == 2 && conn.link.transport == &host.host_loop) self = &conn;
		CHECK(self != nullptr);
		if (self != nullptr) {
			FireteamAssign assign;
			assign.fireteam = 2;
			assign.members = {self->reply.player_slot};
			host.host_loop.client_send(c2s::FIRETEAM_ASSIGN, encode_fireteam_assign(assign));
			role.drain_host_client_gameplay_requests();
			CHECK(host.host_loop.c2s_pending() == 0);
			CHECK(self->fireteam == 2);
		}
		// The next frame's Server_TickUpdate drains what the local drain left.
		host.host_loop.client_send(c2s::ENTITY_UPLINK, std::vector<uint8_t>{0});
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

	// Action 6 from the host's own player must traverse the same dispatcher
	// as a remote C2S 0x16, before the movement-only drain can discard it.
	// [orig: Input_HandleActionBinding_0 @ 0x4E0420 (action 6 @ 0x4E0492);
	// NapiNPServerMsg_HandleWeaponToggle @ 0x511A70]
	{
		auto kernel_storage = std::make_unique<ms::MissionKernel>();
		auto &kernel = *kernel_storage;
		auto role_storage = std::make_unique<inmatch::HostRole>();
		auto &role = *role_storage;
		role.bind(kernel);
		kernel.open_document(two_entity_mission(), "tank_slots", source_over(&files));
		ms::KernelBootOptions options;
		options.game_type = mission_game_type(kernel.mission);
		options.bringup_net_session = [&] { role.bring_up_singleplayer(); };
		std::string error;
		CHECK(kernel.boot(options, error));
		for (int i = 0; i < 8; ++i) role.run_tick(tick_input(0));

		auto &world = kernel.world;
		const auto add_weapon = [&](const char *name, int clip, int rounds) {
			w::WeaponTableEntry def;
			def.name = name;
			def.valid = true;
			def.category = 11;
			def.clipsize = clip;
			def.startrounds = rounds;
			world.tables.weapons.entries.push_back(def);
		};
		add_weapon("WPN_TANK_CANNON", 1, 40);
		add_weapon("WPN_TANK_COAX", 200, 600);
		w::Entity hull;
		hull.kind = w::EntityKind::Item;
		hull.has_item_def = true;
		hull.item_type = 1;
		hull.item_attrib = w::kItemAttribEweap;
		hull.primary_weapon = "WPN_TANK_COAX";
		const auto hull_handle = world.registry.spawn(1, hull);
		CHECK(hull_handle.valid());
		if (!hull_handle.valid()) return 1;
		w::Entity gun;
		gun.kind = w::EntityKind::Item;
		gun.has_item_def = true;
		gun.item_type = 6;
		gun.item_attrib = w::kItemAttribEweap;
		gun.primary_weapon = "WPN_TANK_CANNON";
		gun.emplacement_attachment_flags = 2;
		gun.emplacement_parent = hull_handle;
		gun.emplacement_parent_spawn_id = world.registry.get(hull_handle)->registry_spawn_id;
		gun.ground_target = hull_handle;
		w::Seat seat;
		seat.type = w::SeatType::Gunner;
		seat.bone_index = 1;
		seat.retail_slot = 9;
		gun.seats.push_back(seat);
		const auto gun_handle = world.registry.spawn(1, gun);
		CHECK(gun_handle.valid());
		if (!gun_handle.valid()) return 1;
		CHECK(world.vehicles.process_attach(world.cached.local_player, gun_handle, 1));
		auto &live_gun = *world.registry.get(gun_handle);
		auto &live_hull = *world.registry.get(hull_handle);
		CHECK(world.vehicles.prepare_weapon_slot(live_hull));
		live_gun.primary_weapon_slot.clip = 0;
		live_gun.primary_weapon_slot.reserve = 37;
		live_hull.primary_weapon_slot.clip = 123;
		live_hull.primary_weapon_slot.reserve = 321;
		for (bool parent : {true, false, true, false}) {
			MountedWeaponSlotSelection request;
			request.use_parent_slot = parent;
			role.state.host_loop.client_send(c2s::MOUNTED_WEAPON_SLOT_SELECT,
					encode_mounted_weapon_slot_selection(request));
			role.drain_host_client_gameplay_requests();
			CHECK(role.state.host_loop.c2s_pending() == 0);
			CHECK(live_gun.primary_weapon_slot.redirect_to_parent_slot == parent);
			const auto *selected = world.vehicles.resolve_mounted_ammo_slot(live_gun);
			CHECK(selected == (parent ? &live_hull.primary_weapon_slot : &live_gun.primary_weapon_slot));
			CHECK(kernel.local.player()->equipped_adm_index ==
					(parent ? live_hull.primary_weapon_slot_adm : live_gun.primary_weapon_slot_adm));
			CHECK(live_gun.primary_weapon_slot.clip == 0 && live_gun.primary_weapon_slot.reserve == 37);
			CHECK(live_hull.primary_weapon_slot.clip == 123 && live_hull.primary_weapon_slot.reserve == 321);
		}
	}

	// --- the dedicated (HostOnly) bring-up -----------------------------------
	{
		ms::MissionKernel kernel;
		inmatch::HostRole role;
		role.bind(kernel);
		inmatch::ListenHostState &host = role.state;
		kernel.open_document(two_entity_mission(), "synth", source_over(&files));
		ms::KernelBootOptions options;
		options.playable = false;
        options.mp_session = true; // a dedicated host has no player of its own
		options.game_type = mission_game_type(kernel.mission);
		options.bringup_net_session = [&] {
			inmatch::HostConfig cfg;
			cfg.config.server_name = "listen_host_test";
			cfg.config.max_players = 4;
			cfg.config.game_type = options.game_type;
			cfg.config.unlimited_vehicles = false; // game.cfg unlimited_vehicles = 0
			cfg.socket_mode = inmatch::SocketMode::Lan;
			cfg.serve_and_play = false; // SERVERTYPE serve-only: no local player
			inmatch::HostBringup bringup;
			bringup.host_cfg = cfg;
			role.bring_up(bringup);
		};
		std::string error;
		CHECK(kernel.boot(options, error));
		CHECK(error.empty());

		// A host config without unlimited vehicles removes destroyed hulls.
		CHECK(!host.host_owner.ctx.config.unlimited_vehicles);
		CHECK(!kernel.world.rules.vehicle_respawns);
		CHECK(!host.host_owner.serve_and_play);
		// The loopback channel exists but no local client registers on it.
		CHECK(host.host_owner.host_loopback == &host.host_loop);
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
		// A network host is in the session (the host launch passes type 1..3).
		// [orig: UI_HandleHostSessionStart -> CNapiNetwork_SetNetworkType(2)
		//  @0x556e83; the +0x58 store @0x4c4a85]
		CHECK(kernel.local.view_session_inputs.in_session);
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
        kernel.open_document(two_entity_mission(), "respawn", source_over(&files));
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

	// --- the frame's weapon-action walk pumps the NPC gunners ----------------
	// Every role frame runs the walk after its weather and view legs, so an NPC
	// gunner's queued FIRE on its emplacement leaves as a round within the
	// frame: on the listen host, on a dedicated host (no local player) and in
	// the bare local role alike. The entity pass itself no longer pumps.
	// [orig: Game_ProcessMainFrame -- Camera_ComputeThirdPersonView @0x526781,
	//  then the WeaponAction_ProcessAllEntities call @0x526786;
	//  WeaponAction_ProcessAllEntities @0x5426A6..0x5426C9]
	for (const int frame_kind : {0, 1, 2}) {
		auto kernel_storage = std::make_unique<ms::MissionKernel>();
		auto &kernel = *kernel_storage;
		auto host_storage = std::make_unique<inmatch::HostRole>();
		auto local_storage = std::make_unique<inmatch::LocalRole>();
		inmatch::Role &role = frame_kind == 2
				? static_cast<inmatch::Role &>(*local_storage)
				: static_cast<inmatch::Role &>(*host_storage);
		role.bind(kernel);
		kernel.open_document(two_entity_mission(), "gunner_walk", source_over(&files));
		ms::KernelBootOptions options;
		options.game_type = mission_game_type(kernel.mission);
		if (frame_kind == 0) {
			options.bringup_net_session = [&] { host_storage->bring_up_singleplayer(); };
		} else if (frame_kind == 1) {
			options.playable = false;
			options.mp_session = true;
			options.bringup_net_session = [&] {
				inmatch::HostConfig cfg;
				cfg.config.server_name = "gunner_walk";
				cfg.config.max_players = 4;
				cfg.config.game_type = options.game_type;
				cfg.socket_mode = inmatch::SocketMode::Lan;
				inmatch::HostBringup bringup;
				bringup.host_cfg = cfg;
				host_storage->bring_up(bringup);
			};
		}
		std::string error;
		CHECK(kernel.boot(options, error));
		auto &world = kernel.world;
		w::EntityHandle npc;
		for (size_t slot = 0; slot < world.registry.pool_capacity(0) && !npc.valid(); ++slot) {
			const w::Entity *row =
					world.registry.get(w::EntityHandle::make(0, static_cast<int>(slot)));
			if (row != nullptr && row->handle != world.cached.local_player &&
					world.ai.for_handle(row->handle) != nullptr)
				npc = row->handle;
		}
		CHECK(npc.valid());
		if (!npc.valid()) return 1;
		world.tables.ammo.entries.resize(2);
		world.tables.ammo.entries[1].velocity = 800;
		world.tables.ammo.entries[1].max_age_ticks = 124;
		world.tables.ammo.entries[1].min_damage = 10;
		world.tables.ammo.entries[1].max_damage = 40;
		world.tables.ammo.entries[1].valid = true;
		w::WeaponTableEntry def;
		def.name = "WPN_WALK_TEST";
		def.valid = true;
		def.ammo_index = 1;
		def.clipsize = -1;
		def.action_fsm.clip_capacity = -1;
		for (int action = 0; action < w::weapon_action::kCount; ++action)
			def.action_fsm.actions[action].id = action;
		world.tables.weapons.entries.push_back(def);
		w::Entity gun;
		gun.kind = w::EntityKind::Item;
		gun.has_item_def = true;
		gun.item_type = 6;
		gun.item_attrib = w::kItemAttribEweap;
		gun.primary_weapon = "WPN_WALK_TEST";
		w::Seat seat;
		seat.type = w::SeatType::Gunner;
		seat.bone_index = 1;
		gun.seats.push_back(seat);
		const auto gun_handle = world.registry.spawn(1, gun);
		CHECK(gun_handle.valid());
		if (!gun_handle.valid()) return 1;
		CHECK(world.vehicles.process_attach(npc, gun_handle, 1));
		w::Entity &live_gun = *world.registry.get(gun_handle);
		CHECK(live_gun.primary_weapon_owner == npc);
		live_gun.primary_weapon_slot.next = w::weapon_action::kFire;
		const int rounds_before = world.out.rounds.count;
		role.run_tick(tick_input(0));
		CHECK(live_gun.primary_weapon_slot.current == w::weapon_action::kFire);
		CHECK(world.out.rounds.count == rounds_before + 1);
		const int32_t last = (world.out.rounds.cursor + w::RoundRing::kCapacity - 1) %
				w::RoundRing::kCapacity;
		CHECK(world.out.rounds.records[static_cast<size_t>(last)].shooter_handle == npc.packed);
	}

	// --- the authority-local fire gate on an MP listen host ------------------
	// The host's own client takes its looped-back tick seed, and its own shot
	// reaches the server only while its own slot is active against that clock:
	// refused on the frame the seed lands (tick == floor), then admitted; a
	// disarm opens the 3-period grace, then refuses; a zero seed parks the
	// clock and refuses. Single player has no gate.
	// [orig: NapiNPClientMsg_HandleSessionKey @0x4297c0;
	//  Entity_FireWeaponAndSendPacket @0x42be12..0x42be44 -> PlayerSlot_IsActive
	//  @0x4FC760; Server_SendRandomSeedToPlayer @0x5101A0]
	{
		ms::MissionKernel kernel;
		inmatch::HostRole role;
		role.bind(kernel);
		inmatch::ListenHostState &host = role.state;
		kernel.open_document(two_entity_mission(), "fire_gate", source_over(&files));
		ms::KernelBootOptions options;
		options.mp_session = true;
		options.game_type = mission_game_type(kernel.mission);
		options.bringup_net_session = [&] {
			inmatch::HostConfig cfg;
			cfg.config.server_name = "fire_gate";
			cfg.config.max_players = 4;
			cfg.config.game_type = options.game_type;
			cfg.socket_mode = inmatch::SocketMode::Lan;
			cfg.serve_and_play = true; // a listen host: HostClient + its own player
			inmatch::HostBringup bringup;
			bringup.host_cfg = cfg;
			role.bring_up(bringup);
		};
		std::string error;
		CHECK(kernel.boot(options, error));
		CHECK(host.host_owner.serve_and_play && host.client_runtime != nullptr);
		CHECK(kernel.local.has_local_player());
		opennova::inmatch::NullDatagramSocket socket;
		role.set_socket(&socket);
		inmatch::NapiNPConnection *own = nullptr;
		for (inmatch::NapiNPConnection &conn : host.host_owner.ctx.np_protocol.connection_list)
			if (conn.link.owned_entity == kernel.world.cached.local_player) own = &conn;
		CHECK(own != nullptr);
		if (own != nullptr && host.client_runtime != nullptr) {
			CHECK(own->fire_tick_mode && own->tick_seed != 0);
			// The seed rode the loopback to the host's own client.
			role.run_tick(tick_input(0));
			CHECK(host.client_runtime->current_tick() != 0);
			for (int i = 0; i < 3; ++i) role.run_tick(tick_input(0));
			CHECK(host.client_runtime->current_tick() > own->fire_tick_floor);
			CHECK(role.local_fire_admitted());
			CHECK(kernel.local.authority_fire_admitted);
			// The tick on the floor is refused.
			const uint32_t floor = own->fire_tick_floor;
			own->fire_tick_floor = host.client_runtime->current_tick();
			CHECK(!role.local_fire_admitted());
			own->fire_tick_floor = floor;
			// A disarm: the grace admits, then refuses past three periods.
			(void)inmatch::Server_DisarmPlayerTickSeed(*own, kernel.world.logic_tick);
			CHECK(role.local_fire_admitted());
			const uint32_t holdoff = host.host_owner.ctx.config.effective_send_holdoff_ticks();
			kernel.world.logic_tick += 3 * holdoff;
			CHECK(!role.local_fire_admitted());
			role.run_tick(tick_input(0));
			CHECK(!kernel.local.authority_fire_admitted);
			// A re-arm admits again; the disarm's zero seed then parks the
			// host client's clock and refuses.
			(void)inmatch::Server_RerollPlayerTickSeed(*own);
			own->fire_tick_floor = 1;
			CHECK(role.local_fire_admitted());
			host.host_loop.host_send(s2c::TICK_SEED, {0, 0, 0, 0});
			role.run_tick(tick_input(0));
			CHECK(host.client_runtime->current_tick() == 0);
			CHECK(!role.local_fire_admitted());
		}
	}

	// --- the authority's own stance rides its looped-back C2S 0x1D ----------
	// The server handler latches the host's own player from the 0x1D its own
	// client sent: the datagram the press frame's client frame flushed is read
	// at the next frame's head, so the stance lands in that frame's server
	// tick, ahead of its body update.
	// [orig: Game_ProcessMainFrame -- CNapiNetwork_PumpManagerReceive @0x526528,
	//  Client_ProcessNetworkFrame @0x526692, Server_TickUpdate @0x5266b6 ->
	//  CNapiNetwork_PumpServerProtocolRecv @0x51d895;
	//  NapiNPServerMsg_HandleStanceChange @0x501c60, latches @0x501d1b / @0x501d2d]
	{
		ms::MissionKernel kernel;
		inmatch::HostRole role;
		role.bind(kernel);
		inmatch::ListenHostState &host = role.state;
		kernel.open_document(two_entity_mission(), "stance", source_over(&files));
		ms::KernelBootOptions options;
		options.game_type = mission_game_type(kernel.mission);
		options.bringup_net_session = [&] { role.bring_up_singleplayer(); };
		std::string error;
		CHECK(kernel.boot(options, error));
		opennova::inmatch::NullDatagramSocket socket;
		role.set_socket(&socket);
		for (int i = 0; i < 4; ++i) role.run_tick(tick_input(0));
		auto &lp = kernel.local;
		const auto body_stance = [&] {
			const w::AiEntity *body = lp.player_ai();
			return body != nullptr ? body->inf.stance : w::InfantryState::Stance::kStand;
		};
		CHECK(lp.has_local_player() && lp.stance_latch() == 0);
		// A looped-back crouch (action 169) reaches the dispatcher and latches.
		host.host_loop.client_send(c2s::STANCE_CHANGE, std::vector<uint8_t>{0xA9, 0x00});
		role.run_tick(tick_input(0));
		CHECK(lp.stance_latch() == 1 && lp.input.crouch && !lp.input.prone);
		CHECK(body_stance() == w::InfantryState::Stance::kCrouch);
		CHECK(lp.player() != nullptr && lp.player()->net_stance_bits == 2);
		// The key only queues the 0x1D (action 170, prone) on the host's own
		// client: the press frame's body stays crouched, its client frame
		// flushes the datagram, and the next frame's server tick latches it.
		// [orig: Input_HandleActionBinding_0 case 170 @0x4e0df3 ->
		//  CNapiNetwork_QueueReliableMessage, no latch write]
		CHECK(role.request_stance(2));
		CHECK(lp.stance_latch() == 1);
		CHECK(host.host_loop.c2s_pending() == 0);
		role.run_tick(tick_input(0));
		CHECK(lp.stance_latch() == 1 && body_stance() == w::InfantryState::Stance::kCrouch);
		CHECK(host.host_loop.c2s_pending() == 1);
		role.run_tick(tick_input(0));
		CHECK(lp.stance_latch() == 2 && lp.input.prone && !lp.input.crouch);
		CHECK(body_stance() == w::InfantryState::Stance::kProne);
		CHECK(host.host_loop.c2s_pending() == 0);
		// Every press sends, the selected stance included; the stand (172)
		// then clears both bits the same way.
		CHECK(role.request_stance(2));
		role.run_tick(tick_input(0));
		role.run_tick(tick_input(0));
		CHECK(lp.stance_latch() == 2);
		CHECK(role.request_stance(0));
		role.run_tick(tick_input(0));
		CHECK(lp.stance_latch() == 2);
		role.run_tick(tick_input(0));
		CHECK(lp.stance_latch() == 0 && !lp.input.prone && !lp.input.crouch);
		CHECK(body_stance() == w::InfantryState::Stance::kStand);
		// The host's chat line, emote and radio call take the same path as
		// its 0x1D: queued on its own client connection at the press, sent by
		// that frame's client frame, dispatched by the next frame's server
		// tick. [orig: Chat_SendTeamMessage QueueReliableMessage(0xD) @0x49a9b4;
		//  NetPacket_SendEmoteRequest @0x42c147 (0x14); NetPacket_SendRadioCallRequest
		//  @0x42c177 (0x13); the manager FIFO drained @0x526528]
		inmatch::ClientRuntime &own = *host.client_runtime;
		std::string line = "on me";
		CHECK(own.queue_chat_message(1, line, 0) == hud::ChatSendResult::Sent);
		CHECK(own.queue_voice_menu_pick(c2s::EMOTE_REQUEST, 3));
		CHECK(own.queue_voice_menu_pick(c2s::RADIO_CALL_REQUEST, 5));
		CHECK(host.host_loop.c2s_pending() == 0);
		role.run_tick(tick_input(0));
		CHECK(host.host_loop.c2s_pending() == 3);
		role.run_tick(tick_input(0));
		CHECK(host.host_loop.c2s_pending() == 0);
		// So do its medic call, its team change and its command-map squad
		// sends: each sender queues on the local connection with no authority
		// branch. [orig: action 217 -> QueueReliableMessage(0x2E) @0x49b50c;
		//  DeathScreen_OnSwapTeams @0x5535ba -> 0x4D @0x42ddac; the squad
		//  senders 0x17 @0x42ddf2, 0x43 @0x42dc37, 0x44 @0x42dcac, 0x45 @0x42dcf7,
		//  0x46 @0x42dd2f, 0x4B @0x42dd7f, 0x4F @0x42de2d, 0x3F @0x5488b9]
		CHECK(role.send_medic_request());
		CHECK(own.queue_team_change_request());
		FireteamAssign assign;
		assign.fireteam = 1;
		CHECK(own.queue_squad_message(c2s::FIRETEAM_ASSIGN, encode_fireteam_assign(assign)));
		CHECK(host.host_loop.c2s_pending() == 0);
		role.run_tick(tick_input(0));
		CHECK(host.host_loop.c2s_pending() == 3);
		role.run_tick(tick_input(0));
		// Dispatched; what remains is the handlers' own follow-up traffic.
		replication::Datagram left;
		while (host.host_loop.host_recv(left))
			CHECK(left.tag != c2s::MEDIC_REQUEST && left.tag != c2s::TEAM_CHANGE_REQUEST &&
					left.tag != c2s::FIRETEAM_ASSIGN);
	}

	if (failures == 0) std::printf("host_role: all checks passed\n");
	return failures == 0 ? 0 : 1;
}
