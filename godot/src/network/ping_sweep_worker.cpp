#include "network/ping_sweep_worker.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#endif

#include <net/novaworld/ping_sweep.h>

#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <algorithm>
#include <cstdio>
#include <thread>

namespace godot {

namespace {

#ifdef _WIN32

// One dotted quad -> network-order IPAddr. Returns false for anything that is
// not a plain a.b.c.d (the unreported "0.0.0.0" rows are filtered by the
// caller).
bool parse_ipv4(const std::string &dotted, unsigned long &out) {
	unsigned a = 0, b = 0, c = 0, d = 0;
	if (std::sscanf(dotted.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return false;
	if (a > 255 || b > 255 || c > 255 || d > 255) return false;
	out = static_cast<unsigned long>(a) | (static_cast<unsigned long>(b) << 8) |
	      (static_cast<unsigned long>(c) << 16) | (static_cast<unsigned long>(d) << 24);
	return true;
}

struct EchoSlot {
	int64_t rid = 0;
	unsigned long addr = 0;
	HANDLE event = nullptr;
	std::vector<unsigned char> reply;
	bool answered = false;
	int ms = 0;
};

// Fire every slot's echo concurrently (event-completion form) and harvest.
// The per-echo timeout is the witnessed 3000 ms; a completed request signals
// its event whether it succeeded or timed out.
void run_echo_pass(HANDLE icmp, std::vector<EchoSlot *> &slots) {
	static unsigned char payload[8] = {'o', 'p', 'e', 'n', 'n', 'o', 'v', 'a'};
	std::vector<HANDLE> events;
	std::vector<EchoSlot *> in_flight;
	for (EchoSlot *slot : slots) {
		slot->reply.assign(sizeof(ICMP_ECHO_REPLY) + sizeof(payload) + 8, 0);
		slot->event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (slot->event == nullptr) continue;
		const DWORD rc = IcmpSendEcho2(
				icmp, slot->event, nullptr, nullptr, slot->addr, payload,
				sizeof(payload), nullptr, slot->reply.data(),
				static_cast<DWORD>(slot->reply.size()), opennova::kPingTimeoutMs);
		if (rc == 0 && GetLastError() != ERROR_IO_PENDING) {
			CloseHandle(slot->event);
			slot->event = nullptr;
			continue;
		}
		events.push_back(slot->event);
		in_flight.push_back(slot);
	}
	if (!events.empty()) {
		// Every request self-completes at its timeout; the extra headroom only
		// covers scheduling. WaitForMultipleObjects caps at 64 handles — the
		// caller chunks the slots accordingly.
		WaitForMultipleObjects(static_cast<DWORD>(events.size()), events.data(),
		                       TRUE, opennova::kPingTimeoutMs + 1000);
	}
	for (EchoSlot *slot : in_flight) {
		const DWORD n = IcmpParseReplies(slot->reply.data(),
		                                 static_cast<DWORD>(slot->reply.size()));
		if (n > 0) {
			const ICMP_ECHO_REPLY *reply =
					reinterpret_cast<const ICMP_ECHO_REPLY *>(slot->reply.data());
			if (reply->Status == IP_SUCCESS) {
				slot->answered = true;
				slot->ms = static_cast<int>(reply->RoundTripTime);
			}
		}
	}
	for (EchoSlot *slot : slots) {
		if (slot->event != nullptr) {
			CloseHandle(slot->event);
			slot->event = nullptr;
		}
	}
}

void sweep_thread(std::vector<std::pair<int64_t, std::string>> targets,
                  Callable sink, int64_t generation) {
	Dictionary results;
	const HANDLE icmp = IcmpCreateFile();
	std::vector<EchoSlot> slots;
	if (icmp != INVALID_HANDLE_VALUE) {
		slots.reserve(targets.size());
		for (const auto &[rid, dotted] : targets) {
			unsigned long addr = 0;
			if (!parse_ipv4(dotted, addr)) continue;
			EchoSlot slot;
			slot.rid = rid;
			slot.addr = addr;
			slots.push_back(std::move(slot));
		}
		// The initial pass plus the witnessed retries over whatever stayed
		// unanswered, in wait-capped chunks of 60.
		for (int pass = 0; pass <= opennova::kPingRetries; ++pass) {
			std::vector<EchoSlot *> pending;
			for (EchoSlot &slot : slots)
				if (!slot.answered) pending.push_back(&slot);
			if (pending.empty()) break;
			for (size_t start = 0; start < pending.size(); start += 60) {
				std::vector<EchoSlot *> chunk(
						pending.begin() + start,
						pending.begin() +
								std::min(start + 60, pending.size()));
				run_echo_pass(icmp, chunk);
			}
		}
		IcmpCloseHandle(icmp);
	}
	for (const EchoSlot &slot : slots) {
		results[slot.rid] = slot.answered
				? opennova::fold_ping_result(0, slot.ms)
				: opennova::fold_ping_result(-1, 0);
	}
	sink.call_deferred(results, generation);
}

#endif // _WIN32

} // namespace

void run_ping_sweep(std::vector<std::pair<int64_t, std::string>> targets,
                    Callable sink, int64_t generation) {
#ifdef _WIN32
	std::thread(sweep_thread, std::move(targets), std::move(sink), generation)
			.detach();
#else
	// No unprivileged ICMP facility modeled off Windows: one empty pass so the
	// browser settles its pending state.
	(void)targets;
	sink.call_deferred(Dictionary(), generation);
#endif
}

} // namespace godot
