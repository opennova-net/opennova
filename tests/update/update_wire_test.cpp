// The updater's wire (engine/net/update): the requests byte for byte as UPDATE.EXE 2.1.4.2 builds
// them, the server's reading of them, and the replies a server sends as the client's parser reads
// them, its closed grammar included (docs/update/updater-re.md §4).

#include "common/test_expect.h"

#include <net/update/update_wire.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace opennova::update;

namespace {

std::vector<uint8_t> bytes(std::string_view text) {
	return std::vector<uint8_t>(text.begin(), text.end());
}

int test_requests() {
	// The client's empty password leaves a trailing space.
	TEST_EXPECT(encode_login("JOTRus", "", kIdGatewayLogin) == "LOGIN JOTRus \r\nID -4\r\n\r\n");
	TEST_EXPECT(encode_get({ "SERVER.LST" }, kIdServerList) == "GET SERVER.LST\r\nID -3\r\n\r\n");
	TEST_EXPECT(encode_get({ "a.def", "b.def" }, 7) == "GET a.def b.def\r\nID 7\r\n\r\n");
	TEST_EXPECT(encode_crc("JOPTR.CMD", kIdCommandFile) == "CRC JOPTR.CMD\r\nID -1\r\n\r\n");
	TEST_EXPECT(encode_load_test(2) == "LOADTEST\r\nID 2\r\n\r\n");
	TEST_EXPECT(encode_close() == "CLOSE\r\n\r\n");

	// The server reads each back, two pipelined in one buffer.
	const std::string two =
			encode_login("JOTRus", "", kIdServerLogin) + encode_crc("JOPTR.CMD", -1);
	Request request;
	size_t consumed = 0;
	TEST_EXPECT(decode_request(two, request, consumed) == Frame::Complete);
	TEST_EXPECT(
			request.verb == Verb::Login && request.args.size() == 1 && request.args[0] == "JOTRus");
	TEST_EXPECT(request.id == kIdServerLogin);
	const std::string rest = two.substr(consumed);
	TEST_EXPECT(decode_request(rest, request, consumed) == Frame::Complete);
	TEST_EXPECT(request.verb == Verb::Crc && request.args[0] == "JOPTR.CMD" && request.id == -1);
	TEST_EXPECT(decode_request(encode_close(), request, consumed) == Frame::Complete);
	TEST_EXPECT(request.verb == Verb::Close && !request.id);
	TEST_EXPECT(
			decode_request("GET a.def b.def\r\nID 7\r\n", request, consumed) == Frame::NeedMore);
	TEST_EXPECT(decode_request("PUT x\r\nID 1\r\n\r\n", request, consumed) == Frame::Malformed);
	TEST_EXPECT(decode_request("GET x\r\nHOST y\r\n\r\n", request, consumed) == Frame::Malformed);
	TEST_EXPECT(decode_request(std::string(kMaxRequestHeaderBytes + 1, 'A'), request, consumed) ==
			Frame::Malformed);
	return 0;
}

int test_replies() {
	Reply reply;
	size_t consumed = 0;
	const std::vector<uint8_t> login = encode_login_reply(kIdGatewayLogin, 0);
	TEST_EXPECT(login == bytes("LOGIN\r\nID -4\r\nRESULT 0\r\n\r\n"));
	TEST_EXPECT(decode_reply(login.data(), login.size(), reply, consumed) == Frame::Complete);
	TEST_EXPECT(reply.verb == Verb::Login && reply.id == -4 && reply.result == 0);
	TEST_EXPECT(consumed == login.size());

	const std::vector<uint8_t> refused = encode_login_reply(kIdServerLogin, -1);
	TEST_EXPECT(decode_reply(refused.data(), refused.size(), reply, consumed) == Frame::Complete);
	TEST_EXPECT(reply.result == -1);

	const std::vector<uint8_t> list = bytes("a.example\r\n");
	const std::vector<uint8_t> get =
			encode_get_reply("SERVER.LST", kIdServerList, list.data(), list.size());
	TEST_EXPECT(
			get == bytes("GET SERVER.LST\r\nID -3\r\nSIZE 11\r\nRESULT 0\r\n\r\na.example\r\n"));
	// The whole payload must be buffered.
	TEST_EXPECT(decode_reply(get.data(), get.size() - 1, reply, consumed) == Frame::NeedMore);
	TEST_EXPECT(decode_reply(get.data(), get.size(), reply, consumed) == Frame::Complete);
	TEST_EXPECT(reply.verb == Verb::Get && reply.name == "SERVER.LST" && reply.payload == list);

	// A CRC goes out signed and comes back as the same 32 bits.
	const std::vector<uint8_t> crc = encode_crc_reply(0xFC891918u, kIdCommandFile);
	TEST_EXPECT(crc == bytes("CRC -58124008\r\nID -1\r\n\r\n"));
	TEST_EXPECT(decode_reply(crc.data(), crc.size(), reply, consumed) == Frame::Complete);
	TEST_EXPECT(uint32_t(reply.crc) == 0xFC891918u);

	const std::vector<uint8_t> load = encode_load_test_reply(3, 42);
	TEST_EXPECT(decode_reply(load.data(), load.size(), reply, consumed) == Frame::Complete);
	TEST_EXPECT(reply.verb == Verb::LoadTest && reply.id == 3 &&
			reply.payload == std::vector<uint8_t>{ 42 });

	// The client's reading: any line order, lower case, LOGIN by its first three letters.
	const std::vector<uint8_t> loose = bytes("size 2\r\nid 9\r\nget x.def\r\n\r\nhi");
	TEST_EXPECT(decode_reply(loose.data(), loose.size(), reply, consumed) == Frame::Complete);
	TEST_EXPECT(reply.verb == Verb::Get && reply.id == 9 && reply.name == "x.def");
	const std::vector<uint8_t> logx = bytes("LOGXYZ\r\nID 1\r\n\r\n");
	TEST_EXPECT(decode_reply(logx.data(), logx.size(), reply, consumed) == Frame::Complete);
	TEST_EXPECT(reply.verb == Verb::Login);

	// The closed grammar: a line retail does not know, a name past 31 characters.
	const std::vector<uint8_t> unknown = bytes("LOGIN\r\nX-HEADER 1\r\n\r\n");
	TEST_EXPECT(decode_reply(unknown.data(), unknown.size(), reply, consumed) == Frame::Malformed);
	const std::vector<uint8_t> long_name =
			bytes("GET abcdefghijklmnopqrstuvwxyz012345\r\nID 1\r\n\r\n");
	TEST_EXPECT(
			decode_reply(long_name.data(), long_name.size(), reply, consumed) == Frame::Malformed);
	const std::vector<uint8_t> no_verb = bytes("ID 1\r\n\r\n");
	TEST_EXPECT(decode_reply(no_verb.data(), no_verb.size(), reply, consumed) == Frame::Malformed);
	return 0;
}

} // namespace

int main() {
	if (int rc = test_requests())
		return rc;
	if (int rc = test_replies())
		return rc;
	std::printf("update_wire: ok\n");
	return 0;
}
