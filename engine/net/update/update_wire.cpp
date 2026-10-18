#include "update_wire.h"

#include <base/io/strutil.h>

#include <cstring>

namespace opennova::update {

namespace {

constexpr std::string_view kCrLf = "\r\n";
constexpr std::string_view kEnd = "\r\n\r\n";

std::string with_id(std::string first_line, int32_t id) {
	return first_line + "\r\nID " + std::to_string(id) + "\r\n\r\n";
}

std::vector<uint8_t> bytes_of(const std::string &header, const uint8_t *data, size_t size) {
	std::vector<uint8_t> out(header.begin(), header.end());
	if (size)
		out.insert(out.end(), data, data + size);
	return out;
}

bool prefix_icase(std::string_view line, std::string_view prefix, size_t length) {
	if (line.size() < length)
		return false;
	return strutil::iequals(line.substr(0, length), prefix.substr(0, length));
}

// atol: leading spaces, an optional sign, digits; saturating at the int32 range.
int32_t atol_like(std::string_view text) {
	size_t i = 0;
	while (i < text.size() && (text[i] == ' ' || text[i] == '\t'))
		++i;
	bool negative = false;
	if (i < text.size() && (text[i] == '-' || text[i] == '+'))
		negative = text[i++] == '-';
	int64_t value = 0;
	for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i) {
		value = value * 10 + (text[i] - '0');
		if (value > 0x80000000ll)
			value = 0x80000000ll;
	}
	if (negative)
		value = -value;
	if (value > 0x7FFFFFFFll)
		value = 0x7FFFFFFFll;
	return static_cast<int32_t>(value);
}

std::string_view rest_after(std::string_view line, size_t skip) {
	return line.size() > skip ? line.substr(skip) : std::string_view();
}

std::vector<std::string> split_spaces(std::string_view text) {
	std::vector<std::string> out;
	size_t i = 0;
	while (i < text.size()) {
		while (i < text.size() && (text[i] == ' ' || text[i] == '\t'))
			++i;
		size_t end = i;
		while (end < text.size() && text[end] != ' ' && text[end] != '\t')
			++end;
		if (end > i)
			out.emplace_back(text.substr(i, end - i));
		i = end;
	}
	return out;
}

std::optional<Verb> request_verb(std::string_view word) {
	if (strutil::iequals(word, "LOGIN"))
		return Verb::Login;
	if (strutil::iequals(word, "GET"))
		return Verb::Get;
	if (strutil::iequals(word, "CRC"))
		return Verb::Crc;
	if (strutil::iequals(word, "LOADTEST"))
		return Verb::LoadTest;
	if (strutil::iequals(word, "PERFTEST"))
		return Verb::PerfTest;
	if (strutil::iequals(word, "CLOSE"))
		return Verb::Close;
	return std::nullopt;
}

} // namespace

std::string encode_login(std::string_view login, std::string_view password, int32_t id) {
	return with_id("LOGIN " + std::string(login) + " " + std::string(password), id);
}

std::string encode_get(const std::vector<std::string> &names, int32_t id) {
	std::string line = "GET";
	for (const std::string &name : names)
		line += " " + name;
	return with_id(line, id);
}

std::string encode_crc(std::string_view name, int32_t id) {
	return with_id("CRC " + std::string(name), id);
}

std::string encode_load_test(int32_t id) {
	return with_id("LOADTEST", id);
}

std::string encode_close() {
	return "CLOSE\r\n\r\n";
}

Frame decode_request(std::string_view buffer, Request &out, size_t &consumed) {
	const size_t end = buffer.find(kEnd);
	if (end == std::string_view::npos)
		return buffer.size() > kMaxRequestHeaderBytes ? Frame::Malformed : Frame::NeedMore;
	if (end + kEnd.size() > kMaxRequestHeaderBytes)
		return Frame::Malformed;
	const std::string_view header = buffer.substr(0, end);
	Request request;
	size_t at = 0;
	bool first = true;
	while (at <= header.size()) {
		const size_t eol = header.find(kCrLf, at);
		const std::string_view line = header.substr(
				at, eol == std::string_view::npos ? std::string_view::npos : eol - at);
		if (first) {
			std::vector<std::string> words = split_spaces(line);
			if (words.empty())
				return Frame::Malformed;
			const std::optional<Verb> verb = request_verb(words[0]);
			if (!verb)
				return Frame::Malformed;
			request.verb = *verb;
			request.args.assign(words.begin() + 1, words.end());
			first = false;
		} else if (prefix_icase(line, "ID", 2)) {
			request.id = atol_like(rest_after(line, 2));
		} else if (!line.empty()) {
			return Frame::Malformed;
		}
		if (eol == std::string_view::npos)
			break;
		at = eol + kCrLf.size();
	}
	out = std::move(request);
	consumed = end + kEnd.size();
	return Frame::Complete;
}

