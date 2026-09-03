#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>

#include <net/novaworld/gate_response.h>

namespace godot {

// The NovaWorld gate's answer as the shell reads it (NovaWorldClient.get_server_info,
// null until a gate response lands): a value wrapper over the engine's
// GateResponse — the POST endpoint, the startup URL, the UDP NovaWorld
// address, the gate-issued session-auth codes (NW-S3: issued only to an
// authenticated request; their presence tells whether login is the
// remaining blocker) and the MET label.
class NovaWorldGateInfo : public RefCounted {
	GDCLASS(NovaWorldGateInfo, RefCounted)

	opennova::GateResponse value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::GateResponse &p_value) { value_ = p_value; }

	// Dotted X.Y.Z.W of the network-order address.
	String get_post_ip() const;
	int get_post_port() const { return static_cast<int>(value_.post_port); }
	String get_startup_url() const;
	String get_udp_novaworld() const;
	String get_udp_code1() const;
	String get_udp_code2() const;
	String get_met_label() const;
};

} // namespace godot
