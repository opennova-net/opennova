#include "network/lan_session.h"

#include <godot_cpp/classes/ip.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

#include "util/data_format.h"
#include "util/string_convert.h"

#include <net/novaworld/lobby_vars.h> // resolve_address_without_dns
#include <net/npwire/nw_session_framing.h> // next_connection_index
#include <net/npwire/peer_addr.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace godot {

namespace {

constexpr const char *DEFAULT_BROADCAST = "255.255.255.255";

Ref<LanServerRow> row_record(const opennova::LanDiscoveryRow &row, uint32_t client_index) {
	// Retail hosts advertise the typed server name in the host's single-byte
	// codepage (cp1252), never UTF-8.
	Ref<LanServerRow> out = LanServerRow::make(
			opennova::cp1252_to_gd(row.server.server_name),
			opennova::to_gd(row.host_ip), static_cast<int>(row.port));
	out->set_players(static_cast<int>(row.server.current_players));
	out->set_max_players(static_cast<int>(row.server.max_players));
	out->set_gametype(static_cast<int64_t>(row.server.gametype));
	out->set_server_flags(static_cast<int64_t>(row.server.server_flags));
	out->set_session_id(opennova::to_gd(row.server.session_id));
	out->set_expansion(opennova::to_gd(row.server.expansion));
	out->set_host_key(static_cast<int64_t>(row.server.host_key));
	out->set_password_required(row.server.password_required);
	// The browse's connection, which a join from this row goes on with (D-NET-521).
	out->set_client_index(static_cast<int64_t>(client_index));
	return out;
}

// The enumerate address of the browse destination, as the transport resolves it
// (the engine's resolve_address_without_dns carries the witness): the broadcast
// literal is the broadcast (0xFFFFFFFF), else the platform's inet_addr, else its
// name lookup, in PeerAddr's packing; 0 when nothing resolves, and the
// announces then write no EIP (net/npwire/lan_discovery.h, D-NET-520).
uint32_t enumerate_address(const String &p_target) {
	const uint32_t address = opennova::resolve_address_without_dns(opennova::to_std(p_target));
	if (address != 0) return address;
	IP *ip = IP::get_singleton();
	if (ip == nullptr) return 0;
	const PackedStringArray octets = ip->resolve_hostname(p_target, IP::TYPE_IPV4).split(".");
	if (octets.size() != 4) return 0;
	std::array<uint8_t, 4> bytes{};
	for (int i = 0; i < 4; ++i) {
		if (!octets[i].is_valid_int()) return 0;
		const int64_t value = octets[i].to_int();
		if (value < 0 || value > 255) return 0;
		bytes[static_cast<size_t>(i)] = static_cast<uint8_t>(value);
	}
	return opennova::peer_addr_from_octets(bytes, 0).ip;
}

} // namespace

LanSession::LanSession() {
	set_process(false);
}

LanSession::~LanSession() {
	stop();
}

void LanSession::_bind_methods() {
	ClassDB::bind_method(
			D_METHOD("start_browsing", "destination", "port_min", "port_max", "connect_type"),
			&LanSession::start_browsing,
			DEFVAL(String(DEFAULT_BROADCAST)), DEFVAL(int(opennova::kRetailLanPortMin)), DEFVAL(int(opennova::kRetailLanPortMax)),
			DEFVAL(int(opennova::kLanConnectTypeLan)));
	ClassDB::bind_method(D_METHOD("set_resource_root", "root"), &LanSession::set_resource_root);
	ClassDB::bind_method(D_METHOD("get_resource_root"), &LanSession::get_resource_root);
	ClassDB::bind_method(D_METHOD("stop"), &LanSession::stop);
	ClassDB::bind_method(D_METHOD("get_servers"), &LanSession::get_servers);
	ClassDB::bind_method(D_METHOD("is_browsing"), &LanSession::is_browsing);
	ClassDB::bind_method(D_METHOD("get_client_index"), &LanSession::get_client_index);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "resource_root", PROPERTY_HINT_RESOURCE_TYPE, "ResourceRoot"),
	             "set_resource_root", "get_resource_root");

	ADD_SIGNAL(MethodInfo("servers_changed", PropertyInfo(Variant::ARRAY, "servers")));
	// The browse window expired (the 30 s gate): the LAN screen re-enables its
	// search button on this edge.
	ADD_SIGNAL(MethodInfo("browse_finished"));
	ADD_SIGNAL(MethodInfo("error_occurred", PropertyInfo(Variant::STRING, "message")));
}