std::vector<uint8_t> encode_login_reply(int32_t id, int8_t result) {
	const std::string header = "LOGIN\r\nID " + std::to_string(id) + "\r\nRESULT " +
			std::to_string(int(result)) + "\r\n\r\n";
	return bytes_of(header, nullptr, 0);
}

std::vector<uint8_t> encode_get_reply(
		std::string_view name, int32_t id, const uint8_t *data, size_t size, int8_t result) {
	std::vector<uint8_t> out = encode_get_reply_header(name, id, size, result);
	if (size)
		out.insert(out.end(), data, data + size);
	return out;
}

std::vector<uint8_t> encode_get_reply_header(
		std::string_view name, int32_t id, uint64_t size, int8_t result) {
	const std::string header = "GET " + std::string(name) + "\r\nID " + std::to_string(id) +
			"\r\nSIZE " + std::to_string(size) + "\r\nRESULT " + std::to_string(int(result)) +
			"\r\n\r\n";
	return bytes_of(header, nullptr, 0);
}

std::vector<uint8_t> encode_crc_reply(uint32_t crc, int32_t id) {
	const std::string header = "CRC " + std::to_string(static_cast<int32_t>(crc)) + "\r\nID " +
			std::to_string(id) + "\r\n\r\n";
	return bytes_of(header, nullptr, 0);
}

std::vector<uint8_t> encode_load_test_reply(int32_t id, uint8_t load) {
	const std::string header = "LOADTEST\r\nID " + std::to_string(id) + "\r\nSIZE 1\r\n\r\n";
	return bytes_of(header, &load, 1);
}

Frame decode_reply(const uint8_t *data, size_t size, Reply &out, size_t &consumed) {
	const std::string_view buffer(reinterpret_cast<const char *>(data), size);
	const size_t end = buffer.find(kEnd);
	if (end == std::string_view::npos)
		return Frame::NeedMore;
	Reply reply;
	bool have_verb = false;
	size_t at = 0;
	// [orig: NLUpdateProtocol_ParseReplyHeader @ 0x4011d0 (UPDATE.EXE): the prefix order and
	//  widths, LOGIN by its first three letters]
	while (at < end + kCrLf.size()) {
		const size_t eol = buffer.find(kCrLf, at);
		const std::string_view line = buffer.substr(at, eol - at);
		if (prefix_icase(line, "LOGIN", 3)) {
			reply.verb = Verb::Login;
			have_verb = true;
		} else if (prefix_icase(line, "GET", 3)) {
			reply.verb = Verb::Get;
			reply.name = std::string(rest_after(line, 4));
			if (reply.name.size() >= kReplyNameBytes)
				return Frame::Malformed;
			have_verb = true;
		} else if (prefix_icase(line, "CRC", 3)) {
			reply.verb = Verb::Crc;
			reply.crc = atol_like(rest_after(line, 4));
			have_verb = true;
		} else if (prefix_icase(line, "CLOSE", 5)) {
			reply.verb = Verb::Close;
			have_verb = true;
		} else if (prefix_icase(line, "PERFTEST", 8)) {
			reply.verb = Verb::PerfTest;
			have_verb = true;
		} else if (prefix_icase(line, "LOADTEST", 8)) {
			reply.verb = Verb::LoadTest;
			have_verb = true;
		} else if (prefix_icase(line, "ID", 2)) {
			reply.id = atol_like(rest_after(line, 3));
		} else if (prefix_icase(line, "SIZE", 4)) {
			reply.size = atol_like(rest_after(line, 5));
		} else if (prefix_icase(line, "RESULT", 6)) {
			reply.result = static_cast<int8_t>(atol_like(rest_after(line, 7)));
		} else {
			return Frame::Malformed;
		}
		at = eol + kCrLf.size();
	}
	if (!have_verb || reply.size < 0)
		return Frame::Malformed;
	const size_t header_length = end + kEnd.size();
	if (size - header_length < static_cast<size_t>(reply.size))
		return Frame::NeedMore;
	reply.payload.assign(data + header_length, data + header_length + reply.size);
	out = std::move(reply);
	consumed = header_length + static_cast<size_t>(out.size);
	return Frame::Complete;
}

} // namespace opennova::update
