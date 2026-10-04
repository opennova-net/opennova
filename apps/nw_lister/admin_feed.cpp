#include "admin_feed.h"

#include "log.h"
#include "socket_compat.h"
#include "udp.h" // Addr, resolve

#include <net/novacrypto/nwu.h> // NWU_LCG_MAGIC

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace opennova::lister {

namespace {

constexpr uint8_t kMagic[4] = {0x00, 0x00, 0x0D, 0x0A};
constexpr size_t kHeaderSize = 8;
constexpr uint32_t kMaxPacket = 1u << 20;
constexpr int kIoTimeoutMs = 10000;

std::string trim(const std::string &s) {
	size_t b = 0, e = s.size();
	while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
	while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
	return s.substr(b, e - b);
}

std::string lower(std::string s) {
	for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return s;
}

std::vector<std::string> split_lines(const std::string &text) {
	std::vector<std::string> out;
	std::istringstream in(text);
	std::string line;
	while (std::getline(in, line)) {
		if (!line.empty() && line.back() == '\r') line.pop_back();
		out.push_back(line);
	}
	return out;
}

std::vector<std::string> split_tabs(const std::string &line) {
	std::vector<std::string> out;
	size_t start = 0;
	while (true) {
		const size_t tab = line.find('\t', start);
		out.push_back(trim(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start)));
		if (tab == std::string::npos) break;
		start = tab + 1;
	}
	return out;
}

bool all_digits(const std::string &s) {
	return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

// --- one blocking TCP connection --------------------------------------------

bool send_all(SockHandle s, const uint8_t *data, size_t size) {
#ifdef MSG_NOSIGNAL
	constexpr int kFlags = MSG_NOSIGNAL; // a reset peer must not SIGPIPE the lister
#else
	constexpr int kFlags = 0;
#endif
	while (size) {
		const int n = ::send(s, reinterpret_cast<const char *>(data), static_cast<int>(size), kFlags);
		if (n <= 0) return false;
		data += n;
		size -= static_cast<size_t>(n);
	}
	return true;
}

bool recv_exact(SockHandle s, uint8_t *data, size_t size) {
	while (size) {
		const int n = ::recv(s, reinterpret_cast<char *>(data), static_cast<int>(size), 0);
		if (n <= 0) return false;
		data += n;
		size -= static_cast<size_t>(n);
	}
	return true;
}

bool send_packet(SockHandle s, const uint8_t *payload, size_t size) {
	std::vector<uint8_t> p(kHeaderSize + size);
	std::memcpy(p.data(), kMagic, 4);
	const uint32_t total = static_cast<uint32_t>(p.size());
	for (int i = 0; i < 4; ++i) p[4 + i] = static_cast<uint8_t>(total >> (8 * i));
	if (size) std::memcpy(p.data() + kHeaderSize, payload, size);
	return send_all(s, p.data(), p.size());
}

bool recv_packet(SockHandle s, std::vector<uint8_t> &payload, std::string &error) {
	uint8_t h[kHeaderSize];
	if (!recv_exact(s, h, sizeof(h))) {
		error = "connection closed or timed out";
		return false;
	}
	if (std::memcmp(h, kMagic, 4) != 0) {
		error = "not a retail admin packet";
		return false;
	}
	const uint32_t total = h[4] | (h[5] << 8) | (h[6] << 16) | (static_cast<uint32_t>(h[7]) << 24);
	if (total < kHeaderSize || total > kMaxPacket) {
		error = "bad admin packet length";
		return false;
	}
	payload.resize(total - kHeaderSize);
	if (!payload.empty() && !recv_exact(s, payload.data(), payload.size())) {
		error = "connection closed mid-packet";
		return false;
	}
	return true;
}

bool recv_text(SockHandle s, std::string &text, std::string &error) {
	std::vector<uint8_t> p;
	if (!recv_packet(s, p, error)) return false;
	if (p.empty() || p.back() != 0) {
		error = "admin reply is not NUL-terminated";
		return false;
	}
	text.assign(p.begin(), p.end() - 1);
	return true;
}

bool command(SockHandle s, const char *cmd, std::string &reply, std::string &error) {
	const size_t n = std::strlen(cmd) + 1; // with its NUL
	if (!send_packet(s, reinterpret_cast<const uint8_t *>(cmd), n)) {
		error = std::string("send failed: ") + cmd;
		return false;
	}
	if (!recv_text(s, reply, error)) return false;
	const std::string head = lower(trim(reply));
	if (head.rfind("error", 0) == 0 || head.rfind("usage", 0) == 0) {
		error = std::string(cmd) + ": " + trim(reply);
		return false;
	}
	return true;
}

void close_abortive(SockHandle s) {
	// Retail mishandles an orderly FIN: the admin client slot is kept, and a
	// later table growth corrupts it (WolfRAT2's SocketTransport). A reset
	// takes retail's working removal path.
	linger l{};
	l.l_onoff = 1;
	l.l_linger = 0;
	setsockopt(s, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char *>(&l), sizeof(l));
	close_sock(s);
}

SockHandle connect_with_timeout(const Addr &a, int timeout_ms, std::string &error) {
	SockHandle s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s == kInvalidSock) {
		error = "socket() failed";
		return kInvalidSock;
	}
	sockaddr_in sa{};
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = a.ip_be;
	sa.sin_port = htons(a.port);
	set_nonblocking(s, true);
	const int rc = ::connect(s, reinterpret_cast<sockaddr *>(&sa), sizeof(sa));
	if (rc != 0 && !connect_in_progress()) {
		error = "connect refused";
		close_sock(s);
		return kInvalidSock;
	}
	if (rc != 0) {
		fd_set w, e;
		FD_ZERO(&w);
		FD_ZERO(&e);
		FD_SET(s, &w);
		FD_SET(s, &e);
		timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
		int soerr = 0;
		SockLen len = sizeof(soerr);
		if (::select(select_nfds(s), nullptr, &w, &e, &tv) <= 0 || !FD_ISSET(s, &w) ||
		    getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&soerr), &len) != 0 || soerr != 0) {
			error = "connect failed or timed out";
			close_sock(s);
			return kInvalidSock;
		}
	}
	set_nonblocking(s, false);
	set_io_timeout(s, kIoTimeoutMs);
	return s;
}

} // namespace

