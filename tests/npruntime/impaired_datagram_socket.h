#pragma once

// An impairing decorator over the real-socket seam (net/npwire/idatagram_socket.h),
// the test-side sibling of net/npwire/counting_datagram_socket.h. It lives under
// tests/ because nothing but a test constructs it (scripts/lint/
// orphan_header_check.py keeps test-only headers out of engine/). Every datagram
// the owner loop SENDS
// passes through a seeded link model on a virtual clock before it reaches the
// inner socket: it may be lost, duplicated, delayed (a base one-way latency plus
// uniform jitter that keeps FIFO order) or held back long enough to arrive
// behind later datagrams.
// Receives pass through untouched; impair both directions by decorating both
// endpoints' sockets.
//
// Test infrastructure, not protocol and not a port of anything in the original
// engine: it exists so ctest can soak the NP session layer (the 0x44/0x84
// recovery, the send holdoff, the retention bounds) over the link a player on a
// lossy internet connection sees.
//
// Deterministic by construction: the same seed and the same sequence of
// send_to calls produce the same delivery schedule on every platform. The
// generator is a private xorshift64* and every probability is an integer
// per-million threshold (no <random> distributions, whose outputs differ
// between standard libraries). Delivery order is (due time, send order), so two
// datagrams due in the same millisecond keep the order they were sent in.
//
// The clock is the caller's: `now_ms` is read on every send_to / recv_from /
// release_due, so a test advances one virtual millisecond counter for both
// endpoints and the link never reads wall time. Header-only.

#include <net/npwire/idatagram_socket.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace opennova {

struct DatagramImpairment {
	uint32_t loss_ppm = 0;          // datagrams dropped, per million sent
	uint32_t duplicate_ppm = 0;     // surviving datagrams delivered twice, per million
	uint32_t reorder_ppm = 0;       // surviving datagrams held back by reorder_hold_ms, per million
	uint32_t delay_ms = 0;          // base one-way latency
	uint32_t jitter_ms = 0;         // extra latency drawn uniformly from [0, jitter_ms]
	uint32_t reorder_hold_ms = 0;   // the extra latency of a reordered datagram
	uint32_t duplicate_gap_ms = 0;  // how long after the original its duplicate lands
	uint64_t seed = 1;
};

struct DatagramImpairmentStats {
	uint64_t sent = 0;       // send_to calls seen
	uint64_t dropped = 0;    // lost on the link
	uint64_t duplicated = 0; // extra copies scheduled
	uint64_t reordered = 0;  // held back by reorder_hold_ms
	uint64_t delivered = 0;  // copies handed to the inner socket
};

class ImpairedDatagramSocket : public IDatagramSocket {
public:
	using Clock = std::function<uint64_t()>;

	ImpairedDatagramSocket(std::unique_ptr<IDatagramSocket> inner, DatagramImpairment profile,
			Clock now_ms)
			: inner_(std::move(inner)), profile_(profile), now_ms_(std::move(now_ms)),
			  rng_state_(profile.seed != 0 ? profile.seed : 0x9E3779B97F4A7C15ull) {}

	int recv_from(uint8_t *buf, std::size_t cap, PeerAddr &from) override {
		release_due();
		return inner_ != nullptr ? inner_->recv_from(buf, cap, from) : 0;
	}

	void send_to(const PeerAddr &to, const uint8_t *data, std::size_t len) override {
		++stats_.sent;
		const uint64_t now = clock();
		if (roll(profile_.loss_ppm)) {
			++stats_.dropped;
		} else {
			// Jitter varies the latency but never reorders on its own: a datagram
			// not singled out for reordering lands no earlier than the one sent
			// before it, as on a FIFO path. Only reorder_ppm overtakes.
			uint64_t due = now + profile_.delay_ms + draw(profile_.jitter_ms);
			if (roll(profile_.reorder_ppm)) {
				++stats_.reordered;
				due += profile_.reorder_hold_ms;
			} else {
				if (due < fifo_due_) due = fifo_due_;
				fifo_due_ = due;
			}
			schedule(due, to, data, len);
			if (roll(profile_.duplicate_ppm)) {
				++stats_.duplicated;
				schedule(due + profile_.duplicate_gap_ms, to, data, len);
			}
		}
		release_due();
	}

	// Hand every datagram whose delivery time has come to the inner socket, in
	// (due time, send order).
	void release_due() {
		const uint64_t now = clock();
		while (!in_flight_.empty() && in_flight_.begin()->first.first <= now) {
			auto it = in_flight_.begin();
			Pending pending = std::move(it->second);
			in_flight_.erase(it);
			++stats_.delivered;
			if (inner_ != nullptr)
				inner_->send_to(pending.to, pending.bytes.data(), pending.bytes.size());
		}
	}

	// Replace the link model mid-run (a test that heals the link before its
	// drain checks). Datagrams already in flight keep their schedule.
	void set_profile(const DatagramImpairment &profile) {
		profile_.loss_ppm = profile.loss_ppm;
		profile_.duplicate_ppm = profile.duplicate_ppm;
		profile_.reorder_ppm = profile.reorder_ppm;
		profile_.delay_ms = profile.delay_ms;
		profile_.jitter_ms = profile.jitter_ms;
		profile_.reorder_hold_ms = profile.reorder_hold_ms;
		profile_.duplicate_gap_ms = profile.duplicate_gap_ms;
	}

	std::size_t in_flight() const { return in_flight_.size(); }
	const DatagramImpairmentStats &stats() const { return stats_; }
	IDatagramSocket *inner() const { return inner_.get(); }

private:
	struct Pending {
		PeerAddr to{};
		std::vector<uint8_t> bytes;
	};

	uint64_t clock() const { return now_ms_ ? now_ms_() : 0; }

	// xorshift64* (Vigna): a fixed, platform-independent stream.
	uint64_t next() {
		rng_state_ ^= rng_state_ >> 12;
		rng_state_ ^= rng_state_ << 25;
		rng_state_ ^= rng_state_ >> 27;
		return rng_state_ * 0x2545F4914F6CDD1Dull;
	}
	bool roll(uint32_t ppm) {
		if (ppm == 0) return false;
		return (next() >> 32) % 1000000u < ppm;
	}
	uint64_t draw(uint32_t max_inclusive) {
		if (max_inclusive == 0) return 0;
		return (next() >> 32) % (static_cast<uint64_t>(max_inclusive) + 1u);
	}

	void schedule(uint64_t due, const PeerAddr &to, const uint8_t *data, std::size_t len) {
		Pending pending;
		pending.to = to;
		pending.bytes.assign(data, data + len);
		in_flight_.emplace(std::make_pair(due, send_order_++), std::move(pending));
	}

	std::unique_ptr<IDatagramSocket> inner_;
	DatagramImpairment profile_;
	Clock now_ms_;
	uint64_t rng_state_;
	uint64_t send_order_ = 0;
	uint64_t fifo_due_ = 0; // the latest due time on the in-order path
	std::map<std::pair<uint64_t, uint64_t>, Pending> in_flight_;
	DatagramImpairmentStats stats_{};
};

} // namespace opennova
