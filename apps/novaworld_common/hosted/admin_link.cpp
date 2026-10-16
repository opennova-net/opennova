#include "admin_link.h"

#include "loggable.h"

#include <base/io/cp1252.h>
#include <base/io/strutil.h>
#include <net/admin/admin_protocol.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <exception>
#include <string>
#include <utility>
#include <variant>

namespace opennova::novaworld_server::hosted {

namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

// The status poll: one reply, `OK - Current State = ` and the scene (Menus, Game or
// Unknown). [orig: CAdminServer_HandleGet @0x4033C4..0x40340B]
constexpr const char *kStatusCommand = "GET GAMESTATE";
constexpr const char *kGameStatePrefix = "OK - Current State = ";

std::string utc_now() {
	const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
	std::tm tm{};
#ifdef _WIN32
	gmtime_s(&tm, &now);
#else
	gmtime_r(&now, &tm);
#endif
	char text[32];
	std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &tm);
	return text;
}

bool send_packet(net::Socket &socket, const std::vector<uint8_t> &payload) {
	const std::vector<uint8_t> packet = admin_encode_packet(payload.data(), payload.size());
	return net::tcp_send_all(socket, packet.data(), packet.size());
}

std::string seconds_text(int ms) {
	return std::to_string(ms / 1000) + "." + std::to_string((ms % 1000) / 100) + " s";
}

} // namespace

const char *admin_link_state_name(AdminLinkState state) {
	switch (state) {
		case AdminLinkState::Connecting: return "connecting";
		case AdminLinkState::Up: return "up";
		case AdminLinkState::Down: return "down";
		case AdminLinkState::Stopped: return "stopped";
	}
	return "down";
}

std::optional<AdminLinkState> parse_admin_link_state(std::string_view text) {
	for (const AdminLinkState state : { AdminLinkState::Connecting, AdminLinkState::Up,
				 AdminLinkState::Down, AdminLinkState::Stopped }) {
		if (text == admin_link_state_name(state))
			return state;
	}
	return std::nullopt;
}

std::optional<AdminCommandOutcome> parse_admin_command_outcome(std::string_view text) {
	for (const AdminCommandOutcome outcome :
			{ AdminCommandOutcome::Answered, AdminCommandOutcome::Refused,
					AdminCommandOutcome::LinkDown, AdminCommandOutcome::Busy,
					AdminCommandOutcome::TimedOut, AdminCommandOutcome::Stopped }) {
		if (text == admin_command_outcome_name(outcome))
			return outcome;
	}
	return std::nullopt;
}

const char *admin_command_outcome_name(AdminCommandOutcome outcome) {
	switch (outcome) {
		case AdminCommandOutcome::Answered: return "answered";
		case AdminCommandOutcome::Refused: return "refused";
		case AdminCommandOutcome::LinkDown: return "link_down";
		case AdminCommandOutcome::Busy: return "busy";
		case AdminCommandOutcome::TimedOut: return "timed_out";
		case AdminCommandOutcome::Stopped: return "stopped";
	}
	return "link_down";
}

AdminLink::AdminLink(int64_t server_id, AdminLinkTarget target, db::ConnectionPool *pool, AdminLinkConfig config) :
		server_id_(server_id), target_(std::move(target)), pool_(pool), config_(std::move(config)) {
	add_log_secret(target_.password);
}

AdminLink::~AdminLink() {
	request_stop();
	join();
	remove_log_secret(target_.password);
}

void AdminLink::start() {
	std::lock_guard<std::mutex> lock(join_mu_);
	if (thread_.joinable() || finished_.load()) return;
	thread_ = std::thread([this] { thread_main(); });
}

void AdminLink::request_stop() {
	{
		std::lock_guard<std::mutex> lock(mu_);
		stop_ = true;
	}
	cv_.notify_all();
}

void AdminLink::join() {
	std::lock_guard<std::mutex> lock(join_mu_);
	if (thread_.joinable()) thread_.join();
}

AdminLinkStatus AdminLink::status() const {
	std::lock_guard<std::mutex> lock(mu_);
	return status_;
}

std::vector<ConsoleLine> AdminLink::recent() const {
	std::lock_guard<std::mutex> lock(mu_);
	return std::vector<ConsoleLine>(ring_.begin(), ring_.end());
}

