#pragma once
// Bounded in-memory ring over the io::log channel (ADR 0042 d5): the embedder
// installs it as the process sink, it records every message with a monotonic
// sequence, and a tool (MCP's game_logs "engine" source) drains entries after
// a cursor. Installing CHAINS: whatever sink was in the slot before keeps
// receiving every message downstream, so the GDExtension's push_warning
// forwarder and the ring coexist. Infrastructure like io/vfs, not a port.
// Header-only so opennova_io stays an INTERFACE target; the mutex keeps
// record/drain safe if an embedder ever logs off the main thread.
#include <base/io/log.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace opennova {
namespace io {

// The transport label for a level ("debug"/"info"/"warn"/"error") — the one
// mapping the MCP boundary reuses instead of minting its own.
inline const char *log_level_name(LogLevel level) {
	switch (level) {
	case LogLevel::kDebug: return "debug";
	case LogLevel::kInfo: return "info";
	case LogLevel::kWarn: return "warn";
	case LogLevel::kError: return "error";
	}
	return "info";
}

struct LogRingEntry {
	uint64_t sequence = 0; // monotonic from 1; a gap after a drain means the ring wrapped
	LogLevel level = LogLevel::kDebug;
	std::string text;
};

class LogRing {
public:
	static constexpr size_t kCapacity = 512;

	// The process ring — one per process, like the sink slot itself.
	// Instantiable so tests exercise wrap/drain on private rings.
	static LogRing &instance() {
		static LogRing ring;
		return ring;
	}

	// Installs the process ring as the io::log sink. The sink installed
	// before it becomes the downstream chain and keeps receiving every
	// message after the ring records it. Idempotent.
	static void install() {
		if (log_sink_slot() == &LogRing::sink_thunk) return;
		LogRing &ring = instance();
		{
			std::lock_guard<std::mutex> lock(ring.mutex_);
			ring.downstream_ = log_sink_slot();
		}
		set_log_sink(&LogRing::sink_thunk);
	}

	void record(LogLevel level, const char *message) {
		LogSink downstream = nullptr;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			LogRingEntry entry;
			entry.sequence = next_sequence_++;
			entry.level = level;
			entry.text = message;
			if (entries_.size() < kCapacity) {
				entries_.push_back(std::move(entry));
			} else {
				entries_[head_] = std::move(entry);
				head_ = (head_ + 1) % kCapacity;
			}
			downstream = downstream_;
		}
		// Outside the lock: the downstream sink may do arbitrary work
		// (push_warning routes into Godot's logger).
		if (downstream != nullptr) downstream(level, message);
	}

	// Entries with sequence > cursor, oldest first (max_entries == 0 caps at
	// the whole ring). The caller's next cursor is the last returned
	// sequence; a first sequence beyond cursor + 1 means the ring wrapped
	// past unread entries.
	std::vector<LogRingEntry> entries_after(uint64_t cursor, size_t max_entries = 0) const {
		std::lock_guard<std::mutex> lock(mutex_);
		std::vector<LogRingEntry> out;
		const size_t size = entries_.size();
		for (size_t i = 0; i < size; ++i) {
			const LogRingEntry &entry = entries_[(head_ + i) % size];
			if (entry.sequence <= cursor) continue;
			out.push_back(entry);
			if (max_entries != 0 && out.size() >= max_entries) break;
		}
		return out;
	}

	uint64_t last_sequence() const {
		std::lock_guard<std::mutex> lock(mutex_);
		return next_sequence_ - 1;
	}

	// Test seam: a fresh ring for the next assertion (entries dropped, the
	// sequence restarted). Ship code never calls it — cursors depend on the
	// sequence staying monotonic for the life of the process.
	void reset() {
		std::lock_guard<std::mutex> lock(mutex_);
		entries_.clear();
		head_ = 0;
		next_sequence_ = 1;
	}

private:
	static void sink_thunk(LogLevel level, const char *message) {
		instance().record(level, message);
	}

	mutable std::mutex mutex_;
	std::vector<LogRingEntry> entries_; // grows to kCapacity, then wraps at head_
	size_t head_ = 0;                   // index of the oldest entry once full
	uint64_t next_sequence_ = 1;
	LogSink downstream_ = nullptr;
};

} // namespace io
} // namespace opennova
