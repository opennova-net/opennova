// The /INOUT file (D-NET-356): the protocol's _inout.txt NapiLog, turned on at the socket open
// by the switch, its start line then and the HOST STARTED line at the server start, each entry
// stamped and CR LF ended in the buffer, flushed once 5000 ms have passed (the first flush
// truncating the file, the later ones appending), the shutdown's stop line; with the switch
// unset, no line is buffered and no file is written.
// [orig: NapiNPProtocol_Create @0x625AE5..0x625B07 (CNapiLog_Init "_inout.txt", 3, 6, 5000);
//  CNapiNetwork_OpenTransportSocket @0x4C6D02..0x4C6D15; NapiNPProtocol_StartServer ->
//  CNapiNPConnection_LogHostStarted @0x62B629; NapiLog_SetEnabled @0x62F2B0;
//  NapiLog_WriteEntry @0x62EE30; NapiLog_FlushIfExpired @0x62F2F0; NapiLog_EnsureOpen
//  @0x62EDC0; NapiLog_Shutdown @0x62F310]
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_console.h>
#include <runtime/inmatch/server_session.h>

#include <cstdio>
#include <map>
#include <string>

using namespace opennova;

namespace {

int failures = 0;
#define CHECK(c) \
	do { \
		if (!(c)) { \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); \
			++failures; \
		} \
	} while (0)

class MemoryFiles final : public inmatch::ServerFileSink {
public:
	std::map<std::string, std::string> files;
	int opens = 0;
	bool exists(const std::string &name) override { return files.count(name) != 0; }
	void remove(const std::string &name) override { files.erase(name); }
	bool write(const std::string &name, std::string_view bytes, bool append) override {
		++opens;
		std::string &f = files[name];
		if (!append) f.clear();
		f.append(bytes.data(), bytes.size());
		return true;
	}
};

constexpr const char *kStamp = "2026-10-20 11:02:03: ";

struct Host {
	inmatch::NapiNPServerCtx ctx;
	MemoryFiles files;
	inmatch::NapiLog inout{files, inmatch::kInoutLogFileName};
	explicit Host(bool inout_switch) {
		inmatch::set_connection_mode(ctx, inmatch::ConnectionMode::HostOnly);
		ctx.np_protocol.session_name = "Untitled";
		inout.set_stamp_text([] { return std::string(kStamp); });
		ctx.logs.inout = &inout;
		ctx.logs.inout_switch = inout_switch;
		ctx.logs.files = &files;
	}
};

void test_switch_on() {
	Host host(true);
	host.files.files["_inout.txt"] = "an earlier run\r\n";
	inmatch::start_server(host.ctx, inmatch::SessionStartup{});
	const std::string started = std::string(kStamp) + "NapiLog Started!\r\n" + kStamp +
			inmatch::inout_host_line(host.ctx, true) + "\r\n";
	CHECK(host.inout.enabled() && host.inout.pending() == started);
	CHECK(inmatch::inout_host_line(host.ctx, true).find(" : HOST STARTED \"Untitled\".") != std::string::npos);
	host.inout.flush_if_expired(4999);
	CHECK(host.files.opens == 0); // buffered until the interval passes
	host.inout.flush_if_expired(5000);
	CHECK(host.files.files["_inout.txt"] == started); // the first flush truncates
	inmatch::NapiNPConnection joiner;
	joiner.player_name = "Joe";
	inmatch::write_inout_line(host.ctx, inmatch::inout_player_added_line(joiner));
	host.inout.flush_if_expired(9999);
	CHECK(host.files.opens == 1);
	host.inout.flush_if_expired(10000);
	const std::string added = std::string(kStamp) + inmatch::inout_player_added_line(joiner) + "\r\n";
	CHECK(host.files.files["_inout.txt"] == started + added); // appended
	host.inout.shutdown(12000);
	CHECK(!host.inout.enabled() &&
	      host.files.files["_inout.txt"] == started + added + kStamp + "NapiLog Stopped!\r\n");
}

void test_switch_off() {
	Host host(false);
	inmatch::start_server(host.ctx, inmatch::SessionStartup{});
	CHECK(!host.inout.enabled() && host.inout.pending().empty());
	inmatch::write_inout_line(host.ctx, "a line");
	CHECK(host.inout.pending().empty()); // the writers test the log's on byte
	host.inout.flush_if_expired(5000);
	host.inout.shutdown(6000);
	CHECK(host.files.opens == 0 && host.files.files.empty()); // nothing opens the file
}

} // namespace

int main() {
	test_switch_on();
	test_switch_off();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("inout_log: ok\n");
	return 0;
}