AdminCommandResult AdminLink::run(const std::string &command, const AdminCommandOptions &options,
		milliseconds wait) {
	AdminCommandResult out;
	// One packet's line: the server's buffer holds it with its header and NUL, and a control
	// byte has no place in a command (the dispatcher would split on a tab or a line end).
	const bool control = std::any_of(command.begin(), command.end(), [](char c) {
		const auto byte = static_cast<unsigned char>(c);
		return byte < 0x20 || byte == 0x7F;
	});
	// QUIT (the one command that draws no reply: the server closes) is the link's own, which
	// ends the session when the link ends. [orig: CAdminServer_DispatchCommand @0x406811]
	const AdminReplyPlan plan = admin_reply_plan(command);
	const bool quit = plan.framing == AdminReplyFraming::Exact && plan.count == 0;
	if (command.empty() || command.size() > kCommandMaxBytes || control || quit) {
		out.outcome = AdminCommandOutcome::Refused;
		return out;
	}
	auto pending = std::make_shared<Pending>();
	pending->command = command;
	pending->options = options;
	{
		std::lock_guard<std::mutex> lock(mu_);
		if (stop_) {
			out.outcome = AdminCommandOutcome::Stopped;
			return out;
		}
		if (status_.state != AdminLinkState::Up) {
			out.outcome = AdminCommandOutcome::LinkDown;
			return out;
		}
		if (queue_.size() >= config_.queue_max) {
			out.outcome = AdminCommandOutcome::Busy;
			return out;
		}
		queue_.push_back(pending);
	}
	cv_.notify_all();
	std::unique_lock<std::mutex> lock(pending->mu);
	if (pending->cv.wait_for(lock, wait, [&pending] { return pending->done; })) return pending->result;
	// A command still queued never goes; one already sent finishes, its replies in the console.
	pending->cancelled = !pending->sent;
	out.outcome = AdminCommandOutcome::TimedOut;
	out.sent = pending->sent;
	return out;
}

void AdminLink::complete(Pending &pending, AdminCommandOutcome outcome, std::vector<std::string> replies) {
	{
		std::lock_guard<std::mutex> lock(pending.mu);
		pending.result.outcome = outcome;
		pending.result.sent = pending.sent;
		pending.result.replies = std::move(replies);
		pending.done = true;
	}
	pending.cv.notify_all();
}

void AdminLink::fail_queued(AdminCommandOutcome outcome) {
	std::deque<std::shared_ptr<Pending>> failed;
	{
		std::lock_guard<std::mutex> lock(mu_);
		failed.swap(queue_);
	}
	for (const auto &pending : failed) complete(*pending, outcome, {});
}

void AdminLink::set_state(AdminLinkState state, const std::string &error) {
	{
		std::lock_guard<std::mutex> lock(mu_);
		status_.state = state;
		status_.error = error;
		if (state == AdminLinkState::Up)
			status_.up_since = utc_now();
		else
			status_.up_since.clear();
	}
	if (state != AdminLinkState::Up)
		fail_queued(state == AdminLinkState::Stopped ? AdminCommandOutcome::Stopped : AdminCommandOutcome::LinkDown);
}

bool AdminLink::wait_stop(int ms) {
	std::unique_lock<std::mutex> lock(mu_);
	return cv_.wait_for(lock, milliseconds(ms), [this] { return stop_; });
}

void AdminLink::log(const std::string &line) const {
	const std::string text = mask_log_secrets("[hosted] admin link server " + std::to_string(server_id_) + " (" +
			target_.host + ":" + std::to_string(target_.port) + "): " + line);
	if (config_.log)
		config_.log(text);
	else
		std::printf("%s\n", text.c_str());
}

