#include "network/nova_lan_session.h"

#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

#include <npruntime/lan_discovery.h>

#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace godot {

namespace {

constexpr const char *DEFAULT_BROADCAST = "255.255.255.255";
// The witnessed browse window: the LAN screen's state machine re-enables LAN_SEARCH
// (MP_SEARCH) once GetTickCount() - search_start > 0x7530 (30000 ms).
// [orig: UI_ProcessLANSessionStateMachine @ 0x558de0, the 0x7530 gate @0x55933a]
constexpr double BROWSE_WINDOW_SECONDS = 30.0;
// Retail re-announces while enumerating every 3000 ms — discovery is a cadence, not a
// single burst. The pump's interval select is (+37 ? 3000 : enum+20), and +37 IS the
// enumerator identity: the 0x41 announce builder skips the player-count TLV exactly
// when +37 is set, matching the captured client probe shape (which carries none), and
// the golden LAN capture shows the ~3 s re-probe cadence live.
// [orig: CNapiNPConnection_PumpEnumeratorAndSend @ 0x6290c0 interval select;
//  NapiNPSession_SendAnnouncePacket @ 0x61fa00 +37 player-count gate]
constexpr double ANNOUNCE_INTERVAL_SECONDS = 3.0;

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

bool is_usable_address(const String &address) {
	return !address.is_empty() && address != "0.0.0.0" && address != "::";
}

std::string endpoint_key(const String &address, int port) {
	const CharString utf8 = address.utf8();
	return std::string("endpoint:") + utf8.get_data() + ":" + std::to_string(port);
}

Dictionary normalized_server_row(const opennova::np::LanDiscoveryServer &server,
		const String &source_ip, int source_port) {
	Dictionary row;
	const String server_name = String::utf8(server.server_name.c_str());
	row["name"] = server_name;
	row["server_name"] = server_name;
	row["host_ip"] = source_ip;
	row["port"] = source_port;
	row["players"] = static_cast<int64_t>(server.current_players);
	row["max_players"] = static_cast<int64_t>(server.max_players);
	row["gametype"] = static_cast<int64_t>(server.gametype);
	row["session_id"] = String::utf8(server.session_id.c_str());
	row["expansion"] = String::utf8(server.expansion.c_str());
	return row;
}

} // namespace

NovaLanSession::NovaLanSession() {
	set_process(false);
}

NovaLanSession::~NovaLanSession() {
	stop();
}

void NovaLanSession::_bind_methods() {
	ClassDB::bind_method(
			D_METHOD("start_browsing", "destination", "port_min", "port_max"),
			&NovaLanSession::start_browsing,
			DEFVAL(String(DEFAULT_BROADCAST)), DEFVAL(int(opennova::kRetailLanPortMin)), DEFVAL(int(opennova::kRetailLanPortMax)));
	ClassDB::bind_method(D_METHOD("stop"), &NovaLanSession::stop);
	ClassDB::bind_method(D_METHOD("get_servers"), &NovaLanSession::get_servers);
	ClassDB::bind_method(D_METHOD("is_browsing"), &NovaLanSession::is_browsing);

	ADD_SIGNAL(MethodInfo("servers_changed", PropertyInfo(Variant::ARRAY, "servers")));
	ADD_SIGNAL(MethodInfo("error_occurred", PropertyInfo(Variant::STRING, "message")));
}

void NovaLanSession::_ready() {
	set_process(browsing_);
}

