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

namespace godot {

namespace {

#ifdef _WIN32

struct EchoHandles {
	HANDLE event = nullptr;
	std::vector<unsigned char> reply;
};

// One echo's completion: its reply stamped, and an answered echo handed to
// `replied` at once.
void harvest_echo(opennova::PingEcho &echo, EchoHandles &h,
		const opennova::PingSweepRunner::EchoReplied &replied) {
	const DWORD n = IcmpParseReplies(h.reply.data(), static_cast<DWORD>(h.reply.size()));
	if (n == 0) return;
	const ICMP_ECHO_REPLY *reply = reinterpret_cast<const ICMP_ECHO_REPLY *>(h.reply.data());
	if (reply->Status != IP_SUCCESS) return;
	echo.answered = true;
	echo.ms = static_cast<int>(reply->RoundTripTime);
	if (replied) replied(echo);
}

// One device echo pass over a chunk of slots (event-completion form): fire
// every echo, then take each completion as it signals, so each reply answers
// its row as it lands (the runner's per-reply leg carries the witness). The
// per-echo timeout is the witnessed 3000 ms; a completed request signals its
// event whether it succeeded or timed out, so the deadline only adds
// scheduling slack.
void run_echo_pass(HANDLE icmp, std::vector<opennova::PingEcho *> &chunk,
		const opennova::PingSweepRunner::EchoReplied &replied) {
	unsigned char payload[8] = {'o', 'p', 'e', 'n', 'n', 'o', 'v', 'a'};
	std::vector<EchoHandles> handles(chunk.size());
	std::vector<HANDLE> events;
	std::vector<size_t> in_flight;
	for (size_t i = 0; i < chunk.size(); ++i) {
		EchoHandles &h = handles[i];
		h.reply.assign(sizeof(ICMP_ECHO_REPLY) + sizeof(payload) + 8, 0);
		h.event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (h.event == nullptr) continue;
		const DWORD rc = IcmpSendEcho2(
				icmp, h.event, nullptr, nullptr, chunk[i]->addr_be, payload,
				sizeof(payload), nullptr, h.reply.data(),
				static_cast<DWORD>(h.reply.size()), opennova::kPingTimeoutMs);
		if (rc == 0 && GetLastError() != ERROR_IO_PENDING) {
			CloseHandle(h.event);
			h.event = nullptr;
			continue;
		}
		events.push_back(h.event);
		in_flight.push_back(i);
	}
	const ULONGLONG deadline =
			GetTickCount64() + opennova::kPingTimeoutMs + opennova::kPingWaitSlackMs;
	while (!events.empty()) {
		const ULONGLONG now = GetTickCount64();
		if (now >= deadline) break;
		const DWORD signaled = WaitForMultipleObjects(static_cast<DWORD>(events.size()),
				events.data(), FALSE, static_cast<DWORD>(deadline - now));
		// The deadline or a failed wait ends the pass (WAIT_OBJECT_0 is 0).
		if (signaled >= static_cast<DWORD>(events.size())) break;
		const size_t k = static_cast<size_t>(signaled);
		harvest_echo(*chunk[in_flight[k]], handles[in_flight[k]], replied);
		events.erase(events.begin() + static_cast<std::ptrdiff_t>(k));
		in_flight.erase(in_flight.begin() + static_cast<std::ptrdiff_t>(k));
	}
	for (EchoHandles &h : handles) {
		if (h.event != nullptr) CloseHandle(h.event);
	}
}

#endif // _WIN32

} // namespace

void PingSweepWorker::start(std::vector<std::pair<int64_t, std::string>> targets,
                            Callable reply_sink, Callable sink, int64_t generation) {
	runner_.start(std::move(targets),
			[](std::vector<opennova::PingEcho> &echoes, const std::function<bool()> &cancelled,
					const opennova::PingSweepRunner::EchoReplied &replied) {
#ifdef _WIN32
				const HANDLE icmp = IcmpCreateFile();
				if (icmp != INVALID_HANDLE_VALUE) {
					// A chunk's echoes complete (or time out) before the pass returns, so
					// their reply buffers outlive every OS write; the cancel poll sits
					// between chunks.
					opennova::run_ping_passes(
							echoes,
							[icmp, &replied](std::vector<opennova::PingEcho *> &chunk) {
								run_echo_pass(icmp, chunk, replied);
							},
							cancelled);
					IcmpCloseHandle(icmp);
				} else {
					for (opennova::PingEcho &e : echoes) e.attempted = false; // no facility
				}
#else
				// No unprivileged ICMP facility modeled off Windows: every row settles
				// as retail's never-attempted fold so the browser's pending "..."
				// terminates.
				(void)cancelled;
				(void)replied;
				for (opennova::PingEcho &e : echoes) e.attempted = false;
#endif
			},
			[reply_sink](int64_t rid, int ms, int64_t p_generation) {
				reply_sink.call_deferred(rid, ms, p_generation);
			},
			[sink](std::vector<std::pair<int64_t, int>> rows, int64_t p_generation) {
				Dictionary results;
				for (const auto &[rid, ping] : rows) results[rid] = ping;
				sink.call_deferred(results, p_generation);
			},
			generation);
}

} // namespace godot