// A console line: the server's cp1252 as UTF-8 (the link's own lines are ASCII), the admin
// password masked wherever it might show (it never should).
void AdminLink::record(const char *kind, const std::string &actor, const std::string &text) {
	ConsoleLine line;
	line.kind = kind;
	line.actor = actor;
	std::string utf8 = cp1252_to_utf8(text);
	const std::string &secret = target_.password;
	if (secret.size() >= 8) {
		for (size_t at = utf8.find(secret); at != std::string::npos; at = utf8.find(secret, at + 3))
			utf8.replace(at, secret.size(), "***");
	}
	line.text = mask_log_secrets(std::move(utf8));
	line.at = utc_now();
	std::lock_guard<std::mutex> lock(mu_);
	ring_.push_back(line);
	while (ring_.size() > config_.ring_max) ring_.pop_front();
	if (pool_ == nullptr) return;
	unwritten_.push_back(std::move(line));
	if (unwritten_.size() > config_.ring_max)
		unwritten_.erase(unwritten_.begin(), unwritten_.begin() + (unwritten_.size() - config_.ring_max));
}

void AdminLink::flush_console() {
	if (pool_ == nullptr) return;
	std::vector<ConsoleLine> lines;
	{
		std::lock_guard<std::mutex> lock(mu_);
		if (unwritten_.empty()) return;
		lines.swap(unwritten_);
	}
	try {
		auto lease = pool_->acquire();
		append_console(*lease, server_id_, lines);
	} catch (const std::exception &e) {
		// Kept for the next flush (the newest ring_max of them).
		log(std::string("cannot write the console: ") + e.what());
		std::lock_guard<std::mutex> lock(mu_);
		unwritten_.insert(unwritten_.begin(), lines.begin(), lines.end());
		if (unwritten_.size() > config_.ring_max)
			unwritten_.erase(unwritten_.begin(), unwritten_.begin() + (unwritten_.size() - config_.ring_max));
	}
}

// One packet's payload text: the 8-byte header (the marker and the total length), then the
// payload, its NUL dropped (a reply without one is taken as it is).
// [orig: CAdminServer_SendResponse @0x402D40 @0x402D6B..0x402D85]
bool AdminLink::read_packet(std::string &text, std::string &error) {
	uint8_t header[ADMIN_PACKET_HEADER_BYTES];
	if (!net::tcp_recv_exact(socket_, header, sizeof(header))) {
		error = "the server closed the connection or stopped answering";
		return false;
	}
	size_t size = 0;
	if (!admin_decode_header(header, size)) {
		error = "the server sent something that is no remote-admin packet";
		return false;
	}
	std::vector<uint8_t> payload(size);
	if (size != 0 && !net::tcp_recv_exact(socket_, payload.data(), size)) {
		error = "the connection closed in the middle of a packet";
		return false;
	}
	admin_reply_text(payload, text);
	return true;
}

// Connect and log in, as RAT.exe does: the challenge first (it keys the login), then the
// login encrypted under it. A refused login gets no reply: the server closes.
// [orig: CAdminServer_AcceptConnection @0x405580 (an ip_restrict miss closes before the
//  challenge); CAdminServer_HandleLogin @0x405870 (the failure returns 0, the slot closes;
//  success replies @0x405A0C)] [orig: RAT.exe @0x401323 (the challenge's byte 0)]
bool AdminLink::log_in(std::string &error) {
	net::Endpoint endpoint;
	if (!net::resolve_ipv4(target_.host, endpoint, /*allow_names=*/false) || target_.port == 0) {
		error = "the machine's admin address is not an IPv4 address and port";
		return false;
	}
	endpoint.port = target_.port;
	socket_ = net::tcp_connect(endpoint, config_.io_timeout_ms);
	if (!socket_.is_valid()) {
		error = "cannot connect to the server's admin port";
		return false;
	}
	uint8_t header[ADMIN_PACKET_HEADER_BYTES];
	size_t size = 0;
	std::vector<uint8_t> challenge;
	if (!net::tcp_recv_exact(socket_, header, sizeof(header)) || !admin_decode_header(header, size) ||
			size > ADMIN_SERVER_PACKET_MAX_BYTES) {
		error = "no challenge from the server (admin.cfg's ip_restrict may not admit the service)";
		return false;
	}
	challenge.resize(size);
	if (size != 0 && !net::tcp_recv_exact(socket_, challenge.data(), size)) {
		error = "the connection closed in the middle of the challenge";
		return false;
	}
	if (!admin_challenge_valid(challenge)) {
		error = "the server's first packet is no remote-admin challenge";
		return false;
	}
	const auto login = admin_encode_login(challenge, target_.user, target_.password);
	if (!send_packet(socket_, std::vector<uint8_t>(login.begin(), login.end()))) {
		error = "the login could not be sent";
		return false;
	}
	std::string reply;
	std::string ignored;
	if (!read_packet(reply, ignored)) {
		error = "the server refused the login";
		return false;
	}
	if (!admin_login_accepted(reply)) {
		error = "the server answered the login with something else";
		return false;
	}
	return true;
}

