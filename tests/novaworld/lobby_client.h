// A retail-protocol NovaWorld client for the service's end-to-end checks: the gate's
// probe (gate_hands_out: the lobby a gate hands a Joint Operations client) and a
// lobby session (LobbyClient: a ClientSession pumped on a thread of its own, which
// can list a keyless host), both over the engine's own codecs. nw_programs_e2e
// drives the five programs on loopback with them; the local stack's smoke test
// (stack_smoke.cpp) drives a running stack by its addresses. POSIX only.
// Infrastructure only.
#pragma once

#include "net_sockets.h"

#include <net/napi/envelope.h>
#include <net/napi/session.h>
#include <net/novaworld/client_session.h>
#include <net/novaworld/gate_probe.h>
#include <net/novaworld/gate_response.h>
#include <net/title/title_identity.h>
#include <base/gameprofile/gameprofile.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace lobby_client {

namespace net = opennova::net;
using Notice = opennova::ClientSession::Notice;

inline const opennova::TitleIdentity &jo_title() {
	return *opennova::title_identity(opennova::gameprofile::GAME_JO);
}

// The lobby endpoint the gate at `gate` hands a Joint Operations client (its
// reply's udpnovaworld, "host:port"), "" when no reply came in five tries a
// second apart (a gate with no live lobby answers nothing).
inline std::string gate_hands_out(const net::Endpoint &gate) {
	net::ScopedSocket client(net::udp_bind(0));
	const auto inner = opennova::gate_probe_build(jo_title().gate_tag);
	std::vector<uint8_t> probe(inner.size() + 16);
	size_t probe_size = 0;
	if (!client.is_valid() ||
			opennova::napi_envelope_encode(
					inner.data(), inner.size(), probe.data(), probe.size(), &probe_size) != 0)
		return {};
	probe.resize(probe_size);
	uint8_t rx[2048];
	for (int attempt = 0; attempt < 5; ++attempt) {
		net::udp_send_to(client.get(), gate, probe.data(), probe.size());
		net::Endpoint from{};
		const int n = net::udp_recv_from(client.get(), rx, sizeof(rx), from, 1000);
		if (n <= 0)
			continue;
		std::vector<uint8_t> reply(static_cast<size_t>(n));
		size_t reply_len = 0;
		opennova::GateResponse out;
		if (opennova::napi_envelope_decode(
					rx, static_cast<size_t>(n), reply.data(), reply.size(), &reply_len) == 0 &&
				opennova::gate_response_decrypt_and_parse(reply.data(), reply_len, out) &&
				out.udp_novaworld.size() == 1)
			return out.udp_novaworld.front();
	}
	return {};
}

// A retail-protocol lobby client: a Joint Operations ClientSession to `server`,
// its datagrams pumped on a thread of its own, its notices kept.
class LobbyClient {
public:
	using Clock = std::chrono::steady_clock;

	LobbyClient(const char *na, uint32_t index, uint32_t key, const net::Endpoint &server) :
			session_(config_of(na, index, key)), server_(server) {
		socket_ = net::udp_bind(0, &port_);
	}
	~LobbyClient() { stop(); }
	LobbyClient(const LobbyClient &) = delete;
	LobbyClient &operator=(const LobbyClient &) = delete;

	void start() {
		{
			std::lock_guard<std::mutex> lock(mu_);
			send(session_.start());
		}
		thread_ = std::thread([this] { run(); });
	}

	// Its goodbye, then the pump's end.
	void stop() {
		if (!thread_.joinable())
			return;
		{
			std::lock_guard<std::mutex> lock(mu_);
			send(session_.build_goodbye());
		}
		abandon();
	}

	// The pump's end with no goodbye, as a client the lobby stops hearing.
	void abandon() {
		if (!thread_.joinable())
			return;
		stopping_.store(true);
		thread_.join();
		net::close_socket(socket_);
	}

	bool verified() {
		std::lock_guard<std::mutex> lock(mu_);
		return session_.is_verified();
	}

	// A ClientHostRequest listing `name` (no HostKey: a keyless host, listed with no owner).
	void host(const std::string &name) {
		std::lock_guard<std::mutex> lock(mu_);
		session_.queue_statement(
				opennova::make_client_host_request(0, { { 0, "NWUID", session_.server_nwuid() } },
						{ { 0, "LobbyName", "jop_2_consumer" }, { 0, "MaxPlayers", "8" } },
						{ { 0, "ServerName", name }, { 0, "Players", "1" } },
						{ { 0, "PlayerName", "Host" } }));
	}

	size_t notice_count() {
		std::lock_guard<std::mutex> lock(mu_);
		return notices_.size();
	}

	// The first notice of `kind` at index `from` or after, waited for `ms`.
	std::optional<Notice> wait_notice(Notice::Kind kind, size_t from, int ms) {
		const auto deadline = Clock::now() + std::chrono::milliseconds(ms);
		for (;;) {
			{
				std::lock_guard<std::mutex> lock(mu_);
				for (size_t i = from; i < notices_.size(); ++i)
					if (notices_[i].kind == kind)
						return notices_[i];
			}
			if (Clock::now() >= deadline)
				return std::nullopt;
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
	}

	// The port its socket bound (its lobby_sessions row is at <its address>:<this>).
	uint16_t port() const { return port_; }

private:
	static opennova::ClientSession::Config config_of(const char *na, uint32_t index, uint32_t key) {
		opennova::ClientSession::Config cfg =
				opennova::ClientSession::Config::lobby(jo_title(), jo_title().gate_tag);
		cfg.client_index = index;
		cfg.client_key = key;
		cfg.na = na;
		cfg.cookie_vars = [] {
			return std::vector<std::pair<std::string, std::string>>{ { "NWUID", "" } };
		};
		return cfg;
	}

	void send(const std::vector<uint8_t> &datagram) {
		if (!datagram.empty())
			net::udp_send_to(socket_, server_, datagram.data(), datagram.size());
	}

	void run() {
		uint8_t rx[4096];
		while (!stopping_.load()) {
			net::Endpoint from;
			const int n = net::udp_recv_from(socket_, rx, sizeof rx, from, 20);
			std::lock_guard<std::mutex> lock(mu_);
			std::vector<std::vector<uint8_t>> out;
			if (n > 0)
				session_.handle_datagram(rx, static_cast<size_t>(n), out);
			session_.finish_receive_batch(out);
			session_.pump(out);
			session_.process_periodic_update();
			for (const auto &datagram : out)
				send(datagram);
			for (Notice &notice : session_.take_notices())
				notices_.push_back(std::move(notice));
		}
	}

	std::mutex mu_;
	opennova::ClientSession session_;
	net::Socket socket_;
	uint16_t port_ = 0;
	net::Endpoint server_;
	std::vector<Notice> notices_;
	std::atomic<bool> stopping_{ false };
	std::thread thread_;
};

} // namespace lobby_client
