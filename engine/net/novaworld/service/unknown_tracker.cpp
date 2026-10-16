#include <net/novaworld/unknown_tracker.h>

#include <net/novaworld/db/database.h>

#include <algorithm>
#include <utility>
#include <variant>

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
	// slow database write doesn't block the listener threads' record() calls.
	// Each with what it adds: its count since its last successful flush.
	std::vector<UnknownSighting> pending;
	std::vector<uint64_t> added;
	std::set<Key> flushed;
	{
		std::lock_guard<std::mutex> lk(mu_);
		if (dirty_.empty()) return;
		pending.reserve(dirty_.size());
		for (const auto &key : dirty_) {
			auto it = seen_.find(key);
			if (it != seen_.end()) {
				pending.push_back(it->second);
				const auto done = flushed_.find(key);
				added.push_back(it->second.count - (done == flushed_.end() ? 0 : done->second));
				flushed.insert(key);
			}
		}
	}

	// On conflict keep the FIRST sample + first_seen_ms + sample_meta (the
	// row's first writer's, whichever process); the count adds this flush's
	// sightings to every process's before it, and last_seen_ms keeps the
	// latest. excluded.* is the row we tried to insert.
	static constexpr char kUpsert[] =
			"INSERT INTO unknown_messages "
			"  (channel, signature, count, first_seen_ms, last_seen_ms, sample, sample_meta) "
			"VALUES ($1, $2, $3, $4, $5, $6, $7) "
			"ON CONFLICT(channel, signature) DO UPDATE SET "
			"  count = unknown_messages.count + excluded.count, "
			"  last_seen_ms = GREATEST(unknown_messages.last_seen_ms, excluded.last_seen_ms);";

	// One transaction: a failure adds none of them, and the next flush adds
	// them all again.
	db::Transaction tx(db);
	for (size_t i = 0; i < pending.size(); ++i) {
		const UnknownSighting &s = pending[i];
		std::vector<db::BindValue> binds;
		binds.reserve(7);
		binds.emplace_back(s.channel);
		binds.emplace_back(s.signature);
		binds.emplace_back(static_cast<int64_t>(added[i]));
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
	tx.commit();

	// Only drop keys we actually flushed — anything record()'d during the DB
	// I/O above stays dirty for the next tick, its count past what this flush
	// added.
	std::lock_guard<std::mutex> lk(mu_);
	size_t i = 0;
	for (const auto &key : flushed) {
		flushed_[key] = pending[i].count;
		++i;
		if (seen_[key].count == flushed_[key])
			dirty_.erase(key);
	}
}

std::vector<UnknownSighting> UnknownTracker::list(
		db::Database &db, const std::string &channel, std::size_t limit) {
	const std::string sql =
			std::string("SELECT channel, signature, count, first_seen_ms, last_seen_ms, sample, "
						"COALESCE(sample_meta, '') FROM unknown_messages") +
			(channel.empty() ? "" : " WHERE channel = $2") +
			" ORDER BY last_seen_ms DESC, id DESC LIMIT $1;";
	std::vector<db::BindValue> binds{ static_cast<int64_t>(limit) };
	if (!channel.empty())
		binds.emplace_back(channel);
	std::vector<UnknownSighting> out;
	for (const db::Row &r : db.query(sql, binds)) {
		UnknownSighting s;
		s.channel = r.as_text(0).value_or("");
		s.signature = r.as_text(1).value_or("");
		s.count = static_cast<uint64_t>(r.as_int(2).value_or(0));
		s.first_seen_ms = static_cast<uint64_t>(r.as_int(3).value_or(0));
		s.last_seen_ms = static_cast<uint64_t>(r.as_int(4).value_or(0));
		if (const auto *bytes = std::get_if<std::vector<uint8_t>>(&r.values[5]))
			s.sample = *bytes;
		s.sample_meta = r.as_text(6).value_or("");
		out.push_back(std::move(s));
	}
	return out;
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
