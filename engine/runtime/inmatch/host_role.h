// The host role: the SP/LAN listen server and the dedicated host. It owns the
// in-match host state (the loopback the host's own client rides, the np host
// owner, the HostClient replica runtime) and runs the ONE listen frame
// [orig: Game_ProcessMainFrame @0x5263f0] every fixed tick. ADR 0043 d3: the
// role replaces inmatch::listen_host::frame and the embedders' pumps around it.
#pragma once

#include <net/npwire/idatagram_socket.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/host_session.h>
#include <runtime/inmatch/loopback_channel.h>
#include <runtime/inmatch/session.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace opennova::inmatch {

struct ListenHostState {
	replication::LoopbackChannel host_loop; // the local dcb-2 client; declared before the runtime
	inmatch::HostOwner host_owner;
	std::unique_ptr<inmatch::ClientRuntime> client_runtime;
	// Deterministic overrides for the clock-minted session identity (the tick
	// digest pins them); zero = mint as the original does.
	uint32_t host_key = 0;
	uint32_t host_start_tick = 0;
	uint32_t session_seed_id = 0;
};

// What a shell hands the general bring-up beyond the host config: the S2C
// 0x45 terrain-tile source, the mission text state the initial-state burst
// streams, and the location-name table.
struct HostBringup {
	inmatch::HostConfig host_cfg;
	std::vector<uint8_t> terrain_til_data;
	bool mission_text_loaded = false;
	std::string mission_briefing3;
	std::string mission_briefing2;
	std::unordered_map<int32_t, std::string> mission_location_texts;
};

class HostRole final : public Role {
public:
	HostRole();
	// The embedder's form: the session kind it runs under and the item-class
	// resolver every HostClient view it builds takes (empty = none yet; the
	// embedder installs one through set_item_class_resolver once its catalog
	// exists).
	explicit HostRole(RoleKind kind,
			replication::ClientReplicaPipeline::ItemClassResolver item_class_resolver = {});
	RoleKind kind() const override { return kind_; }
	// The session kind this host runs under: a shell's SP listen server keeps
	// SinglePlayer (pause/step/reset stay available); a LAN host is ListenHost
	// or DedicatedHost by serve_and_play.
	void set_kind(RoleKind kind) { kind_ = kind; }

	ListenHostState state;

	// The socket the host pump reads and writes; null = the socketless
	// (SP / test) host, every datagram dropped.
	void set_socket(opennova::IDatagramSocket *socket) { socket_ = socket; }
	// The shell's item-class resolver for the HostClient's view (the wire
	// class of a type id); re-installed whenever the role rebuilds that runtime.
	void set_item_class_resolver(replication::ClientReplicaPipeline::ItemClassResolver resolver);

	// The SP listen server: SINGLEPLAYERGAME, one player, the mission's own
	// game type, socketless [orig: SinglePlayer_StartMission @0x561af0].
	void bring_up_singleplayer();
	// A HostOnly dedicated host: no local-player connection, no local player.
	void bring_up_dedicated(const inmatch::HostConfig &host_cfg);
	// The shell's general bring-up (the LAN host or its SP listen server with
	// the shell's mission text and terrain tiles).
	void bring_up(const HostBringup &bringup);
	// The embedder's bring-up record for the next boot-hook bring_up(): staged
	// right before the kernel boots (the text, tiles and config are final by
	// then), consumed by the hook.
	void stage_bringup(HostBringup bringup) { staged_bringup_ = std::move(bringup); }
	bool bring_up() override;

	// The C2S drain the host's own client feeds before the server tick.
	void drain_host_client_gameplay_requests();

	bool send_medic_request() override;
	void run_tick(const TickInput &input) override;
	bool reset_to_baseline(SessionError &error) override;
	// The host's mission exit: the round-reset 0x25 to every in-match remote,
	// the "NP.C:SH:STOP" description on every connection, one final flush.
	void close() override;
	ClientRuntime *client_runtime() override { return state.client_runtime.get(); }
	int64_t last_net_us() const override { return last_net_us_; }

private:
	void reset_state(const inmatch::GameConfig &config, bool serve_and_play);
	void make_client_runtime(uint32_t game_type);

	RoleKind kind_ = RoleKind::ListenHost;
	HostBringup staged_bringup_;
	opennova::IDatagramSocket *socket_ = nullptr;
	replication::ClientReplicaPipeline::ItemClassResolver item_class_resolver_;
	int64_t last_net_us_ = 0;
    uint64_t local_round_reset_seen_ = 0;
};

} // namespace opennova::inmatch
