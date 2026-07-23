#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/packet_peer_udp.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

#include <string>
#include <unordered_map>

namespace godot {

// LAN browser for the retail game-server UDP range. A browse sends the same
// witnessed 0x41 ClientHello to every candidate port, then collects the 0x81
// ServerHello replies for the retail 30-second discovery window. The portable
// npruntime helper owns the wire format; this node owns only Godot UDP, result
// normalization, and the MpMenuHost-facing signal.
class NovaLanSession : public Node {
	GDCLASS(NovaLanSession, Node)

public:
	NovaLanSession();
	~NovaLanSession() override;

	int start_browsing(const String &destination, int port_min, int port_max);
	void stop();
	Array get_servers() const;
	bool is_browsing() const { return browsing_; }

	void _ready() override;
	void _process(double delta) override;

protected:
	static void _bind_methods();

private:
	void poll_replies();
	void upsert_server(const Dictionary &row, const std::string &key);
	void emit_error(const String &message);

	Ref<PacketPeerUDP> socket_;
	Array servers_;
	std::unordered_map<std::string, int> server_indices_;
	int port_min_ = 32768;
	int port_max_ = 32787;
	double browse_elapsed_s_ = 0.0;
	bool browsing_ = false;
};

} // namespace godot
