// The NWU lobby driver's own draws (NwuLobbySession with no Environment::random_u32, what
// opennova-nw-lister runs on): 32 sessions opened on 32 threads at once each draw a client index
// and key no other session shares. A per-thread generator seeded from std::random_device fails
// this where libstdc++ serves random_device from RDSEED on a CPU with AMD's RDSEED erratum: the
// threads whose seed came back 0 open with the same index and key.

#include <net/novaworld/nwu_lobby_session.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <set>
#include <thread>
#include <vector>

using namespace opennova;

namespace {

class NullSocket final : public IDatagramSocket {
public:
	int recv_from(uint8_t *, std::size_t, PeerAddr &) override { return 0; }
	void send_to(const PeerAddr &, const uint8_t *, std::size_t) override {}
};

} // namespace

int main() {
	constexpr int kThreads = 32;
	struct Draw {
		uint32_t index = 0;
		uint32_t key = 0;
	};
	std::vector<Draw> draws(kThreads);
	std::atomic<int> ready{0};
	std::atomic<bool> go{false};
	std::vector<std::thread> threads;
	threads.reserve(kThreads);
	for (int t = 0; t < kThreads; ++t) {
		threads.emplace_back([&, t] {
			NullSocket gate;
			NullSocket session;
			NwuLobbySession lobby(NwuLobbySession::Hooks{}, NwuLobbySession::Environment{});
			ready.fetch_add(1);
			while (!go.load()) std::this_thread::yield();
			lobby.open(gate, session);
			draws[t] = {lobby.client_index(), lobby.client_key()};
		});
	}
	while (ready.load() < kThreads) std::this_thread::yield();
	go.store(true);
	for (std::thread &thread : threads) thread.join();

	std::set<uint32_t> seen;
	for (const Draw &d : draws) {
		seen.insert(d.index);
		seen.insert(d.key);
	}
	// 64 uniform 32-bit draws repeat with probability about 2016 / 2^32.
	if (seen.size() != 2 * static_cast<size_t>(kThreads)) {
		std::fprintf(stderr, "FAIL: %zu distinct client index / key values over %d sessions\n",
		             seen.size(), kThreads);
		return 1;
	}
	std::printf("OK: %d lobby sessions opened at once draw %zu distinct index / key values\n",
	            kThreads, seen.size());
	return 0;
}
