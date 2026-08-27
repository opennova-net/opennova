#include "network/lan_session.h"

#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace godot {

namespace {

constexpr const char *DEFAULT_BROADCAST = "255.255.255.255";

uint32_t pick_random_uint32() {
	static thread_local std::mt19937 gen{std::random_device{}()};
	return std::uniform_int_distribution<uint32_t>(1)(gen);
}

PackedByteArray to_packed_bytes(const std::vector<uint8_t> &bytes) {
	PackedByteArray out;
	out.resize(static_cast<int>(bytes.size()));
	if (!bytes.empty()) {
		std::memcpy(out.ptrw(), bytes.data(), bytes.size());
	}
	return out;
}

Dictionary row_dictionary(const opennova::np::LanDiscoveryRow &row) {
	Dictionary out;
	const String server_name = String::utf8(row.server.server_name.c_str());
	out["name"] = server_name;
	out["server_name"] = server_name;
	out["host_ip"] = String::utf8(row.host_ip.c_str());
	out["port"] = row.port;
	out["players"] = static_cast<int64_t>(row.server.current_players);
	out["max_players"] = static_cast<int64_t>(row.server.max_players);
	out["gametype"] = static_cast<int64_t>(row.server.gametype);
	out["session_id"] = String::utf8(row.server.session_id.c_str());
	out["expansion"] = String::utf8(row.server.expansion.c_str());
	return out;
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
			D_METHOD("start_browsing", "destination", "port_min", "port_max"),
			&LanSession::start_browsing,
			DEFVAL(String(DEFAULT_BROADCAST)), DEFVAL(int(opennova::kRetailLanPortMin)), DEFVAL(int(opennova::kRetailLanPortMax)));
	ClassDB::bind_method(D_METHOD("stop"), &LanSession::stop);
	ClassDB::bind_method(D_METHOD("get_servers"), &LanSession::get_servers);
	ClassDB::bind_method(D_METHOD("is_browsing"), &LanSession::is_browsing);

	ADD_SIGNAL(MethodInfo("servers_changed", PropertyInfo(Variant::ARRAY, "servers")));
	ADD_SIGNAL(MethodInfo("error_occurred", PropertyInfo(Variant::STRING, "message")));
}

void LanSession::_ready() {
	set_process(browser_.browsing());
}

int LanSession::start_browsing(const String &destination, int port_min, int port_max) {
	stop();
	servers_.clear();
	emit_signal("servers_changed", get_servers());

	const String target = destination.strip_edges();
	if (target.is_empty() || !browser_.begin(pick_random_uint32(), port_min, port_max)) {
		emit_error("LAN browse destination or port range is invalid");
		return static_cast<int>(ERR_INVALID_PARAMETER);
	}

	socket_.instantiate();
	const Error bind_error = socket_->bind(0, "0.0.0.0");
	if (bind_error != OK) {
		socket_.unref();
		browser_.stop();
		emit_error(String("LAN browse socket bind failed (error ") +
				String::num_int64(static_cast<int64_t>(bind_error)) + ")");
		return static_cast<int>(bind_error);
	}
	socket_->set_broadcast_enabled(true);

	probe_ = to_packed_bytes(browser_.probe());
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
	for (int port = browser_.port_min(); port <= browser_.port_max(); ++port) {
		const Error destination_error = socket_->set_dest_address(browse_target_, port);
		if (destination_error != OK) {
			if (first_send_error == OK) first_send_error = destination_error;
			continue;
		}
		const Error send_error = socket_->put_packet(probe_);
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

Array LanSession::get_servers() const {
	return servers_.duplicate(true);
}

void LanSession::_process(double delta) {
	if (!browser_.browsing() || !socket_.is_valid()) {
		return;
	}

	poll_replies();
	bool announce_due = false;
	if (!browser_.advance(delta, announce_due)) {
		stop();
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
		const std::string source_ip(socket_->get_packet_ip().utf8().get_data());
		const int source_port = static_cast<int>(socket_->get_packet_port());
		if (browser_.accept_reply(packet.ptr(), static_cast<size_t>(packet.size()),
					source_ip, source_port) != opennova::np::LanRowChange::kNone)
			changed = true;
	}
	if (changed) rebuild_rows();
}

void LanSession::rebuild_rows() {
	servers_.clear();
	for (const opennova::np::LanDiscoveryRow &row : browser_.servers())
		servers_.push_back(row_dictionary(row));
	emit_signal("servers_changed", get_servers());
}

void LanSession::emit_error(const String &message) {
	emit_signal("error_occurred", message);
}

} // namespace godot