// The server keeps a slot whose client closed with a FIN (D-NET-360) and corrupts its slot
// table once six are held, so a logged-in session ends with QUIT, the server closing first,
// and anything else with a reset (which also spares the server's side a TIME_WAIT on its admin
// port). [orig: CAdminServer_ProcessFrame @0x406FA3 (recv() == 0 is not a close);
//  CAdminServer_DispatchCommand @0x406811 (QUIT returns 0)] [orig: RAT.exe @0x401475..0x40148D]
void AdminLink::close_session(bool logged_in) {
	if (!socket_.is_valid()) return;
	if (logged_in && send_packet(socket_, admin_encode_command(ADMIN_COMMAND_QUIT))) {
		const auto deadline = Clock::now() + milliseconds(config_.quit_timeout_ms);
		uint8_t drain[256];
		while (Clock::now() < deadline) {
			const int ready = net::tcp_wait_readable(socket_, 50);
			if (ready < 0) break;
			if (ready == 0) continue;
			const int n = net::tcp_recv(socket_, drain, sizeof(drain));
			if (n == 0) {
				net::close_socket(socket_);
				return;
			}
			if (n < 0) break;
		}
	}
	net::close_socket_reset(socket_);
}

bool AdminLink::drain_unsolicited(std::string &error) {
	for (;;) {
		const int ready = net::tcp_wait_readable(socket_, 0);
		if (ready < 0) {
			error = "the connection failed";
			return false;
		}
		if (ready == 0) return true;
		std::string text;
		if (!read_packet(text, error)) return false;
		// A stray packet belongs to the command before it, as far as anything can tell: it goes
		// to the console only when that command was recorded (a read the site polls is not,
		// and neither are its replies).
		log("a stray packet, after a command's counted replies (" + std::to_string(text.size()) + " bytes)");
		if (last_command_recorded_) record("status", "", "a stray reply after the last command: " + text);
	}
}

// The replies by the dispatcher's count (admin_reply_plan): each one a count names is waited
// for up to the I/O timeout, and its absence ends the session (the stream can no longer be
// trusted); the QuietGap framing reads until the server stays quiet for the gap.
bool AdminLink::exchange(const std::string &command, std::vector<std::string> &replies, std::string &error) {
	if (!drain_unsolicited(error)) return false;
	if (!send_packet(socket_, admin_encode_command(command))) {
		error = "the command could not be sent";
		return false;
	}
	const AdminReplyPlan plan = admin_reply_plan(command);
	while (!admin_replies_complete(plan, replies)) {
		const bool gap = plan.framing == AdminReplyFraming::QuietGap;
		const int ready = net::tcp_wait_readable(socket_, gap ? config_.quiet_gap_ms : config_.io_timeout_ms);
		if (ready < 0) {
			error = "the connection failed";
			return false;
		}
		if (ready == 0) {
			if (gap) return true;
			error = "no reply in " + seconds_text(config_.io_timeout_ms);
			return false;
		}
		std::string text;
		if (!read_packet(text, error)) return false;
		replies.push_back(std::move(text));
	}
	return true;
}

bool AdminLink::poll_status(std::string &error) {
	std::vector<std::string> replies;
	const bool ok = exchange(kStatusCommand, replies, error);
	last_command_recorded_ = false;
	if (!ok) return false;
	std::string scene = "Unknown";
	if (!replies.empty() && strutil::starts_with_icase(replies[0], kGameStatePrefix))
		scene = replies[0].substr(std::char_traits<char>::length(kGameStatePrefix));
	bool changed = false;
	{
		std::lock_guard<std::mutex> lock(mu_);
		changed = scene != status_.game_state;
		status_.game_state = scene;
		status_.last_poll_at = utc_now();
	}
	if (changed) record("status", "", "the server's scene: " + scene);
	return true;
}

