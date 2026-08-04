#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/packet_peer_udp.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include <npwire/entity_class.h>
#include <npwire/wire_capture.h>

#include <netsim/client_replica_pipeline.h>
#include <netsim/item_replication_catalog.h>
#include <netsim/replica_history.h>

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace godot {

class NovaItemDatabase;

// The in-match NovaWorld spectator/replay client — a DEV TOOL, not the live
// gameplay receive path.
//
// The live in-match receive path is NovaSimulation's joiner (enable_join ->
// joiner_pump -> ClientRuntime) rendered by wire_present_pass; that is what a
// player uses to join and play. NovaNetClient is the read-only spectator that
// binds a UDP socket, receives the in-match wire stream, decodes it incrementally
// with the same ClientReplicaPipeline live play uses. ReplicaHistory journals
// that canonical state for scrubbing/events; it is not a second entity fold. The
// binding exposes the resulting per-entity world model + the
// mission name (read off the wire) to GDScript. Its source is interchangeable:
// today nw_replay (a captured session) streams the authoritative world to it, and
// it can equally be pointed at a live server to observe — but it is a spectator/
// debugging surface, not the joiner that owns the in-match handshake.
//
// Bootstrap: connect_to_replay() binds the socket and sends a small REGISTER ping
// so the source learns this client's address (resent until the first datagram
// arrives); then every datagram is pushed through the decoder and folded into the
// world model. The mission/map name comes from the wire (S2C 0x7B), so the host
// need not be told it out of band.
class NovaNetClient : public Node {
	GDCLASS(NovaNetClient, Node)

public:
	enum State {
		STATE_IDLE = 0,
		STATE_CONNECTING,   // REGISTER sent, awaiting the first datagram
		STATE_RECEIVING,    // streaming + decoding the live world
		STATE_DISCONNECTED,
		STATE_ERROR,
	};

	NovaNetClient();
	~NovaNetClient();

	// Config (GDScript properties).
	void set_replay_host(const String &host);
	String get_replay_host() const;
	void set_replay_port(int port);
	int get_replay_port() const;

	// The items.def database (already loaded from the game dir) supplies the
	// immutable replication catalog needed to size S2C 0x0A records. Without it,
	// unknown record widths fail closed instead of guessing Person or Vehicle.
	void set_item_database(const Ref<NovaItemDatabase> &db);

	// Lifecycle.
	void connect_to_replay();
	void stop();

	State get_state() const { return state_; }
	// The mission/map .bms name, read off the wire (S2C 0x7B). Empty until seen.
	String get_mission() const { return mission_; }

	// World-model query (history/event tooling).
	int get_entity_count() const;
	// One dict per live entity: {handle, pool, type_id, name, team, team_known,
	// owner_session, spawn_tag, is_own_player}.
	Array get_entities() const;
	// The most recent capture frame folded into the model (the render clock head).
	int get_latest_frame() const;
	Vector2i get_frame_range() const;
	// Interpolated pose at fractional capture-frame `frame_f` for `handle`:
	// {found, pos (Vector3 world meters), heading_deg, has_heading, alive, dead,
	// mounted}. Respawn/mount/dead transitions snap (never interpolate across).
	Dictionary sample_at(int handle, double frame_f) const;

	// Every decoded in-game event on the timeline, in capture order. One dict per
	// event: {frame, kind (ReplayEventKind int), has_pos, pos (Vector3 world
	// meters), has_dir, dir (Vector2), source, target, aux (0xFFFF = none),
	// adm_index, event_type, sound, label}. Feeds the kill feed + projectile/zone
	// markers. source/target/aux are entity handles (resolve via get_entities()).
	Array get_events() const;
	// The environment snapshot in effect at capture-frame `frame_f` (the latest
	// snapshot at or before it; the first when `frame_f` precedes the stream):
	// {found, frame, fog_dist, fog_accel, tod_fixed, quake_ticks, cloud_scroll,
	// overcast}. found=false until the wire carries an env block.
	Dictionary env_at(double frame_f) const;

	// The same flat replica projection consumed by WirePresentPass for live play.
	// A spectator has no locally-controlled Person, so the local-handle seam is
	// explicitly absent.
	PackedFloat32Array get_present_snapshot();
	int get_present_stride() const;
	int64_t get_present_layout_revision() const { return present_layout_revision_; }
	bool has_local_player() const { return false; }
	int get_local_player_wire_handle() const { return -1; }
	int get_logic_tick() const { return static_cast<int>(logic_tick_); }

	// Engine hooks.
	void _process(double delta) override;

protected:
	static void _bind_methods();

private:
	void send_register();
	void drain_socket();
	void advance_replica_ticks(double delta);
	void refresh_present_layout();
	void install_class_resolver();
	void enter_state(State next, const String &reason = String());
	opennova::netsim::ClientReplicaPipeline::ItemClassResolution
	class_of(uint16_t wire_type) const;

	// Config.
	String replay_host_ = "127.0.0.1";
	int replay_port_ = 42000;
	double publish_interval_s_ = 0.1; // cap world_updated notifications at 10 Hz
	double connect_timeout_s_ = 15.0;
	double register_resend_s_ = 0.5;

	// State.
	State state_ = STATE_IDLE;
	Ref<PacketPeerUDP> socket_;
	String mission_; // map .bms name, read off the wire (S2C 0x7B)

	double connect_elapsed_ = 0.0;
	double since_register_ = 0.0;
	double since_publish_ = 0.0;
	double replica_tick_credit_s_ = 0.0;
	bool dirty_ = false; // replica/history changes since the last notification

	// Decode + world model.
	opennova::CaptureDecoder decoder_;
	opennova::netsim::ClientReplicaPipeline replicas_;
	opennova::netsim::ReplicaHistory history_;
	int recv_counter_ = 0;            // synthetic capture frame_index for arrivals
	int local_port_ = 0;             // our bound UDP port (datagram dst)
	uint64_t logic_tick_ = 0;
	uint64_t observed_replica_topology_revision_ = 0;
	int64_t present_layout_revision_ = 0;
	std::vector<std::pair<uint16_t, uint16_t>> present_layout_;

	// Immutable items.def replication profiles. This is configuration, not
	// per-connection state, and is built through the same adapter live play uses.
	std::shared_ptr<const opennova::netsim::ItemReplicationCatalog>
			replication_catalog_;
};

} // namespace godot

VARIANT_ENUM_CAST(godot::NovaNetClient::State);
