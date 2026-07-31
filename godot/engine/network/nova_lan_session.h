#pragma once

#include <npwire/net_ports.h>

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/packet_peer_udp.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

#include <string>
#include <unordered_map>
#include <vector>

namespace godot {

// LAN browser for the retail game-server UDP range. A browse sends the same
// witnessed 0x41 ClientHello to every candidate port, re-announcing on the
// retail enumerator cadence, then collects the 0x81 ServerHello replies for
// the retail 30-second discovery window. The portable npruntime helper owns
// the wire format; this node owns only Godot UDP, result normalization, and
// the MpMenuCompanion-facing signal.
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
	int send_probe_burst(Error &first_send_error);
	void upsert_server(const Dictionary &row, const std::string &key);
	void emit_error(const String &message);

	Ref<PacketPeerUDP> socket_;
	Array servers_;
	std::unordered_map<std::string, int> server_indices_;
	String browse_target_;
	PackedByteArray probe_;
	int port_min_ = opennova::kRetailLanPortMin;
	int port_max_ = opennova::kRetailLanPortMax;
	double browse_elapsed_s_ = 0.0;
	double announce_elapsed_s_ = 0.0;
	bool browsing_ = false;
};

} // namespace godot
