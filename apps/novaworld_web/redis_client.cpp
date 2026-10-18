#include "redis_client.h"

#include <net/novaworld/db/database.h>

#include <base/io/strutil.h>

#include <hiredis/hiredis.h>
#include <hiredis/hiredis_ssl.h>
#include <openssl/ssl.h>

#include <cerrno>
#include <optional>
#include <utility>

namespace opennova::novaworld_server {

namespace {

// What a URL names (redis://[user[:password]@]host[:port][/db], rediss:// for
// TLS; an IPv6 host in brackets).
struct Endpoint {
	bool tls = false;
	std::string host;
	int port = 6379;
	int db = 0;
	std::string user;
	std::string password;
};

std::optional<Endpoint> parse_url(std::string_view url) {
	Endpoint e;
	if (url.substr(0, 8) == "redis://") {
		url.remove_prefix(8);
	} else if (url.substr(0, 9) == "rediss://") {
		e.tls = true;
		url.remove_prefix(9);
	} else {
		return std::nullopt;
	}
	const size_t slash = url.find('/');
	std::string_view authority = url.substr(0, slash);
	if (slash != std::string_view::npos) {
		const std::string_view path = url.substr(slash + 1);
		if (!path.empty()) {
			const auto db = strutil::all_digits(path) ? strutil::parse_int(std::string(path))
													  : std::nullopt;
			if (!db)
				return std::nullopt;
			e.db = *db;
		}
	}
	if (const size_t at = authority.rfind('@'); at != std::string_view::npos) {
		const std::string_view userinfo = authority.substr(0, at);
		const size_t colon = userinfo.find(':');
		e.user = std::string(userinfo.substr(0, colon));
		if (colon != std::string_view::npos)
			e.password = std::string(userinfo.substr(colon + 1));
		authority.remove_prefix(at + 1);
	}
	std::string_view port;
	if (!authority.empty() && authority.front() == '[') {
		const size_t close = authority.find(']');
		if (close == std::string_view::npos)
			return std::nullopt;
		e.host = std::string(authority.substr(1, close - 1));
		const std::string_view rest = authority.substr(close + 1);
		if (!rest.empty()) {
			if (rest.front() != ':')
				return std::nullopt;
			port = rest.substr(1);
		}
	} else {
		const size_t colon = authority.find(':');
		e.host = std::string(authority.substr(0, colon));
		if (colon != std::string_view::npos)
			port = authority.substr(colon + 1);
	}
	if (e.host.empty())
		return std::nullopt;
	if (!port.empty()) {
		const auto n =
				strutil::all_digits(port) ? strutil::parse_int(std::string(port)) : std::nullopt;
		if (!n || *n < 1 || *n > 65535)
			return std::nullopt;
		e.port = *n;
	}
	return e;
}

timeval to_timeval(int ms) {
	timeval tv{};
	tv.tv_sec = ms / 1000;
	tv.tv_usec = (ms % 1000) * 1000;
	return tv;
}

// The reply's value, owned; an error reply throws (the connection stays up).
RedisReply take_reply(const redisReply *r, const char *what) {
	RedisReply out;
	switch (r->type) {
		case REDIS_REPLY_ERROR:
			throw RedisError(
					std::string("redis: ") + what + ": " + std::string(r->str, r->len), false);
		case REDIS_REPLY_INTEGER:
			out.type = RedisReply::Type::Integer;
			out.integer = r->integer;
			break;
		case REDIS_REPLY_STRING:
		case REDIS_REPLY_STATUS:
			out.type = RedisReply::Type::String;
			out.string.assign(r->str, r->len);
			break;
		case REDIS_REPLY_ARRAY:
			out.type = RedisReply::Type::Array;
			out.elements.reserve(r->elements);
			for (size_t i = 0; i < r->elements; ++i)
				out.elements.push_back(take_reply(r->element[i], what));
			break;
		default: // REDIS_REPLY_NIL; RESP3's types never come (no HELLO 3)
			break;
	}
	return out;
}

} // namespace

RedisError::RedisError(const std::string &message, bool connection_lost) :
		std::runtime_error(message), connection_lost_(connection_lost) {}

RedisConnection::RedisConnection(const RedisOptions &options) : options_(options) {
	open();
}

RedisConnection::~RedisConnection() {
	close();
}

void RedisConnection::close() {
	if (ctx_)
		redisFree(ctx_);
	ctx_ = nullptr;
}

void RedisConnection::open() {
	close();
	const std::string where = db::masked_url(options_.url);
	const auto endpoint = parse_url(options_.url);
	if (!endpoint)
		throw RedisError("redis: not a redis:// or rediss:// URL: " + where, true);
	const timeval connect_timeout = to_timeval(options_.connect_timeout_ms);
	const timeval command_timeout = to_timeval(options_.command_timeout_ms);
	redisOptions opts{};
	REDIS_OPTIONS_SET_TCP(&opts, endpoint->host.c_str(), endpoint->port);
	opts.connect_timeout = &connect_timeout;
	opts.command_timeout = &command_timeout;
	redisContext *ctx = redisConnectWithOptions(&opts);
	auto fail = [&](const std::string &why) {
		if (ctx)
			redisFree(ctx);
		throw RedisError("redis: connect " + where + ": " + why, true);
	};
	if (!ctx)
		fail("out of memory");
	if (ctx->err)
		fail(ctx->errstr);
	redisEnableKeepAlive(ctx);

	if (endpoint->tls) {
		// The system's CAs (ElastiCache's certificates chain to Amazon's public
		// roots) and the URL's host checked against the certificate's names.
		SSL_CTX *ssl_ctx = SSL_CTX_new(TLS_client_method());
		if (!ssl_ctx)
			fail("TLS context");
		SSL_CTX_set_min_proto_version(ssl_ctx, TLS1_2_VERSION);
		SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_PEER, nullptr);
		SSL_CTX_set_default_verify_paths(ssl_ctx);
		SSL *ssl = SSL_new(ssl_ctx);
		SSL_CTX_free(ssl_ctx); // the SSL holds its own reference
		if (!ssl)
			fail("TLS session");
		SSL_set_tlsext_host_name(ssl, endpoint->host.c_str());
		SSL_set1_host(ssl, endpoint->host.c_str());
		// On success hiredis owns the SSL; on failure it is still ours.
		if (redisInitiateSSL(ctx, ssl) != REDIS_OK) {
			SSL_free(ssl);
			fail(std::string("TLS: ") + ctx->errstr);
		}
	}

