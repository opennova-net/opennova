// The one simulation profile collector (ADR 0043 d5): every tick-side span or
// count the dev tools' Stats window shows lands on a fixed slot of ONE
// TickProfile through an RAII scope or a lap marker, keyed by the same slot
// table the frame-stats board and the Stats window rows read
// (frame_stats_slots.h). It replaces the per-layer *Perf records (world, AI,
// collision, server tick, host session, client frame, joiner pump, replication
// fan) that were threaded through every tick signature and summed field by
// field at each layer boundary.
//
// Cost contract: while inactive a scope or lap tests one bool and reads no
// clock; while active a span costs the same two steady_clock reads the old
// records did. The embedder (the Godot Simulation, a headless host, a ctest)
// turns the profile on with the capture window, drains the touched slots onto
// its board once per render frame, and resets it. Nothing on the simulation
// path reads the collector.
#pragma once

#include <base/io/perf_clock.h>
#include <runtime/devtools/frame_stats_slots.h>

#include <array>
#include <chrono>
#include <cstdint>

namespace opennova::devtools {

class TickProfile {
public:
	bool active() const { return active_; }
	void set_active(bool active) { active_ = active; }

	// Add a span (microseconds) or a count onto a slot. A slot written at
	// least once since the last reset is "touched"; the embedder folds only
	// touched slots so a role that never runs a phase never samples its row.
	void add(Slot slot, int64_t amount) {
		sums_[static_cast<int>(slot)] += amount;
		touched_[static_cast<int>(slot)] = 1;
	}
	// Assign a VALUE slot (a plain count published once per tick).
	void set_value(Slot slot, int64_t value) {
		sums_[static_cast<int>(slot)] = value;
		touched_[static_cast<int>(slot)] = 1;
	}
	int64_t sum(Slot slot) const { return sums_[static_cast<int>(slot)]; }
	bool touched(Slot slot) const { return touched_[static_cast<int>(slot)] != 0; }

	template <class F>
	void for_each_touched(F &&fn) const {
		for (int i = 0; i < kSlotCount; ++i) {
			if (touched_[i] != 0) fn(static_cast<Slot>(i), sums_[i]);
		}
	}

	// Zero every slot and its touched bit; the active state is unchanged.
	void reset() {
		sums_.fill(0);
		touched_.fill(0);
	}

private:
	bool active_ = false;
	std::array<int64_t, kSlotCount> sums_{};
	std::array<uint8_t, kSlotCount> touched_{};
};

// One span: the constructor samples the clock when the profile is active,
// the destructor adds the elapsed microseconds onto the slot. A null or
// inactive profile costs one branch.
class ProfileScope {
public:
	ProfileScope(TickProfile *profile, Slot slot)
			: profile_(profile != nullptr && profile->active() ? profile : nullptr),
			  slot_(slot),
			  start_(profile_ != nullptr ? io::perf_now_us() : 0) {}
	~ProfileScope() {
		if (profile_ == nullptr) return;
		const int64_t span = static_cast<int64_t>(io::perf_now_us() - start_);
		profile_->add(slot_, span);
		if (also_ != Slot::COUNT) profile_->add(also_, span);
	}
	ProfileScope(const ProfileScope &) = delete;
	ProfileScope &operator=(const ProfileScope &) = delete;

	// Land the whole span on `slot` as well when it closes, with no further
	// read: a caller that learns mid-span that the span is one child row's.
	void also(Slot slot) { also_ = slot; }

private:
	TickProfile *profile_;
	Slot slot_;
	uint64_t start_;
	Slot also_ = Slot::COUNT;
};

// Sequential phases of one function: mark(slot) adds the time since the
// previous mark (or the construction) onto the slot and restarts the lap.
class ProfileLap {
public:
	explicit ProfileLap(TickProfile *profile)
			: profile_(profile != nullptr && profile->active() ? profile : nullptr),
			  last_(profile_ != nullptr ? io::perf_now_us() : 0) {}
	bool active() const { return profile_ != nullptr; }
	void mark(Slot slot) {
		if (profile_ == nullptr) return;
		const uint64_t now = io::perf_now_us();
		profile_->add(slot, static_cast<int64_t>(now - last_));
		last_ = now;
	}
	// Restart the lap without attributing the time since the last mark.
	void restart() {
		if (profile_ != nullptr) last_ = io::perf_now_us();
	}
	// The clock sample the last mark (or restart, or construction) took; 0
	// while inactive. Lets a caller span several laps onto one more slot
	// without a second clock read.
	uint64_t last() const { return last_; }

private:
	TickProfile *profile_;
	uint64_t last_;
};

// Many short laps onto one slot, each a few microseconds, which a ProfileScope's microsecond
// clock would truncate: the laps sum in nanoseconds and land on the slot once, when the sum
// goes out of scope. A null or inactive profile runs each lap with no clock read.
class ProfileNsSum {
public:
	ProfileNsSum(TickProfile *profile, Slot slot)
			: profile_(profile != nullptr && profile->active() ? profile : nullptr), slot_(slot) {}
	~ProfileNsSum() {
		if (profile_ != nullptr) profile_->add(slot_, ns_ / 1000);
	}
	ProfileNsSum(const ProfileNsSum &) = delete;
	ProfileNsSum &operator=(const ProfileNsSum &) = delete;
	template <class F>
	void lap(F &&fn) {
		if (profile_ == nullptr) {
			fn();
			return;
		}
		const auto start = std::chrono::steady_clock::now();
		fn();
		ns_ += std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - start).count();
	}

private:
	TickProfile *profile_;
	Slot slot_;
	int64_t ns_ = 0;
};

}  // namespace opennova::devtools