void AdminLink::execute(Pending &pending, bool &session_ok, std::string &error) {
	{
		std::lock_guard<std::mutex> lock(pending.mu);
		if (pending.cancelled) return; // its caller stopped waiting before it went
		pending.sent = true;
	}
	if (pending.options.record) record("command", pending.options.actor, pending.command);
	std::vector<std::string> replies;
	session_ok = exchange(pending.command, replies, error);
	last_command_recorded_ = pending.options.record;
	if (pending.options.record) {
		for (const std::string &reply : replies) record("reply", "", reply);
		// Written before the caller hears back, so a console read after the
		// command's answer holds it.
		flush_console();
	}
	complete(pending, session_ok ? AdminCommandOutcome::Answered : AdminCommandOutcome::LinkDown,
			std::move(replies));
}

void AdminLink::thread_main() {
	int backoff = config_.reconnect_min_ms;
	bool logged_in = false;
	auto next_poll = Clock::now();
	try {
		for (;;) {
			{
				std::lock_guard<std::mutex> lock(mu_);
				if (stop_) break;
			}
			if (!logged_in) {
				set_state(AdminLinkState::Connecting, {});
				std::string error;
				logged_in = log_in(error);
				if (!logged_in) {
					close_session(false);
					set_state(AdminLinkState::Down, error);
					// A refusal that repeats is recorded once, not at every retry.
					if (error != last_recorded_error_) {
						last_recorded_error_ = error;
						log("cannot log in: " + error + "; retrying");
						record("status", "", "the admin login failed: " + error);
					}
					flush_console();
					if (wait_stop(backoff)) break;
					backoff = std::min(backoff * 2, config_.reconnect_max_ms);
					continue;
				}
				backoff = config_.reconnect_min_ms;
				last_recorded_error_.clear();
				last_command_recorded_ = false;
				set_state(AdminLinkState::Up, {});
				log("logged in as " + target_.user);
				record("status", "", "the admin session is up");
				next_poll = Clock::now();
			}
			// Idle until a command, the status poll's turn or the stop, looking at the socket
			// every idle_check_ms so a server that went away is noticed (and reset) at once.
			std::shared_ptr<Pending> item;
			{
				std::unique_lock<std::mutex> lock(mu_);
				const auto until = std::min(next_poll, Clock::now() + milliseconds(config_.idle_check_ms));
				cv_.wait_until(lock, until, [this] { return stop_ || !queue_.empty(); });
				if (stop_) break;
				if (!queue_.empty()) {
					item = queue_.front();
					queue_.pop_front();
				}
			}
			std::string error;
			bool ok = true;
			if (item) {
				execute(*item, ok, error);
			} else if (Clock::now() >= next_poll) {
				ok = poll_status(error);
				next_poll = Clock::now() + milliseconds(config_.status_interval_ms);
			} else {
				ok = drain_unsolicited(error);
			}
			if (!ok) {
				close_session(false);
				logged_in = false;
				set_state(AdminLinkState::Down, error);
				log("lost the session: " + error);
				record("status", "", "the admin session was lost: " + error);
				last_recorded_error_ = error;
				flush_console();
				if (wait_stop(config_.reconnect_min_ms)) break;
				continue;
			}
			flush_console();
		}
	} catch (const std::exception &e) {
		log(std::string("ended by an error: ") + e.what());
	}
	if (logged_in) record("status", "", "the admin session is closed");
	close_session(logged_in);
	set_state(AdminLinkState::Stopped, {});
	flush_console();
	finished_.store(true);
}

AdminLinks::AdminLinks(db::ConnectionPool *pool, AdminLinkConfig config) : pool_(pool), config_(std::move(config)) {}

AdminLinks::~AdminLinks() { stop_all(); }

void AdminLinks::sync(const std::map<int64_t, AdminLinkTarget> &wanted) {
	std::lock_guard<std::mutex> lock(mu_);
	for (auto it = ending_.begin(); it != ending_.end();) {
		if ((*it)->finished()) {
			(*it)->join();
			it = ending_.erase(it);
		} else {
			++it;
		}
	}
	for (auto it = links_.begin(); it != links_.end();) {
		const auto want = wanted.find(it->first);
		if (want != wanted.end() && want->second == it->second->target() && !it->second->finished()) {
			++it;
			continue;
		}
		it->second->request_stop();
		ending_.push_back(it->second);
		it = links_.erase(it);
	}
	for (const auto &entry : wanted) {
		if (links_.count(entry.first) != 0) continue;
		auto link = std::make_shared<AdminLink>(entry.first, entry.second, pool_, config_);
		link->start();
		links_.emplace(entry.first, std::move(link));
	}
}