	const std::string &password =
			endpoint->password.empty() ? options_.password : endpoint->password;
	auto setup = [&](std::vector<std::string> argv) {
		std::vector<const char *> args;
		std::vector<size_t> lens;
		for (const auto &a : argv) {
			args.push_back(a.data());
			lens.push_back(a.size());
		}
		auto *r = static_cast<redisReply *>(
				redisCommandArgv(ctx, static_cast<int>(args.size()), args.data(), lens.data()));
		if (!r)
			fail(argv[0] + ": " + ctx->errstr);
		const bool refused = r->type == REDIS_REPLY_ERROR;
		const std::string text = refused ? std::string(r->str, r->len) : std::string();
		freeReplyObject(r);
		if (refused)
			fail(argv[0] + " refused: " + text);
	};
	if (!password.empty()) {
		if (endpoint->user.empty())
			setup({ "AUTH", password });
		else
			setup({ "AUTH", endpoint->user, password });
	}
	if (endpoint->db != 0)
		setup({ "SELECT", std::to_string(endpoint->db) });
	ctx_ = ctx;
}

RedisReply RedisConnection::command(const std::vector<std::string> &argv, RedisResend resend) {
	std::vector<const char *> args;
	std::vector<size_t> lens;
	args.reserve(argv.size());
	lens.reserve(argv.size());
	for (const auto &a : argv) {
		args.push_back(a.data());
		lens.push_back(a.size());
	}
	const char *what = argv.empty() ? "?" : argv[0].c_str();
	for (int attempt = 0;; ++attempt) {
		if (!ctx_)
			open();
		auto *r = static_cast<redisReply *>(
				redisCommandArgv(ctx_, static_cast<int>(args.size()), args.data(), lens.data()));
		if (r) {
			struct Free {
				void operator()(redisReply *p) const { freeReplyObject(p); }
			};
			const std::unique_ptr<redisReply, Free> owned(r);
			return take_reply(r, what);
		}
		// The context is unusable after any I/O failure. A connection the peer
		// closed (EOF, a reset, a broken pipe) is most often one the server
		// dropped while it sat idle (a failover, a restart), which never saw this
		// command; but the drop may have met the reply of a command the server
		// ran, so it goes again once on a new connection only when the caller
		// says a second run does no harm (RedisResend). A timeout may have run it
		// on a server that is merely slow: never again.
		const int err = ctx_->err;
		const int saved_errno = errno;
		const std::string why = ctx_->errstr;
		close();
		const bool dropped = err == REDIS_ERR_EOF ||
				(err == REDIS_ERR_IO && (saved_errno == ECONNRESET || saved_errno == EPIPE));
		if (!dropped || attempt > 0 || resend == RedisResend::Never) {
			throw RedisError("redis: " + std::string(what) + " on " + db::masked_url(options_.url) +
							": " + why,
					true);
		}
	}
}

