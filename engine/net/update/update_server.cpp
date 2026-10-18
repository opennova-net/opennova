#include "update_server.h"

#include "update_wire.h"

#include <base/io/os_path.h>
#include <base/io/strutil.h>
#include <formats/update/server_list.h>
#include <formats/update/update_crc.h>

#include <algorithm>
#include <cstdio>

namespace opennova::update {

namespace {

class BytesFile final : public UpdateFile {
public:
	explicit BytesFile(std::vector<uint8_t> bytes) : bytes_(std::move(bytes)) {}
	uint64_t size() const override { return bytes_.size(); }
	bool read(uint8_t *out, size_t count) override {
		if (count > bytes_.size() - at_)
			return false;
		std::copy_n(bytes_.data() + at_, count, out);
		at_ += count;
		return true;
	}

private:
	std::vector<uint8_t> bytes_;
	size_t at_ = 0;
};

class DiskFile final : public UpdateFile {
public:
	DiskFile(std::FILE *file, uint64_t size) : file_(file), size_(size) {}
	~DiskFile() override { std::fclose(file_); }
	DiskFile(const DiskFile &) = delete;
	DiskFile &operator=(const DiskFile &) = delete;
	uint64_t size() const override { return size_; }
	bool read(uint8_t *out, size_t count) override {
		return std::fread(out, 1, count, file_) == count;
	}

private:
	std::FILE *file_;
	uint64_t size_;
};

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

std::unique_ptr<UpdateFile> UpdateFile::from_bytes(std::vector<uint8_t> bytes) {
	return std::make_unique<BytesFile>(std::move(bytes));
}

std::unique_ptr<UpdateFile> UpdateFile::from_disk(const std::string &path) {
	std::FILE *file = io::fopen_utf8(path.c_str(), "rb");
	if (file == nullptr)
		return nullptr;
	long end = -1;
	if (std::fseek(file, 0, SEEK_END) == 0)
		end = std::ftell(file);
	if (end < 0 || std::fseek(file, 0, SEEK_SET) != 0) {
		std::fclose(file);
		return nullptr;
	}
	return std::make_unique<DiskFile>(file, static_cast<uint64_t>(end));
}

std::unique_ptr<UpdateFile> UpdateCatalog::open_file(
		std::string_view login, std::string_view name) const {
	std::vector<uint8_t> bytes;
	if (!read_file(login, name, bytes))
		return nullptr;
	return UpdateFile::from_bytes(std::move(bytes));
}

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
	if (size > 0)
		conn.buffer.append(reinterpret_cast<const char *>(data), size);
	for (;;) {
		if (out.send.size() >= kSendBudget) {
			out.more = conn.body != nullptr || conn.next_name < conn.get_names.size() ||
					!conn.buffer.empty();
			return out;
		}
		// The body going out: as much as the budget leaves.
		if (conn.body) {
			const size_t at = out.send.size();
			const size_t count =
					static_cast<size_t>(std::min<uint64_t>(conn.body_left, kSendBudget - at));
			out.send.resize(at + count);
			if (!conn.body->read(out.send.data() + at, count)) {
				// The file stopped reading partway: its reply cannot complete.
				out.send.resize(at);
				conn.body.reset();
				out.keep_open = false;
				return out;
			}
			conn.body_left -= count;
			if (conn.body_left == 0)
				conn.body.reset();
			continue;
		}
		// The GET's next name.
		if (conn.next_name < conn.get_names.size()) {
			const std::string name = conn.get_names[conn.next_name++];
			if (conn.next_name == conn.get_names.size()) {
				conn.get_names.clear();
				conn.next_name = 0;
			}
			if (!plain_name(name)) {
				out.keep_open = false;
				return out;
			}
			std::unique_ptr<UpdateFile> file;
			if (strutil::iequals(name, kServerListName)) {
				const std::string list = write_server_list(catalog_.server_hosts(conn.login));
				if (!list.empty())
					file = UpdateFile::from_bytes(std::vector<uint8_t>(list.begin(), list.end()));
			} else {
				file = catalog_.open_file(conn.login, name);
			}
			if (!file) {
				append(out.send, encode_get_reply(name, conn.get_id, nullptr, 0, -1));
				continue;
			}
			append(out.send, encode_get_reply_header(name, conn.get_id, file->size()));
			conn.body_left = file->size();
			if (conn.body_left > 0)
				conn.body = std::move(file);
			continue;
		}
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
				// Each name answered in turn at the loop's top, a file's body as the budget
				// allows.
				conn.get_id = id;
				conn.get_names = std::move(request.args);
				conn.next_name = 0;
				break;
			case Verb::Crc: {
				if (conn.login.empty()) {
					out.keep_open = false;
					return out;
				}
				// The command file's CRC; any other name answers 0. A client whose own command
				// file is missing or empty computes 0 too (CmdCrc_ComputeFile, update_crc.cpp), so
				// it takes a 0 answer as a match and goes on to fetch the files, where it fails
				// with error 14.
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
