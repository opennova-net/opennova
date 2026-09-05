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

// One device echo pass over a chunk of slots (event-completion form): fire
// every echo, wait for the chunk, and harvest the replies. The per-echo
// timeout is the witnessed 3000 ms; a completed request signals its event
// whether it succeeded or timed out, so the wait only adds scheduling slack.
void run_echo_pass(HANDLE icmp, std::vector<opennova::PingEcho *> &chunk) {
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
	if (!events.empty()) {
		WaitForMultipleObjects(static_cast<DWORD>(events.size()), events.data(), TRUE,
		                       opennova::kPingTimeoutMs + opennova::kPingWaitSlackMs);
	}
	for (size_t i : in_flight) {
		EchoHandles &h = handles[i];
		const DWORD n = IcmpParseReplies(h.reply.data(), static_cast<DWORD>(h.reply.size()));
		if (n > 0) {
			const ICMP_ECHO_REPLY *reply =
					reinterpret_cast<const ICMP_ECHO_REPLY *>(h.reply.data());
			if (reply->Status == IP_SUCCESS) {
				chunk[i]->answered = true;
				chunk[i]->ms = static_cast<int>(reply->RoundTripTime);
			}
		}
	}
	for (EchoHandles &h : handles) {
		if (h.event != nullptr) CloseHandle(h.event);
	}
}

#endif // _WIN32

Dictionary fold_to_dictionary(const std::vector<opennova::PingEcho> &echoes) {
	Dictionary results;
	for (const auto &[rid, ping] : opennova::fold_ping_sweep(echoes)) results[rid] = ping;
	return results;
}

} // namespace

PingSweepWorker::~PingSweepWorker() { cancel(); }

void PingSweepWorker::cancel() {
	cancel_.store(true);
	if (thread_.joinable()) thread_.join();
	cancel_.store(false);
}

bool PingSweepWorker::start(std::vector<std::pair<int64_t, std::string>> targets,
                            Callable sink, int64_t generation) {
	if (running_.load()) return false;
	if (thread_.joinable()) thread_.join(); // the previous sweep finished; reap it
	running_.store(true);
	cancel_.store(false);
	thread_ = std::thread([this, targets = std::move(targets), sink = std::move(sink),
	                       generation]() mutable {
		std::vector<opennova::PingEcho> echoes = opennova::make_ping_echoes(targets);
#ifdef _WIN32
		const HANDLE icmp = IcmpCreateFile();
		if (icmp != INVALID_HANDLE_VALUE) {
			opennova::run_ping_passes(
					echoes, [icmp](std::vector<opennova::PingEcho *> &chunk) { run_echo_pass(icmp, chunk); },
					[this]() { return cancel_.load(); });
			IcmpCloseHandle(icmp);
		} else {
			for (opennova::PingEcho &e : echoes) e.attempted = false; // no facility: never attempted
		}
#else
		// No unprivileged ICMP facility modeled off Windows: every row settles
		// as retail's never-attempted fold so the browser's pending "..."
		// terminates.
		for (opennova::PingEcho &e : echoes) e.attempted = false;
#endif
		if (!cancel_.load()) sink.call_deferred(fold_to_dictionary(echoes), generation);
		running_.store(false);
	});
	return true;
}

} // namespace godot
