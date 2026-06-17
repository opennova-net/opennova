#include <napi/tlv.h>

#include <cstring>

namespace opennova {

namespace {

constexpr uint8_t TAG_CONTAINER_START = 0x02;
constexpr uint8_t TAG_CONTAINER_END = 0x03;
constexpr uint8_t TAG_FIELD_START = 0x04;
constexpr uint8_t TAG_FIELD_END = 0x05;

constexpr size_t MAX_FIELD_DATA = 0xFFFFu; // LE16 limit

size_t field_size(const NapiField &f) {
	// [0x04][name][0x00][LE16 len][data][0x00][0x05]
	return f.name.size() + f.data.size() + 6u;
}

size_t message_size_impl(const NapiMessage &msg) {
	// [0x02][name][0x00] ... [0x03]
	size_t total = msg.name.size() + 3u;
	for (const auto &f : msg.fields) {
		total += field_size(f);
	}
	for (const auto &c : msg.children) {
		total += message_size_impl(c);
	}
	return total;
}

// Write out the bytes of a name followed by its null terminator.
void write_cstring(uint8_t *&cur, const std::string &s) {
	std::memcpy(cur, s.data(), s.size());
	cur += s.size();
	*cur++ = 0x00;
}

bool encode_field(const NapiField &f, uint8_t *&cur, const uint8_t *end) {
	if (f.data.size() > MAX_FIELD_DATA) {
		return false;
	}
	const size_t need = field_size(f);
	if (static_cast<size_t>(end - cur) < need) {
		return false;
	}
	*cur++ = TAG_FIELD_START;
	write_cstring(cur, f.name);
	const uint16_t dlen = static_cast<uint16_t>(f.data.size());
	*cur++ = static_cast<uint8_t>(dlen & 0xFFu);
	*cur++ = static_cast<uint8_t>((dlen >> 8) & 0xFFu);
	if (!f.data.empty()) {
		std::memcpy(cur, f.data.data(), f.data.size());
		cur += f.data.size();
	}
	*cur++ = 0x00;
	*cur++ = TAG_FIELD_END;
	return true;
}

bool encode_message(const NapiMessage &msg, uint8_t *&cur, const uint8_t *end) {
	const size_t need = message_size_impl(msg);
	if (static_cast<size_t>(end - cur) < need) {
		return false;
	}
	*cur++ = TAG_CONTAINER_START;
	write_cstring(cur, msg.name);
	// The original serializer always emits fields before children, but the
	// parser accepts either order. Match the serializer.
	for (const auto &f : msg.fields) {
		if (!encode_field(f, cur, end)) {
			return false;
		}
	}
	for (const auto &c : msg.children) {
		if (!encode_message(c, cur, end)) {
			return false;
		}
	}
	*cur++ = TAG_CONTAINER_END;
	return true;
}

// Read a null-terminated C-string starting at `cur`. Advances cur past
// the terminator. Returns false on malformed input (no terminator within
// the remaining buffer).
bool read_cstring(const uint8_t *&cur, const uint8_t *end, std::string &out) {
	const uint8_t *p = cur;
	while (p < end && *p != 0x00) {
		++p;
	}
	if (p >= end) {
		return false;
	}
	out.assign(reinterpret_cast<const char *>(cur), static_cast<size_t>(p - cur));
	cur = p + 1; // skip terminator
	return true;
}

bool decode_field(const uint8_t *&cur, const uint8_t *end, NapiField &out) {
	if (cur >= end || *cur != TAG_FIELD_START) {
		return false;
	}
	++cur; // consume 0x04
	if (!read_cstring(cur, end, out.name)) {
		return false;
	}
	if (static_cast<size_t>(end - cur) < 2) {
		return false;
	}
	const uint16_t dlen = static_cast<uint16_t>(cur[0]) |
			static_cast<uint16_t>(static_cast<uint16_t>(cur[1]) << 8);
	cur += 2;
	if (static_cast<size_t>(end - cur) < static_cast<size_t>(dlen) + 2u) {
		return false;
	}
	out.data.assign(cur, cur + dlen);
	cur += dlen;
	// trailing NUL + 0x05 end marker
	if (*cur != 0x00) {
		return false;
	}
	++cur;
	if (*cur != TAG_FIELD_END) {
		return false;
	}
	++cur;
	return true;
}

bool decode_message(const uint8_t *&cur, const uint8_t *end, NapiMessage &out) {
	if (cur >= end || *cur != TAG_CONTAINER_START) {
		return false;
	}
	++cur; // consume 0x02
	if (!read_cstring(cur, end, out.name)) {
		return false;
	}
	while (cur < end) {
		const uint8_t marker = *cur;
		if (marker == TAG_CONTAINER_END) {
			++cur;
			return true;
		}
		if (marker == TAG_CONTAINER_START) {
			NapiMessage child;
			if (!decode_message(cur, end, child)) {
				return false;
			}
			out.children.push_back(std::move(child));
		} else if (marker == TAG_FIELD_START) {
			NapiField field;
			if (!decode_field(cur, end, field)) {
				return false;
			}
			out.fields.push_back(std::move(field));
		} else {
			return false;
		}
	}
	// Ran out of data without seeing 0x03.
	return false;
}

} // namespace

size_t napi_message_size(const NapiMessage &msg) {
	return message_size_impl(msg);
}

int napi_message_encode(const NapiMessage &msg, uint8_t *out, size_t out_cap, size_t *out_size) {
	if (!out || !out_size) {
		return -1;
	}
	uint8_t *cur = out;
	const uint8_t *end = out + out_cap;
	if (!encode_message(msg, cur, end)) {
		return -1;
	}
	*out_size = static_cast<size_t>(cur - out);
	return 0;
}

int napi_message_decode(const uint8_t *data, size_t data_len, NapiMessage &out, size_t *bytes_consumed) {
	if (!data || !bytes_consumed) {
		return -1;
	}
	out = NapiMessage{}; // reset
	const uint8_t *cur = data;
	const uint8_t *end = data + data_len;
	if (!decode_message(cur, end, out)) {
		return -1;
	}
	*bytes_consumed = static_cast<size_t>(cur - data);
	return 0;
}

// ---------------------------------------------------------------------------
// Stream layer

namespace {

constexpr uint8_t TAG_STREAM_END = 0x01;

} // namespace

size_t napi_stream_size(const std::vector<NapiMessage> &messages) {
	size_t total = 1u; // trailing 0x01
	for (const auto &m : messages) {
		total += message_size_impl(m);
	}
	return total;
}

int napi_stream_encode(const std::vector<NapiMessage> &messages,
                       uint8_t *out, size_t out_cap, size_t *out_size) {
	if (!out || !out_size) {
		return -1;
	}
	uint8_t *cur = out;
	const uint8_t *end = out + out_cap;
	for (const auto &m : messages) {
		if (!encode_message(m, cur, end)) {
			return -1;
		}
	}
	if (cur >= end) {
		return -1;
	}
	*cur++ = TAG_STREAM_END;
	*out_size = static_cast<size_t>(cur - out);
	return 0;
}

int napi_stream_decode(const uint8_t *data, size_t data_len,
                       std::vector<NapiMessage> &out, size_t *bytes_consumed) {
	if (!data || !bytes_consumed) {
		return -1;
	}
	out.clear();
	const uint8_t *cur = data;
	const uint8_t *end = data + data_len;
	while (cur < end) {
		const uint8_t marker = *cur;
		if (marker == TAG_STREAM_END) {
			++cur;
			*bytes_consumed = static_cast<size_t>(cur - data);
			return 0;
		}
		if (marker != TAG_CONTAINER_START) {
			return -1;
		}
		NapiMessage msg;
		if (!decode_message(cur, end, msg)) {
			return -1;
		}
		out.push_back(std::move(msg));
	}
	// End-of-buffer without 0x01 terminator is legal — the decoder in
	// sub_5F6810 accepts `v6 <= 0` as a clean stop too.
	*bytes_consumed = static_cast<size_t>(cur - data);
	return 0;
}

} // namespace opennova
