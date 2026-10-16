#include "retail_pins.h"

#include "redis_client.h"

#include <base/io/strutil.h>

#include <vector>

namespace opennova::novaworld_server {

namespace {

// Pins KEYS[1] (the cookie's pin) to ARGV[1] for ARGV[3] ms and adds the
// cookie ARGV[2] to the account's index KEYS[2], whose TTL moves out as far.
constexpr const char *kPutScript = R"lua(
redis.call('SET', KEYS[1], ARGV[1], 'PX', ARGV[3])
redis.call('SADD', KEYS[2], ARGV[2])
redis.call('PEXPIRE', KEYS[2], ARGV[3])
return 1
)lua";

// Deletes KEYS[1] while it holds ARGV[1].
constexpr const char *kDropIfScript = R"lua(
if redis.call('GET', KEYS[1]) == ARGV[1] then
  return redis.call('DEL', KEYS[1])
end
return 0
)lua";

// The account's revocation: KEYS[1] is its index, KEYS[2..] the pins of the
// cookies it listed (ARGV[2..]); each pin still to the account (its value
// starts ARGV[1], "<user_id>:") is deleted, and the cookies leave the index (a
// cookie pinned again since, to anyone, stays pinned). Returns the pins
// deleted.
constexpr const char *kRevokeScript = R"lua(
local dropped = 0
for i = 2, #KEYS do
  local v = redis.call('GET', KEYS[i])
  if v and string.sub(v, 1, #ARGV[1]) == ARGV[1] then
    dropped = dropped + redis.call('DEL', KEYS[i])
  end
  redis.call('SREM', KEYS[1], ARGV[i])
end
return dropped
)lua";

std::string pin_value(const RetailPin &pin) {
	return std::to_string(pin.user_id) + ":" + std::to_string(pin.credentials_gen);
}

std::optional<RetailPin> pin_of(const RedisReply &reply) {
	if (reply.type != RedisReply::Type::String)
		return std::nullopt;
	const auto colon = reply.string.find(':');
	if (colon == std::string::npos)
		return std::nullopt;
	const auto user = strutil::parse_llong(reply.string.substr(0, colon));
	const auto gen = strutil::parse_llong(reply.string.substr(colon + 1));
	if (!user || !gen)
		return std::nullopt;
	return RetailPin{ *user, *gen };
}

std::string ttl_ms() {
	return redis_ms(std::chrono::duration_cast<std::chrono::milliseconds>(RetailPins::kTtl));
}

} // namespace

void RetailPins::put(const std::string &cookie, const RetailPin &pin) {
	redis_.eval(kPutScript,
			{ redis_.key("pin:" + cookie), redis_.key("pins:" + std::to_string(pin.user_id)) },
			{ pin_value(pin), cookie, ttl_ms() }, RedisResend::IfDropped);
}

std::optional<RetailPin> RetailPins::get(const std::string &cookie) {
	if (cookie.empty())
		return std::nullopt;
	// The read moves the pin's expiry out again (GETEX), then its account's
	// index's as far, so the index outlives every pin it lists.
	const auto pin = pin_of(redis_.command(
			{ "GETEX", redis_.key("pin:" + cookie), "PX", ttl_ms() }, RedisResend::IfDropped));
	if (pin) {
		redis_.command({ "PEXPIRE", redis_.key("pins:" + std::to_string(pin->user_id)), ttl_ms() },
				RedisResend::IfDropped);
	}
	return pin;
}

void RetailPins::drop_if(const std::string &cookie, const RetailPin &pin) {
	redis_.eval(kDropIfScript, { redis_.key("pin:" + cookie) }, { pin_value(pin) },
			RedisResend::IfDropped);
}

std::optional<RetailPin> RetailPins::take(const std::string &cookie) {
	if (cookie.empty())
		return std::nullopt;
	return pin_of(redis_.command({ "GETDEL", redis_.key("pin:" + cookie) }, RedisResend::Never));
}

int64_t RetailPins::revoke(int64_t user_id) {
	const std::string index = redis_.key("pins:" + std::to_string(user_id));
	const RedisReply cookies = redis_.command({ "SMEMBERS", index }, RedisResend::IfDropped);
	if (cookies.elements.empty())
		return 0;
	std::vector<std::string> keys{ index };
	std::vector<std::string> args{ std::to_string(user_id) + ":" };
	for (const RedisReply &c : cookies.elements) {
		keys.push_back(redis_.key("pin:" + c.string));
		args.push_back(c.string);
	}
	const RedisReply dropped = redis_.eval(kRevokeScript, keys, args, RedisResend::IfDropped);
	return dropped.type == RedisReply::Type::Integer ? dropped.integer : 0;
}

} // namespace opennova::novaworld_server
