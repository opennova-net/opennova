#include <novaworld/unknown_tracker.h>

#include <novaworld/db/sqlite.h>

#include <algorithm>
#include <utility>

namespace opennova {

void UnknownTracker::record(std::string channel, std::string signature,
                            const uint8_t *sample, std::size_t sample_len,
                            std::string meta, uint64_t now_ms) {
	Key key{std::move(channel), std::move(signature)};

	std::lock_guard<std::mutex> lk(mu_);
	auto it = seen_.find(key);
	if (it == seen_.end()) {
		UnknownSighting s;
		s.channel = key.first;
		s.signature = key.second;
		s.count = 1;
		s.first_seen_ms = now_ms;
		s.last_seen_ms = now_ms;
		s.sample_meta = std::move(meta);
		if (sample != nullptr && sample_len > 0) {
			const std::size_t take = std::min(sample_len, kUnknownSampleCap);
			s.sample.assign(sample, sample + take);
		}
		seen_.emplace(key, std::move(s));
	} else {
		UnknownSighting &s = it->second;
		++s.count;
		s.last_seen_ms = now_ms;
		// First sample + meta are sticky — never overwrite on later sightings.
	}
	dirty_.insert(std::move(key));
}

void UnknownTracker::record(std::string channel, std::string signature,
                            const std::vector<uint8_t> &sample,
                            std::string meta, uint64_t now_ms) {
	record(std::move(channel), std::move(signature), sample.data(),
	       sample.size(), std::move(meta), now_ms);
}

void UnknownTracker::flush(db::Database &db) {
	// Copy the dirty rows out under the lock, then do DB I/O outside it so a
	// slow sqlite write doesn't block the listener threads' record() calls.
	std::vector<UnknownSighting> pending;
	std::set<Key> flushed;
	{
		std::lock_guard<std::mutex> lk(mu_);
		if (dirty_.empty()) return;
		pending.reserve(dirty_.size());
		for (const auto &key : dirty_) {
			auto it = seen_.find(key);
			if (it != seen_.end()) {
				pending.push_back(it->second);
				flushed.insert(key);
			}
		}
	}

	// On conflict keep the FIRST sample + first_seen_ms + sample_meta; only
	// the running count + last_seen_ms advance. excluded.* is the row we
	// tried to insert.
	static constexpr char kUpsert[] =
		"INSERT INTO unknown_messages "
		"  (channel, signature, count, first_seen_ms, last_seen_ms, sample, sample_meta) "
		"VALUES (?, ?, ?, ?, ?, ?, ?) "
		"ON CONFLICT(channel, signature) DO UPDATE SET "
		"  count = excluded.count, "
		"  last_seen_ms = excluded.last_seen_ms;";

	for (const auto &s : pending) {
		std::vector<db::BindValue> binds;
		binds.reserve(7);
		binds.emplace_back(s.channel);
		binds.emplace_back(s.signature);
		binds.emplace_back(static_cast<int64_t>(s.count));
		binds.emplace_back(static_cast<int64_t>(s.first_seen_ms));
		binds.emplace_back(static_cast<int64_t>(s.last_seen_ms));
		if (s.sample.empty()) {
			binds.emplace_back(std::monostate{});
		} else {
			binds.emplace_back(s.sample);
		}
		binds.emplace_back(s.sample_meta);
		db.exec(kUpsert, binds);
	}

	// Only drop keys we actually flushed — anything record()'d during the DB
	// I/O above stays dirty for the next tick.
	std::lock_guard<std::mutex> lk(mu_);
	for (const auto &key : flushed) {
		dirty_.erase(key);
	}
}

std::vector<UnknownSighting> UnknownTracker::snapshot() const {
	std::lock_guard<std::mutex> lk(mu_);
	std::vector<UnknownSighting> out;
	out.reserve(seen_.size());
	for (const auto &[key, s] : seen_) {
		(void)key;
		out.push_back(s);
	}
	return out;
}

} // namespace opennova
