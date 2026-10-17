#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace opennova {

namespace db { class Database; }

// One distinct kind of unhandled signal the server saw. Deduped by
// (channel, signature): every later sighting bumps `count` and updates
// `last_seen_ms`; the very first sighting's bytes are kept in `sample`
// (capped) and `sample_meta` describes its origin (peer addr, method, etc).
// The times are the wall clock's (Unix ms), so every process's rows compare.
struct UnknownSighting {
	std::string channel;     // "gate" | "nwu" | "container" | "ptype" | "pn" | "http"
	std::string signature;   // e.g. "0x47", "ClientFooRequest", "JointOperations", "GET /foo"
	uint64_t    count = 0;
	uint64_t    first_seen_ms = 0;
	uint64_t    last_seen_ms = 0;
	std::vector<uint8_t> sample;  // first sighting only, capped at 512 bytes
	std::string sample_meta;      // e.g. peer "1.2.3.4:64206"
};

// Cap on the captured sample, in bytes. The first sighting of each
// (channel, signature) keeps up to this many bytes; later sightings never
// overwrite it.
inline constexpr std::size_t kUnknownSampleCap = 512;

// What one process keeps of what peers send it unasked, so none can grow it
// without bound: a signature is kept to kUnknownSignatureCap bytes of its
// printable form (unknown_signature), and a channel to kUnknownKeysPerChannel
// distinct signatures, every later new one counted in its channel's
// kUnknownOverflowSignature sighting. The rows go after kUnknownKeepMs unseen
// (UnknownTracker::prune, the periodic unknown_messages.prune job).
inline constexpr std::size_t kUnknownSignatureCap = 128;
inline constexpr std::size_t kUnknownKeysPerChannel = 1024;
inline constexpr const char *kUnknownOverflowSignature = "(other)";
inline constexpr int64_t kUnknownKeepMs = int64_t(30) * 24 * 60 * 60 * 1000;

// The signature a sighting is keyed and stored by: `raw` with each byte outside
// printable ASCII, and the backslash, as \xNN (so two raws never meet in one
// key, and every key is text a row takes); past kUnknownSignatureCap bytes cut,
// with ".." and the 64-bit FNV-1a of the whole of `raw` as 16 hex digits after
// it, so two long signatures still key apart.
std::string unknown_signature(std::string_view raw);

// Thread-safe accumulator for unhandled inbound signals. The listener
// threads (gate / nwudp / http) call record() per packet; the main tick
// loop calls flush() to upsert the dirty set into `unknown_messages`, the one
// record every process's sightings meet in (the gates and the lobbies, web's
// 404s), which list() reads for the /api/unknowns route.
//
// Threading: every public method takes the internal mutex. record() is
// cheap (a map lookup + a few field writes), safe to call from any listener
// thread. flush() does the DB I/O and must be called from the main thread
// only (it shares the dbh the main thread owns).
class UnknownTracker {
public:
	UnknownTracker() = default;

	// Record a sighting. Dedups by (channel, unknown_signature(signature)):
	// increments count, updates last_seen_ms, and on the FIRST sighting
	// captures up to kUnknownSampleCap bytes of `sample` plus `meta`; a new
	// signature past its channel's kUnknownKeysPerChannel counts in the
	// channel's kUnknownOverflowSignature sighting instead. `sample` may be null
	// (sample_len 0) when there's nothing to capture. `now_ms` is the wall
	// clock (Unix ms).
	void record(std::string channel, std::string signature,
	            const uint8_t *sample, std::size_t sample_len,
	            std::string meta, uint64_t now_ms);

	// Convenience overload taking a byte vector as the sample.
	void record(std::string channel, std::string signature,
	            const std::vector<uint8_t> &sample,
	            std::string meta, uint64_t now_ms);

	// Upsert every dirty (channel, signature) into `unknown_messages`, adding
	// the sightings since this tracker's last flush to the row's count (every
	// process with a tracker, each gate's, lobby's and web replica's, flushes into
	// the one row, so none overwrites another's: the count is every process's
	// sum) and keeping the latest last_seen_ms; the first sample stays the
	// row's first writer's. Call from the main tick only. On success clears the
	// dirty set so the next flush only touches rows that changed since. Throws
	// db::Error (logged by the caller), the dirty set left intact so a
	// transient failure retries next tick with every sighting since.
	void flush(db::Database &db);

	// Copy the current accumulator out under the lock (this process's).
	std::vector<UnknownSighting> snapshot() const;

	// The `unknown_messages` rows, every process's flushed sightings, the
	// latest seen first, at most `limit`; only `channel`'s when it is not
	// empty. Throws db::Error.
	static std::vector<UnknownSighting> list(
			db::Database &db, const std::string &channel, std::size_t limit = 500);

	// Delete the `unknown_messages` rows no process has seen for `keep_ms`
	// (by the database's clock); how many went. Throws db::Error.
	static std::size_t prune(db::Database &db, int64_t keep_ms = kUnknownKeepMs);

private:
	using Key = std::pair<std::string, std::string>;

	mutable std::mutex mu_;
	std::map<Key, UnknownSighting> seen_;
	std::set<Key> dirty_;
	// Each key's count as its last successful flush added it to the row.
	std::map<Key, uint64_t> flushed_;
	// Each channel's distinct signatures in seen_, its overflow sighting left out.
	std::map<std::string, std::size_t> keys_per_channel_;
};

} // namespace opennova