RedisReply RedisConnection::eval(std::string_view script, const std::vector<std::string> &keys,
		const std::vector<std::string> &args, RedisResend resend) {
	std::vector<std::string> argv;
	argv.reserve(3 + keys.size() + args.size());
	argv.emplace_back("EVAL");
	argv.emplace_back(script);
	argv.push_back(std::to_string(keys.size()));
	argv.insert(argv.end(), keys.begin(), keys.end());
	argv.insert(argv.end(), args.begin(), args.end());
	return command(argv, resend);
}

RedisPool::Lease::Lease(RedisPool &pool, std::unique_ptr<RedisConnection> conn) :
		pool_(&pool), conn_(std::move(conn)) {}

RedisPool::Lease::~Lease() {
	if (conn_)
		pool_->release(std::move(conn_));
}

RedisPool::RedisPool(RedisOptions options, std::size_t max) :
		options_(std::move(options)), max_(max == 0 ? 1 : max) {}

RedisPool::Lease RedisPool::acquire() {
	std::unique_lock<std::mutex> lock(mu_);
	const auto deadline =
			std::chrono::steady_clock::now() + std::chrono::milliseconds(kRedisAcquireTimeoutMs);
	for (;;) {
		if (!idle_.empty()) {
			auto conn = std::move(idle_.back());
			idle_.pop_back();
			return Lease(*this, std::move(conn));
		}
		if (open_ < max_) {
			++open_;
			lock.unlock();
			try {
				return Lease(*this, std::make_unique<RedisConnection>(options_));
			} catch (...) {
				lock.lock();
				--open_;
				returned_.notify_one();
				throw;
			}
		}
		if (returned_.wait_until(lock, deadline) == std::cv_status::timeout && idle_.empty() &&
				open_ >= max_) {
			throw RedisError("redis: no connection came back within " +
							std::to_string(kRedisAcquireTimeoutMs) + " ms",
					false);
		}
	}
}

void RedisPool::release(std::unique_ptr<RedisConnection> conn) {
	{
		std::lock_guard<std::mutex> lock(mu_);
		if (conn->connected()) {
			idle_.push_back(std::move(conn));
		} else {
			--open_;
		}
	}
	returned_.notify_one();
}

RedisReply RedisPool::command(const std::vector<std::string> &argv, RedisResend resend) {
	return acquire()->command(argv, resend);
}

RedisReply RedisPool::eval(std::string_view script, const std::vector<std::string> &keys,
		const std::vector<std::string> &args, RedisResend resend) {
	return acquire()->eval(script, keys, args, resend);
}

} // namespace opennova::novaworld_server
