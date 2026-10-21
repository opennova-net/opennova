// opennova-serve's lister binds its gate socket as retail's gate worker does (D-NET-NEW-L9-5):
// on every interface over the process block's mpgateserverlocalport quad, the first free port of
// its scan. The shipped block's quad (49152 / 65536 / 1 / 0) scans 49152..65535 from 49152; an
// embedder with no block takes it. A held port of the scan is passed over for the next.
// [orig: CNapiGateManager_WaitForConnect @0x4CE7D2 (the bind address 0), @0x4CE7DE..0x4CE810
//  (the quad); CNapiGateManager_CreateAndBindSocket @0x633770 (@0x6337D4..0x633824, the call
//  @0x633830)]

#include "lister.h"
#include "listing_source.h"
#include "net_sockets.h"

#include <base/gameprofile/gameprofile.h>
#include <formats/gamecfg/game_cfg.h>
#include <net/title/title_identity.h>
#include <runtime/inmatch/host_config.h>
#include <runtime/inmatch/punt_log.h>   // ConnectLog
#include <runtime/inmatch/server_files.h>

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

using namespace opennova;

namespace {

int failures = 0;
#define CHECK(c)                                                                                   \
	do {                                                                                           \
		if (!(c)) {                                                                                \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                               \
			++failures;                                                                            \
		}                                                                                          \
	} while (0)

// A listing with nothing to list: the lister's start needs a source that starts.
class EmptySource final : public serve::ListingSource {
public:
	bool start(int &exit_code) override {
		(void)exit_code;
		return true;
	}
	bool refresh(uint32_t now_ms, bool force) override {
		(void)now_ms;
		(void)force;
		return false;
	}
	HostRegistration registration() const override { return {}; }
	std::vector<HostPlayerSlot> wanted_roster(const std::map<int, HostPlayerSlot> &current) const override {
		(void)current;
		return {};
	}
	void on_command(const ServerCommand &command) override { (void)command; }
};

// The connect log's file, in memory.
struct MemoryFiles final : inmatch::ServerFileSink {
	std::map<std::string, std::string> files;
	bool exists(const std::string &name) override { return files.count(name) != 0; }
	void remove(const std::string &name) override { files.erase(name); }
	bool write(const std::string &name, std::string_view bytes, bool append) override {
		if (!append) files[name].clear();
		files[name].append(bytes.data(), bytes.size());
		return true;
	}
};

// A UDP port nothing holds right now: bound by the OS, then released.
uint16_t free_port() {
	uint16_t port = 0;
	net::ScopedSocket probe(net::udp_bind(0, &port, 0));
	return probe.is_valid() ? port : uint16_t{0};
}

} // namespace

int main() {
	CHECK(net::startup() == 0);
	const TitleIdentity &jo = *title_identity(gameprofile::GAME_JO);

	// The default scan is the shipped block's quad.
	const serve::ListerOptions defaults;
	const std::vector<uint16_t> shipped = inmatch::gate_bind_ports(gamecfg::defaults());
	CHECK(defaults.gate_ports == shipped);
	CHECK(!shipped.empty() && shipped.front() == 49152 && shipped.back() == 65535 &&
	      shipped.size() == 16384);

	// A scan whose first port is held binds the next one.
	uint16_t held_port = 0;
	net::ScopedSocket held(net::udp_bind(0, &held_port, 0));
	const uint16_t next = free_port();
	CHECK(held.is_valid() && held_port != 0 && next != 0 && next != held_port);
	EmptySource source;
	serve::ListerOptions options;
	options.title = &jo;
	options.gate_tag = jo.gate_tag;
	options.master_host = "127.0.0.1";
	options.master_gate_port = free_port();
	options.gate_ports = {held_port, next};
	{
		serve::Lister lister(options, source, nullptr);
		CHECK(lister.start());
		CHECK(lister.gate_port() == next);
	}
	// A scan with every port held binds nothing: the start fails, and with the connect log on
	// the gate wait's line names the manager's state, 2 (the bind's state; the fall back to -6
	// is out of the setter's range). [orig: CNapiGateManager_ProcessConnectStateMachine
	// @0x633E3C, @0x633E64; CNapiGateManager_SetState @0x633886; CNapiGateManager_WaitForConnect
	// @0x4CE8C2..0x4CE8E7]
	options.gate_ports = {held_port};
	MemoryFiles files;
	inmatch::ConnectLog log(files);
	log.set_clock([] { return std::string("T"); });
	log.start(inmatch::ConnectLogHeader{});
	const std::size_t header_bytes = files.files[inmatch::kConnectLogFile].size();
	options.connect_log = &log;
	{
		serve::Lister lister(options, source, nullptr);
		CHECK(!lister.start());
		CHECK(lister.exit_code() == serve::kExitNetwork);
	}
	CHECK(files.files[inmatch::kConnectLogFile].substr(header_bytes) ==
	      "T : GATE GATE SERVICE START FAILED [2].\r\n");
	net::shutdown();
	if (failures != 0) {
		std::printf("opennova_serve_lister_gate_bind: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("opennova_serve_lister_gate_bind: ok\n");
	return 0;
}
