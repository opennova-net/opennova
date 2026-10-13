#pragma once

// How a cloud machine gets what it runs (ADR 0053; ec2_provider.h). A launch's
// user-data carries no file and no credential: only the URL of the service's
// bootstrap route, on the service's PRIVATE address, and a one-time token. At
// its first boot the machine (machine/user_data.sh) posts that token to the
// route (bootstrap_route.h) and gets its bundle once: the working directory's
// files (the secret ones among them: the host file with its game password,
// admin.cfg with the admin login, the service account's credentials),
// opennova-serve's arguments and the serve image. Service policy, not a port.
//
// A token is kTokenChars letters and digits from the OS CSPRNG
// (random_secret), kept here only as its SHA-256, bound to its server and,
// once the launch has named it, to its machine and that machine's private
// address, which alone may redeem it. It is redeemed once and lapses
// kTokenLifetimeMs after it was issued. The store is in memory: a service
// restart forgets every token, so a machine still booting then fails its fetch
// and powers off, which the supervisor reads as a crash (a restart).

#include "machine_provider.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::novaworld_server::hosted {

// An IPv4 network: the addresses whose top bits match `network` under `mask`.
struct Ipv4Cidr {
	uint32_t network = 0;
	uint32_t mask = 0;
};

// A dotted quad ("10.44.0.5"): four decimal parts of 0..255, no sign, no
// leading zero past a lone 0, nothing around it.
std::optional<uint32_t> parse_ipv4(std::string_view text);
// "10.44.0.0/16", or a bare address (/32). The address's bits below the
// prefix must be 0.
std::optional<Ipv4Cidr> parse_ipv4_cidr(std::string_view text);
// Whether `peer` (a TCP peer's address as the HTTP listener reports it: a
// dotted quad, or the same as an IPv4-mapped IPv6 "::ffff:a.b.c.d") lies in
// one of `cidrs`. An IPv6 peer, or no CIDR, admits nothing.
bool ipv4_admitted(std::string_view peer, const std::vector<Ipv4Cidr> &cidrs);
// Whether `cidr` lies wholly inside one of the RFC 1918 private ranges
// (10.0.0.0/8, 172.16.0.0/12, 192.168.0.0/16), so no prefix shorter than /8:
// the networks ONNET_HOSTED_BOOTSTRAP_CIDRS may name (the route must never
// answer a public peer).
bool ipv4_cidr_private(const Ipv4Cidr &cidr);
// "10.44.0.0/16", for the log.
std::string ipv4_cidr_text(const Ipv4Cidr &cidr);

// One log line per key (a peer) per kWindowMs, for at most kMaxKeys keys at
// once: the bootstrap route's refusals inside its networks, which a machine
// retrying its fetch (or anything else in the subnet) could otherwise repeat
// without end. Thread-safe.
class LogThrottle {
public:
	static constexpr uint64_t kWindowMs = 60 * 1000;
	static constexpr size_t kMaxKeys = 256;
	using Clock = std::function<uint64_t()>;

	explicit LogThrottle(Clock now_ms = {});
	// Whether a line about `key` may be logged now (and if so, it counts).
	bool admit(const std::string &key);

private:
	Clock now_;
	std::mutex mu_;
	std::map<std::string, uint64_t> last_ms_;
};

// What one machine fetches.
struct BootstrapBundle {
	int64_t server_id = 0;
	std::string image; // the serve image (ONNET_HOSTED_SERVE_IMAGE), pinned at deploy
	LaunchSpec spec;
};

// The bundle as the machine reads it (machine/user_data.sh):
// {"server_id", "image", "args": [<opennova-serve's arguments>],
//  "files": [{"name", "secret", "content_base64"}]}, each file's bytes in
// standard base64 (the host file is cp1252 text, no JSON string).
std::string bootstrap_bundle_json(const BootstrapBundle &bundle);

class BootstrapStore {
public:
	static constexpr uint64_t kTokenLifetimeMs = 15 * 60 * 1000;
	static constexpr size_t kTokenChars = 48; // ~285 bits
	static constexpr uint64_t kUnfetchedRecordMs = 60 * 60 * 1000;
	// A steady clock in milliseconds; unset, the system's.
	using Clock = std::function<uint64_t()>;

	explicit BootstrapStore(Clock now_ms = {});

	// A new token for `bundle`'s next machine.
	std::string issue(BootstrapBundle bundle);
	// The launch named its machine: `machine_id`, reached at `address` (its
	// private IPv4), which alone may redeem the token. False when the token is
	// unknown (lapsed or revoked).
	bool bind(const std::string &token, const std::string &machine_id, const std::string &address);
	// The launch failed: the token goes, and the machine it was bound to (if
	// any) awaits nothing.
	void revoke(const std::string &token);

	struct Redeemed {
		BootstrapBundle bundle;
		std::string machine_id;
	};
	// The machine's fetch: the bundle when `token` is live, issued for
	// `server_id`, bound, and presented from the bound address; the token is
	// spent then. nullopt otherwise, with `why` for the log (never the token).
	std::optional<Redeemed> redeem(int64_t server_id, std::string_view token, const std::string &peer,
			std::string &why);

	// Whether this process launched `machine_id` and it has not fetched its
	// bundle yet: the provider reads such a machine as Pending (booting) even
	// once EC2 runs it, so the listing's clock starts only when the server is
	// about to. The record goes at the fetch, at forget_machine or revoke, and
	// at the latest kUnfetchedRecordMs after its token lapsed (long past the
	// supervisor's boot timeout, which has ended the machine by then), so the
	// store holds no record of a machine that is gone.
	bool awaiting_fetch(const std::string &machine_id) const;
	// The machine ended (or is being ended): its token (if still live) and its
	// record go.
	void forget_machine(const std::string &machine_id);

	// The live tokens (tests).
	size_t live_tokens() const;

private:
	struct Entry {
		BootstrapBundle bundle;
		uint64_t expires_ms = 0;
		std::string machine_id;
		std::string address;
	};
	void prune(uint64_t now_ms);

	Clock now_;
	mutable std::mutex mu_;
	std::map<std::string, Entry> entries_;       // by the token's SHA-256 (hex)
	// The bound machines that have not fetched yet: machine id -> when the
	// record lapses (its token's lapse + kUnfetchedRecordMs).
	std::map<std::string, uint64_t> unfetched_;
};

} // namespace opennova::novaworld_server::hosted