int NovaLanSession::start_browsing(const String &destination, int port_min, int port_max) {
	stop();
	servers_.clear();
	server_indices_.clear();
	emit_signal("servers_changed", get_servers());

	const String target = destination.strip_edges();
	if (target.is_empty() || port_min < 1 || port_max > 65535 || port_min > port_max) {
		emit_error("LAN browse destination or port range is invalid");
		return static_cast<int>(ERR_INVALID_PARAMETER);
	}

	socket_.instantiate();
	const Error bind_error = socket_->bind(0, "0.0.0.0");
	if (bind_error != OK) {
		socket_.unref();
		emit_error(String("LAN browse socket bind failed (error ") +
				String::num_int64(static_cast<int64_t>(bind_error)) + ")");
		return static_cast<int>(bind_error);
	}
	socket_->set_broadcast_enabled(true);

	// One identity per browse window: retail keeps its connection identity
	// across the enumerator's re-announce pumps, so every burst repeats the
	// same probe bytes.
	probe_ = to_packed_bytes(
			opennova::np::build_lan_discovery_probe(pick_random_uint32()));
	browse_target_ = target;
	port_min_ = port_min;
	port_max_ = port_max;

	Error first_send_error = OK;
	const int sent = send_probe_burst(first_send_error);

	if (sent == 0) {
		const Error send_error = first_send_error == OK ? ERR_CANT_CONNECT : first_send_error;
		stop();
		emit_error(String("LAN discovery probe send failed (error ") +
				String::num_int64(static_cast<int64_t>(send_error)) + ")");
		return static_cast<int>(send_error);
	}

	browse_elapsed_s_ = 0.0;
	announce_elapsed_s_ = 0.0;
	browsing_ = true;
	set_process(true);
	return static_cast<int>(OK);
}

int NovaLanSession::send_probe_burst(Error &first_send_error) {
	int sent = 0;
	if (!socket_.is_valid()) {
		return sent;
	}
	for (int port = port_min_; port <= port_max_; ++port) {
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

void NovaLanSession::stop() {
	if (socket_.is_valid()) {
		socket_->close();
	}
	socket_.unref();
	browsing_ = false;
	browse_elapsed_s_ = 0.0;
	set_process(false);
}

Array NovaLanSession::get_servers() const {
	return servers_.duplicate(true);
}

void NovaLanSession::_process(double delta) {
	if (!browsing_ || !socket_.is_valid()) {
		return;
	}

	poll_replies();
	browse_elapsed_s_ += delta;
	announce_elapsed_s_ += delta;
	if (browse_elapsed_s_ >= BROWSE_WINDOW_SECONDS) {
		stop();
		return;
	}
	// A cold host that binds its port mid-window is only discoverable because
	// the enumerator keeps announcing; transient send errors during a
	// re-announce are dropped like retail's fire-and-forget pump.
	if (announce_elapsed_s_ >= ANNOUNCE_INTERVAL_SECONDS) {
		announce_elapsed_s_ = 0.0;
		Error ignored_error = OK;
		send_probe_burst(ignored_error);
	}
}

void NovaLanSession::poll_replies() {
	while (socket_.is_valid() && socket_->get_available_packet_count() > 0) {
		const PackedByteArray packet = socket_->get_packet();
		const String source_ip = socket_->get_packet_ip();
		const int source_port = static_cast<int>(socket_->get_packet_port());
		if (packet.is_empty() || source_port < port_min_ || source_port > port_max_ ||
				!is_usable_address(source_ip)) {
			continue;
		}

		opennova::np::LanDiscoveryServer server;
		if (!opennova::np::parse_lan_discovery_reply(
				packet.ptr(), static_cast<size_t>(packet.size()), server)) {
			continue;
		}

		upsert_server(normalized_server_row(server, source_ip, source_port),
				endpoint_key(source_ip, source_port));
	}
}

void NovaLanSession::upsert_server(const Dictionary &row, const std::string &key) {
	const auto it = server_indices_.find(key);
	if (it != server_indices_.end()) {
		// The host re-announces every browse interval and its row data is live state
		// (player count, mission rotation) — refresh the stored row in place and
		// re-emit only when something actually changed, so a stale first-seen row
		// does not survive the whole browse window.
		if (servers_[static_cast<size_t>(it->second)] == row) return;
		servers_[static_cast<size_t>(it->second)] = row;
		emit_signal("servers_changed", get_servers());
		return;
	}
	server_indices_.emplace(key, static_cast<int>(servers_.size()));
	servers_.push_back(row);
	emit_signal("servers_changed", get_servers());
}

void NovaLanSession::emit_error(const String &message) {
	emit_signal("error_occurred", message);
}

} // namespace godot