std::shared_ptr<AdminLink> AdminLinks::find(int64_t server_id) const {
	std::lock_guard<std::mutex> lock(mu_);
	const auto it = links_.find(server_id);
	return it == links_.end() ? nullptr : it->second;
}

bool AdminLinks::answering(int64_t server_id) const {
	const std::shared_ptr<AdminLink> link = find(server_id);
	return link != nullptr && link->status().state == AdminLinkState::Up;
}

void AdminLinks::publish(db::Database &db) {
	std::map<int64_t, AdminLinkStatus> now;
	{
		std::lock_guard<std::mutex> lock(mu_);
		for (const auto &[id, link] : links_)
			now.emplace(id, link->status());
	}
	std::lock_guard<std::mutex> publishing(publish_mu_);
	const auto at = [](const std::string &text) {
		return text.empty() ? db::BindValue(std::monostate{}) : db::BindValue(text);
	};
	std::string ids = "{";
	for (const auto &[id, status] : now) {
		ids += (ids.size() > 1 ? "," : "") + std::to_string(id);
		const auto last = published_.find(id);
		if (last != published_.end() && last->second.state == status.state &&
				last->second.game_state == status.game_state &&
				last->second.last_poll_at == status.last_poll_at &&
				last->second.up_since == status.up_since && last->second.error == status.error)
			continue;
		db.exec("INSERT INTO hosted_admin_links (server_id, state, game_state, last_poll_at, "
				"up_since_at, error, "
				"seen_at) VALUES ($1, $2, $3, $4::timestamptz, $5::timestamptz, $6, now()) "
				"ON CONFLICT (server_id) DO UPDATE SET state = EXCLUDED.state, game_state = "
				"EXCLUDED.game_state, "
				"last_poll_at = EXCLUDED.last_poll_at, up_since_at = EXCLUDED.up_since_at, error = "
				"EXCLUDED.error, "
				"seen_at = now();",
				{ db::BindValue(id),
						db::BindValue(std::string(admin_link_state_name(status.state))),
						db::BindValue(status.game_state), at(status.last_poll_at),
						at(status.up_since), db::BindValue(status.error) });
	}
	ids += "}";
	published_ = std::move(now);
	db.exec("DELETE FROM hosted_admin_links WHERE server_id <> ALL($1::bigint[]);",
			{ db::BindValue(ids) });
	const auto steady = std::chrono::steady_clock::now();
	if (steady - refreshed_ >= std::chrono::milliseconds(kRefreshMs)) {
		refreshed_ = steady;
		db.exec("UPDATE hosted_admin_links SET seen_at = now();");
	}
}

std::optional<AdminLinkStatus> admin_link_status(db::Database &db, int64_t server_id) {
	const auto rows = db.query(
			"SELECT state, game_state, last_poll_at, up_since_at, error FROM hosted_admin_links "
			"WHERE server_id = $1 AND seen_at > now() - make_interval(secs => $2);",
			{ db::BindValue(server_id), db::BindValue(static_cast<double>(kAdminLinkStaleS)) });
	if (rows.empty())
		return std::nullopt;
	const db::Row &r = rows.front();
	AdminLinkStatus status;
	status.state = parse_admin_link_state(r.as_text(0).value_or("")).value_or(AdminLinkState::Down);
	status.game_state = r.as_text(1).value_or("");
	status.last_poll_at = r.as_text(2).value_or("");
	status.up_since = r.as_text(3).value_or("");
	status.error = r.as_text(4).value_or("");
	return status;
}

void AdminLinks::stop_all() {
	std::vector<std::shared_ptr<AdminLink>> all;
	{
		std::lock_guard<std::mutex> lock(mu_);
		for (auto &entry : links_) all.push_back(entry.second);
		links_.clear();
		all.insert(all.end(), ending_.begin(), ending_.end());
		ending_.clear();
	}
	for (const auto &link : all) link->request_stop();
	for (const auto &link : all) link->join();
}

} // namespace opennova::novaworld_server::hosted
