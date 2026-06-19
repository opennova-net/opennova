#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/packet_peer_udp.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include <novaworld/ingame_decode.h>
#include <novaworld/replay_timeline.h>
#include <novaworld/wire_capture.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace godot {

class NovaItemDatabase;

// The in-match NovaWorld client — the production multiplayer RECEIVE path.
//
// It binds a UDP socket, receives the in-match wire stream, decodes it
// incrementally with the same libs the cross-validation tests use
// (CaptureDecoder + build_replay_timeline), and exposes the resulting per-entity
// world model to GDScript. Replay vs real differ ONLY by where the bytes come
// from: today nw_replay (a captured session) dials it in and streams a role's
// raw datagrams; cutting over to real multiplayer means pointing this at a real
// server instead. Nothing in the decode/render path changes.
//
// Bootstrap (replay): connect_to_replay() binds the socket, sends a HELLO to
// nw_replay's control endpoint, and waits for the ASSIGN that names this
// instance's role (host or a specific client). Then every datagram that arrives
// is pushed through the decoder and folded into the world model.
class NovaNetClient : public Node {
	GDCLASS(NovaNetClient, Node)

public:
	enum State {
		STATE_IDLE = 0,
		STATE_CONNECTING,   // HELLO sent, awaiting ASSIGN
		STATE_RECEIVING,    // assigned a role, decoding the live stream
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
	// wire type_id -> §5.10b dispatch class table needed to walk S2C 0x0A motion.
	// Without it, only spawns + own-uplink track (other entities sit still).
	void set_item_database(const Ref<NovaItemDatabase> &db);

	// Lifecycle.
	void connect_to_replay();
	void stop();

	State get_state() const { return state_; }
	int get_role_index() const { return role_index_; }
	bool is_host_role() const { return is_host_; }
	int get_session_port() const { return session_port_; }
	String get_mission() const { return mission_; }

	// World-model query (the render feed).
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
	// The entity this instance "is": the one with a clean C2S 0x0C uplink track
	// (a client's own player). 0xFFFF for the host (spectator — no own uplink).
	int get_own_player_handle() const;

	// Engine hooks.
	void _process(double delta) override;

protected:
	static void _bind_methods();

private:
	void send_hello();
	bool parse_assign(const uint8_t *p, int n);
	void drain_socket();
	void rebuild_world();
	void enter_state(State next, const String &reason = String());
	opennova::EntityClass class_of(uint16_t wire_type) const;

	// Config.
	String replay_host_ = "127.0.0.1";
	int replay_port_ = 42000;
	double rebuild_interval_s_ = 0.1; // 10 Hz world-model rebuild
	double connect_timeout_s_ = 15.0;
	double hello_resend_s_ = 0.5;

	// State.
	State state_ = STATE_IDLE;
	Ref<PacketPeerUDP> socket_;
	int role_index_ = -1;
	bool is_host_ = false;
	int session_port_ = 0;
	String mission_;

	double connect_elapsed_ = 0.0;
	double since_hello_ = 0.0;
	double since_rebuild_ = 0.0;
	bool dirty_ = false; // new messages since the last rebuild

	// Decode + world model.
	opennova::CaptureDecoder decoder_;
	std::vector<opennova::InGameMessage> messages_;
	opennova::ReplayTimeline world_;
	int recv_counter_ = 0;            // synthetic capture frame_index for arrivals
	int local_port_ = 0;             // our bound UDP port (datagram dst)

	// wire type_id -> dispatch class, snapshotted from the item database.
	std::unordered_map<uint16_t, opennova::EntityClass> class_table_;
	uint16_t own_player_handle_ = 0xFFFF;
};

} // namespace godot

VARIANT_ENUM_CAST(godot::NovaNetClient::State);
