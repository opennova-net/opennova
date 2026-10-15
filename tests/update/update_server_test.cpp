// The update server's connection machine (engine/net/update/update_server): the stock client's
// whole sequence answered as UPDATE.EXE 2.1.4.2 reads it (docs/update/updater-re.md §3-§5): the
// gateway's login and SERVER.LST, the update server's login, the command file's CRC and the
// files, then CLOSE; and what the server refuses.

#include "common/test_expect.h"

#include <formats/update/server_list.h>
#include <formats/update/update_crc.h>
#include <net/update/update_server.h>
#include <net/update/update_wire.h>

#include <cstdio>
#include <map>
#include <string>
#include <vector>

using namespace opennova::update;

namespace {

std::vector<uint8_t> bytes(std::string_view text) {
	return std::vector<uint8_t>(text.begin(), text.end());
}

class FakeCatalog : public UpdateCatalog {
public:
	std::map<std::string, std::vector<uint8_t>> files = {
		{ "JOPTR.CMD", bytes("[HEADER]\r\n\r\n[COMMAND]\r\nUPD=version.txt,1,2\r\n") },
		{ "version.txt", bytes("1ZLN-ish payload") }
	};
	bool has_product(std::string_view login) const override { return login == "JOTRus"; }
	std::vector<std::string> server_hosts(std::string_view) const override {
		return { "ftp.novaworld.com" };
	}
	std::string command_file_name(std::string_view) const override { return "JOPTR.CMD"; }
	bool read_file(
			std::string_view, std::string_view name, std::vector<uint8_t> &out) const override {
		const auto it = files.find(std::string(name));
		if (it == files.end())
			return false;
		out = it->second;
		return true;
	}
};

// Every reply in `stream`, in order.
std::vector<Reply> replies(const std::vector<uint8_t> &stream) {
	std::vector<Reply> out;
	size_t at = 0;
	while (at < stream.size()) {
		Reply reply;
		size_t consumed = 0;
		if (decode_reply(stream.data() + at, stream.size() - at, reply, consumed) !=
				Frame::Complete)
			break;
		out.push_back(reply);
		at += consumed;
	}
	return out;
}

UpdateServer::Received send(UpdateServer &server, uint32_t connection, const std::string &request) {
	return server.receive(
			connection, reinterpret_cast<const uint8_t *>(request.data()), request.size());
}

int test_stock_sequence() {
	FakeCatalog catalog;
	UpdateServer server(catalog);

	// The gateway: LOGIN id -4, GET SERVER.LST id -3, then CLOSE (OPEN_GATEWAY / LOAD_BALANCE).
	const uint32_t gateway = server.open();
	UpdateServer::Received got = send(server, gateway, encode_login("JOTRus", "", kIdGatewayLogin));
	std::vector<Reply> r = replies(got.send);
	TEST_EXPECT(r.size() == 1 && r[0].verb == Verb::Login && r[0].id == -4 && r[0].result == 0);
	got = send(server, gateway, encode_get({ std::string(kServerListName) }, kIdServerList));
	r = replies(got.send);
	TEST_EXPECT(r.size() == 1 && r[0].verb == Verb::Get && r[0].id == -3 && r[0].result == 0);
	const std::vector<std::string> hosts =
			read_server_list(std::string(r[0].payload.begin(), r[0].payload.end()));
	TEST_EXPECT(hosts == std::vector<std::string>{ "ftp.novaworld.com" });
	got = send(server, gateway, encode_close());
	TEST_EXPECT(!got.keep_open && got.send.empty());
	server.close(gateway);

	// The update server: LOGIN id -2, CRC id -1, GET the command file id -1, GET a row by index.
	const uint32_t update = server.open();
	got = send(server, update,
			encode_login("JOTRus", "", kIdServerLogin) + encode_crc("JOPTR.CMD", kIdCommandFile));
	r = replies(got.send);
	TEST_EXPECT(r.size() == 2 && r[0].id == -2 && r[1].verb == Verb::Crc && r[1].id == -1);
	const std::vector<uint8_t> &cmd = catalog.files["JOPTR.CMD"];
	TEST_EXPECT(uint32_t(r[1].crc) == command_file_crc(cmd.data(), cmd.size()));
	got = send(server, update, encode_get({ "JOPTR.CMD" }, kIdCommandFile));
	r = replies(got.send);
	TEST_EXPECT(r.size() == 1 && r[0].name == "JOPTR.CMD" && r[0].payload == cmd);
	// A request arriving a byte at a time is answered once whole.
	const std::string get_row = encode_get({ "version.txt" }, 0);
	std::vector<uint8_t> stream;
	for (char c : get_row) {
		got = send(server, update, std::string(1, c));
		stream.insert(stream.end(), got.send.begin(), got.send.end());
	}
	r = replies(stream);
	TEST_EXPECT(r.size() == 1 && r[0].id == 0 && r[0].payload == catalog.files["version.txt"]);
	// A missing file fails with RESULT -1; LOADTEST answers a load.
	got = send(server, update, encode_get({ "absent.def" }, 3) + encode_load_test(1));
	r = replies(got.send);
	TEST_EXPECT(r.size() == 2 && r[0].result == -1 && r[0].payload.empty());
	TEST_EXPECT(r[1].verb == Verb::LoadTest && r[1].payload.size() == 1);
	got = send(server, update, encode_close());
	TEST_EXPECT(!got.keep_open);
	return 0;
}

int test_refusals() {
	FakeCatalog catalog;
	UpdateServer server(catalog);
	// An unknown product's login is refused and leaves the connection logged out.
	uint32_t c = server.open();
	UpdateServer::Received got = send(server, c, encode_login("NOPEus", "", kIdServerLogin));
	std::vector<Reply> r = replies(got.send);
	TEST_EXPECT(r.size() == 1 && r[0].result == -1 && got.keep_open);
	got = send(server, c, encode_get({ "JOPTR.CMD" }, -1));
	TEST_EXPECT(!got.keep_open && got.send.empty());
	// A name that would leave the download directory, and a malformed request, close.
	c = server.open();
	send(server, c, encode_login("JOTRus", "", kIdServerLogin));
	got = send(server, c, encode_get({ "..\\x.def" }, 0));
	TEST_EXPECT(!got.keep_open);
	c = server.open();
	got = send(server, c, "HELLO\r\n\r\n");
	TEST_EXPECT(!got.keep_open);
	return 0;
}

} // namespace

int main() {
	if (int rc = test_stock_sequence())
		return rc;
	if (int rc = test_refusals())
		return rc;
	std::printf("update_server: ok\n");
	return 0;
}
