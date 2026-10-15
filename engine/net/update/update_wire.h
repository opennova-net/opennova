#pragma once

// The updater's wire: the plain-text TCP protocol between NovaLogic's UPDATE.EXE and its
// gateway and update servers, both directions. Wire only: socket I/O is the embedder's.
// docs/update/updater-re.md §4.
// [orig: NLUpdateProtocol_Login @ 0x401b50, NLUpdateProtocol_Get @ 0x401f80,
//  NLUpdateProtocol_Crc @ 0x4022f0, NLUpdateProtocol_LoadTest @ 0x402660,
//  NLUpdateProtocol_Close @ 0x401660, NLUpdateProtocol_ParseReplyHeader @ 0x4011d0,
//  NLUpdateProtocol_DispatchReply @ 0x4017a0 (UPDATE.EXE)]
//
// A message is a header of CR LF lines closed by an empty line. A request's first line is its
// verb and arguments, then `ID <n>`. A reply's lines are its verb line and any of `ID <n>`,
// `SIZE <n>`, `RESULT <n>`; SIZE bytes of payload follow the empty line.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::update {

// The gateways UPDATE.EXE overwrites its wizard's URLs with, and the ports it tries on each
// (0x4E4C + 512 * i). [orig: Cupdate2Dlg_StateTick @ 0x408170 (UPDATE.EXE), state START]
inline constexpr std::string_view kRetailGatewayHosts[3] = { "ftp.novaworld.com",
	"ftp4.novaworld.com", "ftp2.novaworld.com" };
inline constexpr uint16_t kGatewayPorts[4] = { 20044, 20556, 21068, 21580 };

// Request ids (the IDB enum UpdateRequestId); n >= 0 is a command row (GET) or a listed
// server (LOADTEST).
inline constexpr int32_t kIdCommandFile = -1;
inline constexpr int32_t kIdServerLogin = -2;
inline constexpr int32_t kIdServerList = -3;
inline constexpr int32_t kIdGatewayLogin = -4;
inline constexpr std::string_view kServerListName = "SERVER.LST";

// A GET reply's name lands in a 32-byte field: at most 31 characters.
// [orig: UpdateReplyHeader::name, NLUpdateProtocol_ParseReplyHeader @ 0x4011d0 (UPDATE.EXE)]
inline constexpr size_t kReplyNameBytes = 32;

enum class Verb { Login, Get, Crc, LoadTest, PerfTest, Close };

// --- Requests (client to server) ---

// `LOGIN <login> <password>`: the client's password is always empty, so the line ends in a
// space. [orig: NLUpdateProtocol_Login @ 0x401b50 (UPDATE.EXE)]
std::string encode_login(std::string_view login, std::string_view password, int32_t id);
// `GET <name> [<name>...]`, one reply per name. [orig: NLUpdateProtocol_Get @ 0x401f80
// (UPDATE.EXE)]
std::string encode_get(const std::vector<std::string> &names, int32_t id);
// `CRC <name>`. [orig: NLUpdateProtocol_Crc @ 0x4022f0 (UPDATE.EXE)]
std::string encode_crc(std::string_view name, int32_t id);
// `LOADTEST`. [orig: NLUpdateProtocol_LoadTest @ 0x402660 (UPDATE.EXE)]
std::string encode_load_test(int32_t id);
// `CLOSE` with no id line; the client then closes its socket. [orig: NLUpdateProtocol_Close
// @ 0x401660 (UPDATE.EXE)]
std::string encode_close();

struct Request {
	Verb verb = Verb::Close;
	std::vector<std::string> args; // the verb line's other tokens
	std::optional<int32_t> id;
};

enum class Frame {
	NeedMore, // no complete message buffered yet
	Complete, // one message decoded; `consumed` bytes of the buffer are its
	Malformed, // a message the peer cannot have meant (or past the header limit)
};

// The largest request header a server buffers before it gives up on the peer.
inline constexpr size_t kMaxRequestHeaderBytes = 4096;

// One request off the front of a server's receive buffer.
Frame decode_request(std::string_view buffer, Request &out, size_t &consumed);

// --- Replies (server to client) ---

// `LOGIN`, `RESULT` < 0 refusing. [orig: Cupdate2Dlg_OnLoginReply @ 0x405c50 (UPDATE.EXE)]
std::vector<uint8_t> encode_login_reply(int32_t id, int8_t result);
// `GET <name>` and the file: the client writes the payload to <its download dir>\<name>, and
// for a row id stamps that row's time on it. `result` < 0 fails the SERVER.LST and command-file
// fetches. [orig: NLUpdateProtocol_OnGetReply @ 0x4013e0, Cupdate2Dlg_OnGetReply @ 0x406ec0
// (UPDATE.EXE)]
std::vector<uint8_t> encode_get_reply(
		std::string_view name, int32_t id, const uint8_t *data, size_t size, int8_t result = 0);
// `CRC <n>`, the command file's CRC as a signed decimal. [orig: Cupdate2Dlg_OnCrcReply
// @ 0x405cd0 (UPDATE.EXE)]
std::vector<uint8_t> encode_crc_reply(uint32_t crc, int32_t id);
// `LOADTEST` with a one-byte payload, the server's load (lower is better).
// [orig: NLUpdateProtocol_OnLoadTestReply @ 0x401500 (UPDATE.EXE)]
std::vector<uint8_t> encode_load_test_reply(int32_t id, uint8_t load);

struct Reply {
	Verb verb = Verb::Close;
	std::string name; // a GET reply's file
	int32_t crc = 0; // a CRC reply's value
	int32_t id = 0;
	int32_t size = 0;
	int8_t result = 0;
	std::vector<uint8_t> payload;
};

// One reply off the front of a client's receive buffer, read as UPDATE.EXE reads it: each line
// classified by its prefix (`LOG`, `GET`, `CRC`, `CLOSE`, `PERFTEST`, `LOADTEST`, `ID`, `SIZE`,
// `RESULT`, case-insensitive), the numbers by atol after the fixed key width. A line it would
// not recognise (where retail breaks into a debugger), a name past 31 characters or a negative
// SIZE is Malformed. NeedMore until the header and the whole payload are buffered.
Frame decode_reply(const uint8_t *data, size_t size, Reply &out, size_t &consumed);

} // namespace opennova::update
