#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace opennova {

// The server-browser PING SWEEP semantics. On the GSB XXXX finalize retail
// walks every accumulated row and pings its IPv4 (row dword1; the sweep
// carries NO port): NapiGameList_StartPingSweep formats each address and
// creates one ping entry per row, the ping manager opens ONE raw ICMP echo
// socket (`WSASocketA(AF_INET, SOCK_RAW, IPPROTO_ICMP)`, non-blocking) and a
// worker thread drives the echoes; per-row results land in
// NapiGameList_OnPingResult.
// [orig: NapiGameList_StartPingSweep @0x63bcf0 -> NapiPingEntry_Create
//  @0x62ffb0 -> NapiPingManager_Start @0x62fe50; the socket
//  Network_CreateRawSocket @0x62f690 (AF_INET/SOCK_RAW/IPPROTO_ICMP via
//  WSASocketA, FIONBIO); the manager init NapiConnection_Init @0x6302f0]
//
// A raw ICMP socket needs administrator rights on modern Windows, so the
// reimpl's binding sends the echo through the OS ICMP facility
// (IcmpSendEcho2) — the platform-primitive equivalent of retail's raw echo.
// Everything else — the target set, the pass/retry loop, the chunking, and
// the per-row fold — is the witnessed behavior and lives here, portable;
// the binding supplies one echo pass over a chunk of slots and the thread.

// Per-echo timeout and retry count [orig: NapiConnection_Init @0x63037c
// stores 3000 ms at conn+20 and 2 retries at conn+24].
inline constexpr uint32_t kPingTimeoutMs = 3000;
inline constexpr int kPingRetries = 2;

// The device wait is capped at 64 handles (WaitForMultipleObjects); a pass
// is handed over in chunks under that cap, and the wait carries a little
// scheduling slack over the witnessed per-echo timeout.
inline constexpr size_t kPingPassChunk = 60;
inline constexpr uint32_t kPingWaitSlackMs = 1000;

// The per-row result fold the browser applies before display: raw code -4
// (never attempted) reads as -3, every other negative (send/timeout/error)
// reads as -2, and a zero raw code means the entry carries a millisecond
// round-trip time [orig: NapiGameList_OnPingResult @0x63bc60 — raw -4 -> -3,
// -3/-2/-1 -> -2, 0 -> ms from the entry's transfer info].
inline constexpr int kPingNeverAttempted = -3;
inline constexpr int kPingFailed = -2;
inline int fold_ping_result(int raw_code, int ms) {
	if (raw_code == -4) return kPingNeverAttempted;
	if (raw_code < 0) return kPingFailed;
	return ms;
}

// One sweep slot: the browser row and its echo state. A row whose address
// the sweep cannot drive (unparseable, or the unreported 0.0.0.0) is never
// attempted and folds to kPingNeverAttempted; an attempted row the passes
// never answered folds to kPingFailed.
struct PingEcho {
	int64_t row_id = 0;
	std::string dotted;
	uint32_t addr_be = 0; // network-order IPv4, valid while `attempted`
	bool attempted = false;
	bool answered = false;
	int ms = 0;
};

// Plain a.b.c.d -> network-order IPv4; false for anything else.
inline bool parse_ping_ipv4(const std::string &dotted, uint32_t &out_be) {
	unsigned a = 0, b = 0, c = 0, d = 0;
	if (std::sscanf(dotted.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return false;
	if (a > 255 || b > 255 || c > 255 || d > 255) return false;
	out_be = a | (b << 8) | (c << 16) | (d << 24);
	return true;
}

// The sweep's slots for a row set: (row id, dotted IPv4) pairs. The
// unreported host address (empty / 0.0.0.0) is a row the sweep never
// attempts, and retail's fold surfaces exactly that (-4 -> -3) rather than
// leaving the browser's pending "..." forever.
inline std::vector<PingEcho>
make_ping_echoes(const std::vector<std::pair<int64_t, std::string>> &targets) {
	std::vector<PingEcho> echoes;
	echoes.reserve(targets.size());
	for (const auto &[row_id, dotted] : targets) {
		PingEcho e;
		e.row_id = row_id;
		e.dotted = dotted;
		e.attempted = parse_ping_ipv4(dotted, e.addr_be) && e.addr_be != 0;
		echoes.push_back(std::move(e));
	}
	return echoes;
}

// One device echo pass over a chunk of attempted, still-unanswered slots:
// fire every slot's echo, wait, and stamp `answered`/`ms` on the replies.
using PingEchoPass = std::function<void(std::vector<PingEcho *> &chunk)>;

// The pass loop: the initial pass plus kPingRetries more over whatever
// stayed unanswered, each handed to the device in wait-capped chunks
// [orig: the 2 retries NapiConnection_Init @0x63037c stores]. `cancelled`
// (optional) is polled between chunks and ends the sweep early.
inline void run_ping_passes(std::vector<PingEcho> &echoes, const PingEchoPass &pass,
                            const std::function<bool()> &cancelled = {}) {
	for (int attempt = 0; attempt <= kPingRetries; ++attempt) {
		std::vector<PingEcho *> pending;
		for (PingEcho &e : echoes)
			if (e.attempted && !e.answered) pending.push_back(&e);
		if (pending.empty()) return;
		for (size_t start = 0; start < pending.size(); start += kPingPassChunk) {
			if (cancelled && cancelled()) return;
			const size_t end = start + kPingPassChunk < pending.size()
					? start + kPingPassChunk
					: pending.size();
			std::vector<PingEcho *> chunk(pending.begin() + static_cast<std::ptrdiff_t>(start),
			                              pending.begin() + static_cast<std::ptrdiff_t>(end));
			pass(chunk);
		}
	}
}

// Every row's folded display value after the passes: never-attempted for a
// row the sweep could not drive, failed for an attempted row nothing answered,
// the round-trip ms otherwise [orig: NapiGameList_OnPingResult @0x63bc60].
inline std::vector<std::pair<int64_t, int>> fold_ping_sweep(const std::vector<PingEcho> &echoes) {
	std::vector<std::pair<int64_t, int>> out;
	out.reserve(echoes.size());
	for (const PingEcho &e : echoes) {
		const int folded = !e.attempted ? fold_ping_result(-4, 0)
				: !e.answered ? fold_ping_result(-1, 0)
				: fold_ping_result(0, e.ms);
		out.emplace_back(e.row_id, folded);
	}
	return out;
}

} // namespace opennova