void LanSession::_ready() {
	set_process(browser_.browsing());
}

int LanSession::start_browsing(const String &destination, int port_min, int port_max,
		int connect_type) {
	stop();
	servers_.clear();
	emit_signal("servers_changed", get_servers());

	std::string title_error;
	const opennova::TitleIdentity *title = ResourceRoot::session_title(resource_root_, title_error);
	if (title == nullptr) {
		emit_error(String("LAN browse has no identity: ") + opennova::to_gd(title_error));
		return static_cast<int>(ERR_UNCONFIGURED);
	}
	const String target = destination.strip_edges();
	if (target.is_empty() ||
			!browser_.begin(*title, opennova::next_connection_index(), enumerate_address(target),
					port_min, port_max, connect_type)) {
		emit_error("LAN browse destination or port range is invalid");
		return static_cast<int>(ERR_INVALID_PARAMETER);
	}

	socket_.instantiate();
	// The browse socket is the client arm's: the authored client port range,
	// scanned from its min (net_ports.h lan_client_bind_ports, D-NET-294).
	Error bind_error = ERR_CANT_OPEN;
	for (const uint16_t candidate : opennova::lan_client_bind_ports()) {
		bind_error = socket_->bind(candidate, "0.0.0.0");
		if (bind_error == OK) break;
	}
	if (bind_error != OK) {
		socket_.unref();
		browser_.stop();
		emit_error(String("LAN browse socket bind failed (error ") +
				String::num_int64(static_cast<int64_t>(bind_error)) + ")");
		return static_cast<int>(bind_error);
	}
	socket_->set_broadcast_enabled(true);

	browse_target_ = target;

	Error first_send_error = OK;
	const int sent = send_probe_burst(first_send_error);

	if (sent == 0) {
		const Error send_error = first_send_error == OK ? ERR_CANT_CONNECT : first_send_error;
		stop();
		emit_error(String("LAN discovery probe send failed (error ") +
				String::num_int64(static_cast<int64_t>(send_error)) + ")");
		return static_cast<int>(send_error);
	}

	set_process(true);
	return static_cast<int>(OK);
}

int LanSession::send_probe_burst(Error &first_send_error) {
	int sent = 0;
	if (!socket_.is_valid()) {
		return sent;
	}
	// One announce per port, each naming its own destination as EIP / EPN
	// (LanDiscoveryBrowser::probe).
	for (int port = browser_.port_min(); port <= browser_.port_max(); ++port) {
		const Error destination_error = socket_->set_dest_address(browse_target_, port);
		if (destination_error != OK) {
			if (first_send_error == OK) first_send_error = destination_error;
			continue;
		}
		const Error send_error = socket_->put_packet(to_packed_bytes(browser_.probe(port)));
		if (send_error == OK) {
			++sent;
		} else if (first_send_error == OK) {
			first_send_error = send_error;
		}
	}
	return sent;
}

void LanSession::stop() {
	if (socket_.is_valid()) {
		socket_->close();
	}
	socket_.unref();
	browser_.stop();
	set_process(false);
}

TypedArray<LanServerRow> LanSession::get_servers() const {
	return servers_.duplicate();
}

void LanSession::_process(double delta) {
	if (!browser_.browsing() || !socket_.is_valid()) {
		return;
	}

	poll_replies();
	bool announce_due = false;
	if (!browser_.advance(delta, announce_due)) {
		stop();
		emit_signal("browse_finished");
		return;
	}
	// Transient send errors during a re-announce are dropped like retail's
	// fire-and-forget pump.
	if (announce_due) {
		Error ignored_error = OK;
		send_probe_burst(ignored_error);
	}
}

void LanSession::poll_replies() {
	bool changed = false;
	while (socket_.is_valid() && socket_->get_available_packet_count() > 0) {
		const PackedByteArray packet = socket_->get_packet();
		const std::string source_ip = opennova::to_std(socket_->get_packet_ip());
		const int source_port = static_cast<int>(socket_->get_packet_port());
		if (browser_.accept_reply(packet.ptr(), static_cast<size_t>(packet.size()),
					source_ip, source_port) != opennova::LanRowChange::kNone)
			changed = true;
	}
	if (changed) rebuild_rows();
}

void LanSession::rebuild_rows() {
	servers_.clear();
	for (const opennova::LanDiscoveryRow &row : browser_.servers())
		servers_.push_back(row_record(row, browser_.client_index()));
	emit_signal("servers_changed", get_servers());
}

void LanSession::emit_error(const String &message) {
	emit_signal("error_occurred", message);
}

} // namespace godot
