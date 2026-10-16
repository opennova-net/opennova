#pragma once

// Web's Redis: ElastiCache for Valkey in prod, Valkey 8 locally
// (scripts/dev_redis.sh). It holds the ephemeral state every web replica must
// see, each key with a TTL: the retail NW*.dll login, join and host-key
// sessions (session_store.h), the retail login pins (retail_pins.h) and the
// rate limiters' buckets (rate_limiter.h). Anything with an invariant stays in
// PostgreSQL. Through hiredis (hiredis_ssl for rediss://), which only this
// server's core links (scripts/lint/link_graph_check.py), as libpq is contained
// in the service target.

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// Forward-declared so the header stays hiredis-free for its consumers.
struct redisContext;

namespace opennova::novaworld_server {

// Thrown for every failure the wrapper sees: a URL that does not read, a
// connection that would not open (or its TLS handshake, or AUTH), a command
// that timed out or lost its connection, and an error reply. what() names the
// server (its URL, a password in it masked) and the command's name, never an
// argument. connection_lost(): nothing the command did is known to have run.
class RedisError : public std::runtime_error {
public:
	RedisError(const std::string &message, bool connection_lost);
	bool connection_lost() const noexcept { return connection_lost_; }

private:
	bool connection_lost_;
};

// The wait the pool's acquire() allows for a connection to come back, and the
// connections one pool opens at most (a lease lasts one command or script, so
// the HTTP handler threads rarely need as many).
inline constexpr int kRedisAcquireTimeoutMs = 5000;
inline constexpr std::size_t kRedisPoolMax = 16;

// How to reach the server. `url` is redis://host[:port][/db] or rediss://...
// (TLS: the server's certificate is checked against the system's CAs and its
// name against the URL's host), with an optional user[:password]@ before the
// host; `password` is the AUTH password when the URL names none (kept apart: it
// is a secret, the URL is not). Every key the stores write starts with
// `key_prefix`: the service's own namespace, which a test makes unique per
// listener so several share one server. A command waits at most
// command_timeout_ms for its reply.
struct RedisOptions {
	std::string url = "redis://127.0.0.1:6379";
	std::string password;
	std::string key_prefix = "nw:";
	int connect_timeout_ms = 2000;
	int command_timeout_ms = 2000;
};

// A reply, owned: nil, an integer, a string (a bulk or a status reply) or an
// array of replies. An error reply is never one: it throws RedisError.
struct RedisReply {
	enum class Type { Nil, Integer, String, Array };
	Type type = Type::Nil;
	int64_t integer = 0;
	std::string string;
	std::vector<RedisReply> elements;
};

// Whether a command whose connection turned out dropped goes again, once, on a
// reopened one. A drop most often meets a command on a connection the server
// closed while it sat idle (a failover, a restart, the server's idle timeout),
// which never saw it; but one that meets the reply leaves a command the server
// may already have run. Never (the default): the call throws, for a command
// whose second run answers otherwise (GETDEL finds its value taken, SET NX its
// own key). IfDropped: for one whose second run does no harm (a GET, a SET, a
// DEL, a script that leaves what one run would, a rate bucket drawn twice). A
// command that timed out is never sent again (a slow server may run it).
enum class RedisResend { Never, IfDropped };

// One connection. NOT thread-safe: one thread uses it at a time (RedisPool
// leases it). A connection the server or the network dropped is reopened
// before the next command; the command that found it dropped goes again as
// its RedisResend says.
class RedisConnection {
public:
	// Connects (TLS, AUTH, SELECT as the URL asks), or throws RedisError.
	explicit RedisConnection(const RedisOptions &options);
	~RedisConnection();
	RedisConnection(const RedisConnection &) = delete;
	RedisConnection &operator=(const RedisConnection &) = delete;

	// Runs one command, binary-safe; argv[0] is its name.
	RedisReply command(
			const std::vector<std::string> &argv, RedisResend resend = RedisResend::Never);

	// EVAL `script` over `keys` and `args`: the script runs atomically on the
	// server, so a read and the write it decides see no other client between
	// them.
	RedisReply eval(std::string_view script, const std::vector<std::string> &keys,
			const std::vector<std::string> &args, RedisResend resend = RedisResend::Never);

	// False once the connection is lost (until a command reopens it).
	bool connected() const { return ctx_ != nullptr; }

private:
	void open();
	void close();

	const RedisOptions options_;
	redisContext *ctx_ = nullptr;
};

// Connections to one server, each leased by one thread at a time; a lease puts
// its connection back when it ends (dropping one that was lost). acquire()
// hands out an idle connection, opens a new one while fewer than `max` are
// open, and otherwise waits up to kRedisAcquireTimeoutMs for one to come back,
// then throws RedisError. command() and eval() lease a connection for the one
// call. Thread-safe; must outlive every lease it hands out.
class RedisPool {
public:
	class Lease {
	public:
		Lease(Lease &&other) noexcept = default;
		Lease &operator=(Lease &&other) = delete;
		Lease(const Lease &) = delete;
		Lease &operator=(const Lease &) = delete;
		~Lease();

		RedisConnection &operator*() const { return *conn_; }
		RedisConnection *operator->() const { return conn_.get(); }

	private:
		friend class RedisPool;
		Lease(RedisPool &pool, std::unique_ptr<RedisConnection> conn);

		RedisPool *pool_;
		std::unique_ptr<RedisConnection> conn_;
	};

	explicit RedisPool(RedisOptions options, std::size_t max = kRedisPoolMax);
	RedisPool(const RedisPool &) = delete;
	RedisPool &operator=(const RedisPool &) = delete;

	Lease acquire();

	RedisReply command(
			const std::vector<std::string> &argv, RedisResend resend = RedisResend::Never);
	RedisReply eval(std::string_view script, const std::vector<std::string> &keys,
			const std::vector<std::string> &args, RedisResend resend = RedisResend::Never);

	// `name` in the service's namespace: key_prefix + name.
	std::string key(std::string_view name) const { return options_.key_prefix + std::string(name); }

	const RedisOptions &options() const { return options_; }

private:
	void release(std::unique_ptr<RedisConnection> conn);

	const RedisOptions options_;
	const std::size_t max_;
	std::mutex mu_;
	std::condition_variable returned_;
	std::size_t open_ = 0;
	std::vector<std::unique_ptr<RedisConnection>> idle_;
};

// `ms` as the decimal text a command argument carries.
inline std::string redis_ms(std::chrono::milliseconds ms) {
	return std::to_string(ms.count());
}

} // namespace opennova::novaworld_server
