// The update server's connection machine (engine/net/update/update_server): the stock client's
// whole sequence answered as UPDATE.EXE 2.1.4.2 reads it (docs/update/updater-re.md §3-§5): the
// gateway's login and SERVER.LST, the update server's login, the command file's CRC and the
// files, then CLOSE; what the server refuses; pipelined GETs of large files answered a send
// budget at a time; and files served from disk as their replies go out.

#include "common/temp_dir.h"
#include "common/test_expect.h"

#include <base/io/os_path.h>
#include <formats/update/server_list.h>
#include <formats/update/update_crc.h>
#include <net/update/update_server.h>
#include <net/update/update_wire.h>

#include <cstdio>
#include <map>
#include <memory>
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

// A file `size` bytes long whose every byte differs from its neighbours.
std::vector<uint8_t> patterned(size_t size) {
	std::vector<uint8_t> out(size);
	for (size_t i = 0; i < size; ++i)
		out[i] = static_cast<uint8_t>(i * 31 + 7);
	return out;
}

// The replies to `first` and every receive(c, nullptr, 0) after it while `more`, each call's
// replies within kSendBudget (a reply header past it at most), all in one stream.
bool drain(UpdateServer &server, uint32_t c, UpdateServer::Received first,
		std::vector<uint8_t> &stream, size_t &calls) {
	UpdateServer::Received got = std::move(first);
	for (calls = 1;; ++calls) {
		if (got.send.size() > UpdateServer::kSendBudget + 256)
			return false;
		stream.insert(stream.end(), got.send.begin(), got.send.end());
		if (!got.more)
			return got.keep_open;
		got = server.receive(c, nullptr, 0);
	}
}

// Many GETs pipelined in one read, each naming a file past the budget several times, are
// answered a budget at a time: every call's replies within kSendBudget, `more` while replies
// wait, and the stream they make the same replies in the same order, each body whole, as
// one call would have made.
int test_send_budget() {
	FakeCatalog catalog;
	const std::vector<uint8_t> big = patterned(200 * 1024 + 7);
	catalog.files["big.bin"] = big;
	UpdateServer server(catalog);
	const uint32_t c = server.open();
	send(server, c, encode_login("JOTRus", "", kIdServerLogin));
	std::string pipelined;
	for (int32_t id = 0; id < 3; ++id)
		pipelined += encode_get({ "big.bin", "big.bin", "absent.def", "big.bin" }, id);
	pipelined += encode_get({ "version.txt" }, 9) + encode_load_test(10);
	UpdateServer::Received first = send(server, c, pipelined);
	TEST_EXPECT(first.more);
	std::vector<uint8_t> stream;
	size_t calls = 0;
	TEST_EXPECT(drain(server, c, std::move(first), stream, calls));
	TEST_EXPECT(calls >= (9 * big.size()) / UpdateServer::kSendBudget);
	const std::vector<Reply> r = replies(stream);
	TEST_EXPECT(r.size() == 14);
	for (size_t i = 0; i < 12; ++i) {
		TEST_EXPECT(r[i].verb == Verb::Get && r[i].id == int32_t(i / 4));
		if (i % 4 == 2)
			TEST_EXPECT(r[i].result == -1 && r[i].payload.empty());
		else
			TEST_EXPECT(r[i].result == 0 && r[i].payload == big);
	}
	TEST_EXPECT(r[12].id == 9 && r[12].payload == catalog.files["version.txt"]);
	TEST_EXPECT(r[13].verb == Verb::LoadTest && r[13].id == 10);
	// A bad name after big ones closes once their replies went.
	stream.clear();
	TEST_EXPECT(!drain(server, c, send(server, c, encode_get({ "big.bin", "a/b" }, 4)), stream,
			calls));
	TEST_EXPECT(replies(stream).size() == 1 && replies(stream)[0].payload == big);
	return 0;
}

// A catalog serving its files from disk: each GET reply's body read from the file as it goes
// out (UpdateFile::from_disk), byte for byte; a file that does not open fails with RESULT -1,
// and one that stops reading partway ends the connection after its header.
class DiskCatalog : public FakeCatalog {
public:
	std::string dir;
	std::unique_ptr<UpdateFile> open_file(
			std::string_view login, std::string_view name) const override {
		if (name == "short.bin")
			return std::make_unique<ShortFile>();
		if (name == "version.txt")
			return FakeCatalog::open_file(login, name);
		return UpdateFile::from_disk(dir + "/" + std::string(name));
	}

private:
	class ShortFile : public UpdateFile {
	public:
		uint64_t size() const override { return 1000; }
		bool read(uint8_t *, size_t) override { return false; }
	};
};

int test_disk_files() {
	test_temp::TempDir temp("update_server");
	const std::vector<uint8_t> big = patterned(150 * 1024 + 3);
	{
		std::FILE *f = opennova::io::fopen_utf8(temp.file("disk.bin").c_str(), "wb");
		TEST_EXPECT(f != nullptr);
		TEST_EXPECT(std::fwrite(big.data(), 1, big.size(), f) == big.size());
		std::fclose(f);
	}
	DiskCatalog catalog;
	catalog.dir = temp.root();
	UpdateServer server(catalog);
	const uint32_t c = server.open();
	send(server, c, encode_login("JOTRus", "", kIdServerLogin));
	std::vector<uint8_t> stream;
	size_t calls = 0;
	TEST_EXPECT(drain(server, c,
			send(server, c, encode_get({ "disk.bin", "gone.bin", "version.txt" }, 2)), stream,
			calls));
	TEST_EXPECT(calls >= 3);
	const std::vector<Reply> r = replies(stream);
	TEST_EXPECT(r.size() == 3 && r[0].payload == big && r[0].size == int32_t(big.size()));
	TEST_EXPECT(r[1].result == -1 && r[2].payload == catalog.files["version.txt"]);
	UpdateServer::Received got = send(server, c, encode_get({ "short.bin" }, 3));
	TEST_EXPECT(!got.keep_open);
	const std::string header(got.send.begin(), got.send.end());
	TEST_EXPECT(header == "GET short.bin\r\nID 3\r\nSIZE 1000\r\nRESULT 0\r\n\r\n");
	return 0;
}

} // namespace

int main() {
	if (int rc = test_stock_sequence())
		return rc;
	if (int rc = test_refusals())
		return rc;
	if (int rc = test_send_budget())
		return rc;
	if (int rc = test_disk_files())
		return rc;
	std::printf("update_server: ok\n");
	return 0;
}