std::array<uint8_t, 65> admin_login_response(const uint8_t *challenge, size_t size, const std::string &user,
                                             const std::string &password) {
	std::array<uint8_t, 65> buf{};
	std::memcpy(buf.data(), user.data(), std::min<size_t>(user.size(), 32));
	std::memcpy(buf.data() + 32, password.data(), std::min<size_t>(password.size(), 32));

	size_t key_len = 0;
	while (key_len < size && challenge[key_len] != 0) ++key_len;
	if (key_len == 0) key_len = size;
	if (key_len == 0) return buf;

	uint32_t hash = 0;
	for (size_t i = 0; i < key_len; ++i) {
		const int32_t v = static_cast<int8_t>(challenge[i]);
		hash += static_cast<uint32_t>(v * v) + static_cast<uint32_t>(i);
	}
	hash += 0x32u + static_cast<uint32_t>(key_len);

	const uint32_t m = opennova::NWU_LCG_MAGIC;
	const uint32_t s1 = (hash * m + 1) & 0xFFFF;
	const uint32_t s2 = (m * s1 + 1) & 0xFFFF;
	const uint32_t s3 = (m * s2 + 1) & 0xFFFF;

	for (size_t i = 0; i < buf.size(); ++i) buf[i] = static_cast<uint8_t>(buf[i] + challenge[i % key_len]);
	if (s3 & 1) std::reverse(buf.begin(), buf.end());

	uint8_t add = static_cast<uint8_t>(s1);
	const uint8_t step = static_cast<uint8_t>(s2);
	for (size_t i = 0; i < buf.size(); ++i) {
		buf[i] = static_cast<uint8_t>(buf[i] + i + add);
		add = static_cast<uint8_t>(add + step);
	}

	uint32_t state = s3;
	for (auto &b : buf) {
		state = (m * state + 1) & 0xFFFF;
		b = static_cast<uint8_t>(b + (state & 0xFF));
	}
	return buf;
}

std::vector<AdminPlayer> parse_admin_players(const std::string &reply) {
	std::vector<AdminPlayer> out;
	for (const auto &raw : split_lines(reply)) {
		const std::string line = trim(raw);
		if (line.empty() || line.find_first_not_of("-\t ") == std::string::npos) continue;
		const auto cols = split_tabs(line);
		if (cols.size() < 3 || !all_digits(cols[1])) continue; // header row and stray text
		AdminPlayer p;
		p.name = cols[0];
		p.slot = std::atoi(cols[1].c_str());
		p.team = cols[2];
		if (p.name.empty() || (p.slot == 0 && p.name == "Host")) continue;
		out.push_back(std::move(p));
	}
	return out;
}

