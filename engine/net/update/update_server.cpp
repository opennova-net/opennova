#include "update_server.h"

#include "update_wire.h"

#include <base/io/strutil.h>
#include <formats/update/server_list.h>
#include <formats/update/update_crc.h>

namespace opennova::update {

namespace {

void append(std::vector<uint8_t> &out, const std::vector<uint8_t> &bytes) {
	out.insert(out.end(), bytes.begin(), bytes.end());
}

// A name the client may write into its download directory: a plain file name that fits the
// reply's 32-byte field. [orig: NLUpdateProtocol_OnGetReply @ 0x4013e0 (UPDATE.EXE)]
bool plain_name(std::string_view name) {
	if (name.empty() || name.size() >= kReplyNameBytes || name == "." || name == "..")
		return false;
	return name.find_first_of("\\/:") == std::string_view::npos;
}

} // namespace

uint32_t UpdateServer::open() {
	const uint32_t id = next_++;
	connections_[id] = Connection{};
	return id;
}

void UpdateServer::close(uint32_t connection) {
	connections_.erase(connection);
}

UpdateServer::Received UpdateServer::receive(
		uint32_t connection, const uint8_t *data, size_t size) {
	Received out;
	auto it = connections_.find(connection);
	if (it == connections_.end()) {
		out.keep_open = false;
		return out;
	}
	Connection &conn = it->second;
	conn.buffer.append(reinterpret_cast<const char *>(data), size);
	for (;;) {
		Request request;
		size_t consumed = 0;
		const Frame frame = decode_request(conn.buffer, request, consumed);
		if (frame == Frame::NeedMore)
			break;
		if (frame == Frame::Malformed) {
			out.keep_open = false;
			break;
		}
		conn.buffer.erase(0, consumed);
		const int32_t id = request.id.value_or(0);
		switch (request.verb) {
			case Verb::Login: {
				// [orig: Cupdate2Dlg_OnLoginReply @ 0x405c50 (UPDATE.EXE): RESULT < 0 fails the
				//  login, the gateway's by moving to the next host]
				const std::string login = request.args.empty() ? std::string() : request.args[0];
				const bool known = !login.empty() && catalog_.has_product(login);
				conn.login = known ? login : std::string();
				append(out.send, encode_login_reply(id, known ? 0 : -1));
				break;
			}
			case Verb::Get:
				if (conn.login.empty()) {
					out.keep_open = false;
					return out;
				}
				for (const std::string &name : request.args) {
					if (!plain_name(name)) {
						out.keep_open = false;
						return out;
					}
					std::vector<uint8_t> file;
					bool found = false;
					if (strutil::iequals(name, kServerListName)) {
						const std::string list =
								write_server_list(catalog_.server_hosts(conn.login));
						file.assign(list.begin(), list.end());
						found = !list.empty();
					} else {
						found = catalog_.read_file(conn.login, name, file);
					}
					append(out.send,
							found ? encode_get_reply(name, id, file.data(), file.size())
								  : encode_get_reply(name, id, nullptr, 0, -1));
				}
				break;
			case Verb::Crc: {
				if (conn.login.empty()) {
					out.keep_open = false;
					return out;
				}
				// The command file's CRC; any other name answers 0, which never matches a file.
				// [orig: Cupdate2Dlg_OnCrcReply @ 0x405cd0 (UPDATE.EXE)]
				uint32_t crc = 0;
				const std::string name = request.args.empty() ? std::string() : request.args[0];
				std::vector<uint8_t> file;
				if (strutil::iequals(name, catalog_.command_file_name(conn.login)) &&
						catalog_.read_file(conn.login, name, file))
					crc = command_file_crc(file.data(), file.size());
				append(out.send, encode_crc_reply(crc, id));
				break;
			}
			case Verb::LoadTest:
				append(out.send, encode_load_test_reply(id, 0));
				break;
			case Verb::PerfTest: {
				const std::string reply = "PERFTEST\r\nID " + std::to_string(id) + "\r\n\r\n";
				out.send.insert(out.send.end(), reply.begin(), reply.end());
				break;
			}
			case Verb::Close:
				// The client closes its socket after CLOSE and reads no reply.
				// [orig: NLUpdateProtocol_Close @ 0x401660 (UPDATE.EXE)]
				out.keep_open = false;
				return out;
		}
	}
	return out;
}

} // namespace opennova::update
