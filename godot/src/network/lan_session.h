#pragma once

#include <net/npruntime/lan_discovery.h>
#include <net/npwire/net_ports.h>

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/packet_peer_udp.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include "network/lan_server_row.h"

namespace godot {

// LAN browser for the retail game-server UDP range. The engine's
// LanDiscoveryBrowser (npruntime/lan_discovery.h) owns the browse: the probe
// identity, the 30-second window, the 3-second re-announce cadence, the reply
// filter and the endpoint-keyed rows. This node owns only the Godot UDP
// socket, the LanServerRow shape, and the MpMenuCompanion-facing signals.
class LanSession : public Node {
	GDCLASS(LanSession, Node)

public:
	LanSession();
	~LanSession() override;

	int start_browsing(const String &destination, int port_min, int port_max);
	void stop();
	TypedArray<LanServerRow> get_servers() const;
	bool is_browsing() const { return browser_.browsing(); }

	void _ready() override;
	void _process(double delta) override;

protected:
	static void _bind_methods();

private:
	void poll_replies();
	int send_probe_burst(Error &first_send_error);
	void rebuild_rows();
	void emit_error(const String &message);

	opennova::np::LanDiscoveryBrowser browser_;
	Ref<PacketPeerUDP> socket_;
	TypedArray<LanServerRow> servers_;
	String browse_target_;
	PackedByteArray probe_;
};

} // namespace godot