std::string parse_admin_current_mission(const std::string &reply) {
	for (const auto &raw : split_lines(reply)) {
		if (lower(raw).find("<current mission>") == std::string::npos) continue;
		const size_t colon = raw.find(':');
		if (colon == std::string::npos) continue;
		const std::string rest = trim(raw.substr(colon + 1));
		const std::string low = lower(rest);
		size_t best = std::string::npos;
		for (const char *ext : {".bms", ".npj", ".npz"}) {
			const size_t at = low.find(ext);
			if (at != std::string::npos && at < best) best = at;
		}
		if (best != std::string::npos && best > 0) return rest.substr(0, best);
	}
	return {};
}

int parse_admin_time_left(const std::string &reply) {
	for (const auto &raw : split_lines(reply)) {
		const size_t eq = raw.find('=');
		if (eq == std::string::npos || lower(trim(raw.substr(0, eq))) != "gametime") continue;
		const std::string v = trim(raw.substr(eq + 1));
		const size_t slash = v.find('/');
		if (slash == std::string::npos) return -1;
		const std::string left = trim(v.substr(0, slash)), total = trim(v.substr(slash + 1));
		if (!all_digits(left) || !all_digits(total)) return -1;
		const int remaining = std::atoi(left.c_str());
		return (std::atoi(total.c_str()) > 0 && remaining > 0) ? remaining : -1;
	}
	return -1;
}

// --- AdminFeed ----------------------------------------------------------------

void AdminFeed::start(std::string host, uint16_t port, std::string user, std::string password, int poll_seconds) {
	stop();
	host_ = std::move(host);
	port_ = port;
	user_ = std::move(user);
	password_ = std::move(password);
	poll_seconds_ = poll_seconds < 2 ? 2 : poll_seconds;
	stop_ = false;
	thread_ = std::thread([this] { run(); });
}

void AdminFeed::stop() {
	stop_ = true;
	const intptr_t s = sock_.load();
	if (s != -1) ::shutdown(static_cast<SockHandle>(s), 2); // cut a blocked read short
	if (thread_.joinable()) thread_.join();
}

AdminSnapshot AdminFeed::snapshot() const {
	std::lock_guard<std::mutex> lock(mu_);
	return snap_;
}

void AdminFeed::publish(AdminSnapshot next) {
	std::lock_guard<std::mutex> lock(mu_);
	auto same_players = [](const std::vector<AdminPlayer> &a, const std::vector<AdminPlayer> &b) {
		if (a.size() != b.size()) return false;
		for (size_t i = 0; i < a.size(); ++i)
			if (a[i].slot != b[i].slot || a[i].name != b[i].name || a[i].team != b[i].team) return false;
		return true;
	};
	const bool changed = next.ok != snap_.ok || next.mission != snap_.mission ||
	                     next.time_left_minutes != snap_.time_left_minutes || !same_players(next.players, snap_.players);
	next.seq = snap_.seq + ((changed || snap_.seq == 0) ? 1 : 0); // the first poll always counts
	snap_ = std::move(next);
}

void AdminFeed::run() {
	SockHandle s = kInvalidSock;
	auto drop = [&] {
		if (s == kInvalidSock) return;
		sock_ = -1;
		close_abortive(s);
		s = kInvalidSock;
	};
	while (!stop_) {
		AdminSnapshot next;
		std::string error;
		do {
			if (s == kInvalidSock) {
				Addr a;
				if (!resolve(host_, port_, a)) {
					error = "cannot resolve " + host_;
					break;
				}
				s = connect_with_timeout(a, 5000, error);
				if (s == kInvalidSock) break;
				sock_ = static_cast<intptr_t>(s);
				std::vector<uint8_t> challenge;
				if (!recv_packet(s, challenge, error)) break;
				if (challenge.size() != 33 || challenge.back() != 0) {
					error = "unexpected admin challenge";
					break;
				}
				const auto login = admin_login_response(challenge.data(), challenge.size(), user_, password_);
				std::string reply;
				if (!send_packet(s, login.data(), login.size()) || !recv_text(s, reply, error)) {
					if (error.empty()) error = "login send failed";
					break;
				}
				if (lower(reply).find("logged in") == std::string::npos) {
					error = "admin login refused: " + trim(reply);
					break;
				}
				logf("[admin] logged in to %s:%u", host_.c_str(), port_);
			}
			std::string players, missions, settings;
			if (!command(s, "PLAYER LIST", players, error) || !command(s, "MISSION LIST", missions, error) ||
			    !command(s, "GET GAMESETTINGS", settings, error))
				break;
			next.ok = true;
			next.players = parse_admin_players(players);
			next.mission = parse_admin_current_mission(missions);
			next.time_left_minutes = parse_admin_time_left(settings);
		} while (false);
		if (!next.ok) {
			drop();
			next.status = error;
		}
		publish(std::move(next));
		for (int i = 0; i < poll_seconds_ * 10 && !stop_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	drop();
}

} // namespace opennova::lister
