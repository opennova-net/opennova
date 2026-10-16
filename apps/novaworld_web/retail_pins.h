#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace opennova::novaworld_server {

class RedisPool;

// A retail login's pin: the account a PERSISTENTEXPRESSLOGINDATA cookie's
// login was made for, with the credentials generation that login read. Retail's
// IB3 sets this cookie itself (we never set it) and ships it on every HTTP
// request from the same retail process, so it names that process's login to
// POST /NWLogin.dll (a resume with no password), /NWJoin.dll and
// /NWAccount.dll after the LOGINSESSIONTAG (G.8). A pin counts only while its
// account is still at its generation (HttpListener::pinned_user checks).
struct RetailPin {
	int64_t user_id = 0;
	int64_t credentials_gen = 0;
};

// The pins, in Redis (redis_client.h), so a login on one web replica resumes
// on another and a revocation on one holds on all: <prefix>pin:<cookie> holds
// "<user_id>:<credentials_gen>" until kTtl after its last use (the login that
// wrote it, or a get()), and <prefix>pins:<user_id> the cookies pinned to the
// account (the index revoke() reads), kept at least as long. A Redis failure
// throws RedisError.
class RetailPins {
public:
	// How long a pin lives unused: a retail process logs in once at its start
	// and reads its pin on its later requests (/NWHost.dll, /NWJoin.dll,
	// /NWAccount.dll), each read moving the expiry out again, so however long
	// it runs, only one a day without such a request must log in again.
	static constexpr std::chrono::hours kTtl{ 24 };

	explicit RetailPins(RedisPool &redis) : redis_(redis) {}

	// Pins `cookie` to `pin`, replacing any pin it had.
	void put(const std::string &cookie, const RetailPin &pin);
	// The pin `cookie` holds, its expiry and its account's index's moved out to
	// kTtl from now; nullopt for an empty or unknown cookie.
	std::optional<RetailPin> get(const std::string &cookie);
	// Drops `cookie`'s pin while it is still `pin` (a pin found stale).
	void drop_if(const std::string &cookie, const RetailPin &pin);
	// Drops `cookie`'s pin and returns what it held (a logout).
	std::optional<RetailPin> take(const std::string &cookie);
	// Drops every pin to `user_id` (an account's revocation: an admin password
	// reset, a status other than 'active', a delete). Returns how many.
	int64_t revoke(int64_t user_id);

private:
	RedisPool &redis_;
};

} // namespace opennova::novaworld_server
