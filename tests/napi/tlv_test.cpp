#include <napi/tlv.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

bool expect(bool condition, const char *message) {
	if (condition) {
		return true;
	}
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

bool messages_equal(const opennova::NapiMessage &a, const opennova::NapiMessage &b) {
	if (a.name != b.name) return false;
	if (a.fields.size() != b.fields.size()) return false;
	for (size_t i = 0; i < a.fields.size(); ++i) {
		if (a.fields[i].name != b.fields[i].name) return false;
		if (a.fields[i].data != b.fields[i].data) return false;
	}
	if (a.children.size() != b.children.size()) return false;
	for (size_t i = 0; i < a.children.size(); ++i) {
		if (!messages_equal(a.children[i], b.children[i])) return false;
	}
	return true;
}

// size() matches the byte-exact overheads:
//   container: name + 3
//   field:     name + data + 6
bool check_size_math() {
	opennova::NapiMessage m;
	m.name = "Hello";
	if (!expect(opennova::napi_message_size(m) == 5u + 3u, "empty container size == name + 3")) return false;

	opennova::NapiField f;
	f.name = "Key";
	f.data = {0x01, 0x02, 0x03};
	m.fields.push_back(f);
	// 5+3 + (3+3+6) = 8 + 12 = 20
	if (!expect(opennova::napi_message_size(m) == 20u, "container with one 3-byte field")) return false;

	opennova::NapiMessage child;
	child.name = "Inner";
	m.children.push_back(child);
	// + (5+3) = +8 => 28
	if (!expect(opennova::napi_message_size(m) == 28u, "container with field + empty child")) return false;
	return true;
}

bool roundtrip(const opennova::NapiMessage &msg) {
	std::vector<uint8_t> buf(opennova::napi_message_size(msg) + 16, 0);
	size_t enc_size = 0;
	if (!expect(opennova::napi_message_encode(msg, buf.data(), buf.size(), &enc_size) == 0,
			"encode")) return false;
	if (!expect(enc_size == opennova::napi_message_size(msg), "encoded size matches predicted size")) return false;
	opennova::NapiMessage decoded;
	size_t cons = 0;
	if (!expect(opennova::napi_message_decode(buf.data(), enc_size, decoded, &cons) == 0,
			"decode")) return false;
	if (!expect(cons == enc_size, "bytes_consumed matches encoded size")) return false;
	if (!expect(messages_equal(decoded, msg), "decoded tree equals original")) return false;
	return true;
}

bool check_empty_container() {
	opennova::NapiMessage m;
	m.name = "Empty";
	return roundtrip(m);
}

bool check_fields_only() {
	opennova::NapiMessage m;
	m.name = "FieldsOnly";
	m.fields.push_back({"A", {0x11}});
	m.fields.push_back({"B", {0x22, 0x33}});
	m.fields.push_back({"C", {}}); // zero-length data field
	return roundtrip(m);
}

bool check_nested_containers() {
	opennova::NapiMessage root;
	root.name = "Root";
	opennova::NapiMessage c1;
	c1.name = "Child1";
	c1.fields.push_back({"X", {0xDE, 0xAD}});
	opennova::NapiMessage c2;
	c2.name = "Child2";
	opennova::NapiMessage grandchild;
	grandchild.name = "GrandChild";
	grandchild.fields.push_back({"deep", {0xBE, 0xEF}});
	c2.children.push_back(grandchild);
	root.children.push_back(c1);
	root.children.push_back(c2);
	return roundtrip(root);
}

// Concrete byte-layout check: encoded container [0x02] "n"[0x00] [0x03]
// should be exactly 5 bytes.
bool check_byte_layout_minimal() {
	opennova::NapiMessage m;
	m.name = "n";
	std::vector<uint8_t> buf(16, 0);
	size_t enc_size = 0;
	opennova::napi_message_encode(m, buf.data(), buf.size(), &enc_size);
	const uint8_t expected[] = {0x02, 'n', 0x00, 0x03};
	if (!expect(enc_size == 4u, "minimal container encodes to 4 bytes")) return false;
	for (size_t i = 0; i < 4; ++i) {
		if (!expect(buf[i] == expected[i], "minimal container exact byte layout")) return false;
	}
	return true;
}

// Concrete byte-layout check: field with 1-byte data.
// Container with name "c" holding field "k" with data {0xAB} expands to:
//   02 'c' 00 04 'k' 00 01 00 AB 00 05 03
bool check_byte_layout_field() {
	opennova::NapiMessage m;
	m.name = "c";
	m.fields.push_back({"k", {0xAB}});
	std::vector<uint8_t> buf(32, 0);
	size_t enc_size = 0;
	opennova::napi_message_encode(m, buf.data(), buf.size(), &enc_size);
	const uint8_t expected[] = {
		0x02, 'c', 0x00,
		0x04, 'k', 0x00, 0x01, 0x00, 0xAB, 0x00, 0x05,
		0x03,
	};
	if (!expect(enc_size == sizeof(expected), "field container expected 12 bytes")) return false;
	for (size_t i = 0; i < sizeof(expected); ++i) {
		if (!expect(buf[i] == expected[i], "field container exact byte layout")) return false;
	}
	return true;
}

// Parser must accept interleaved fields + children (the serializer happens
// to emit fields-first, but the parser peeks the marker byte).
bool check_interleaved_parse() {
	// Hand-built byte stream with field + child + field:
	//   02 'R' 00                          (start container "R")
	//     04 'a' 00 01 00 11 00 05         (field a = {0x11})
	//     02 'X' 00 03                      (empty child "X")
	//     04 'b' 00 01 00 22 00 05         (field b = {0x22})
	//   03                                  (end container)
	const uint8_t wire[] = {
		0x02, 'R', 0x00,
		0x04, 'a', 0x00, 0x01, 0x00, 0x11, 0x00, 0x05,
		0x02, 'X', 0x00, 0x03,
		0x04, 'b', 0x00, 0x01, 0x00, 0x22, 0x00, 0x05,
		0x03,
	};
	opennova::NapiMessage m;
	size_t cons = 0;
	if (!expect(opennova::napi_message_decode(wire, sizeof(wire), m, &cons) == 0,
			"interleaved stream decodes")) return false;
	if (!expect(cons == sizeof(wire), "whole stream consumed")) return false;
	if (!expect(m.name == "R", "root name")) return false;
	if (!expect(m.fields.size() == 2, "two fields")) return false;
	if (!expect(m.children.size() == 1, "one child")) return false;
	if (!expect(m.fields[0].name == "a" && m.fields[0].data == std::vector<uint8_t>{0x11}, "first field preserved")) return false;
	if (!expect(m.fields[1].name == "b" && m.fields[1].data == std::vector<uint8_t>{0x22}, "second field preserved")) return false;
	if (!expect(m.children[0].name == "X", "child preserved")) return false;
	return true;
}

// Multi-message stream roundtrip + trailing 0x01 terminator.
// Witnessed in CBufferList_Serialize@0x5f69e0 / sub_5F6810 (stream decoder).
bool check_stream_roundtrip_and_terminator() {
	std::vector<opennova::NapiMessage> msgs;
	opennova::NapiMessage a;
	a.name = "First";
	a.fields.push_back({"alpha", {0xAA, 0xBB}});
	msgs.push_back(a);
	opennova::NapiMessage b;
	b.name = "Second";
	msgs.push_back(b);
	std::vector<uint8_t> buf(opennova::napi_stream_size(msgs) + 8, 0);
	size_t enc_size = 0;
	if (!expect(opennova::napi_stream_encode(msgs, buf.data(), buf.size(), &enc_size) == 0,
			"stream encode")) return false;
	if (!expect(buf[enc_size - 1] == 0x01,
			"stream encode writes trailing 0x01 terminator")) return false;
	std::vector<opennova::NapiMessage> decoded;
	size_t cons = 0;
	if (!expect(opennova::napi_stream_decode(buf.data(), enc_size, decoded, &cons) == 0,
			"stream decode")) return false;
	if (!expect(cons == enc_size, "whole stream consumed including terminator")) return false;
	if (!expect(decoded.size() == msgs.size(), "message count preserved")) return false;
	for (size_t i = 0; i < msgs.size(); ++i) {
		if (!expect(messages_equal(decoded[i], msgs[i]), "stream roundtrip preserves message content")) return false;
	}
	return true;
}

// Empty stream: just a single 0x01.
bool check_empty_stream() {
	std::vector<opennova::NapiMessage> msgs;
	std::vector<uint8_t> buf(4, 0);
	size_t enc_size = 0;
	if (!expect(opennova::napi_stream_encode(msgs, buf.data(), buf.size(), &enc_size) == 0,
			"empty stream encodes to single terminator")) return false;
	if (!expect(enc_size == 1u && buf[0] == 0x01, "empty stream is exactly {0x01}")) return false;
	std::vector<opennova::NapiMessage> decoded;
	size_t cons = 0;
	if (!expect(opennova::napi_stream_decode(buf.data(), enc_size, decoded, &cons) == 0,
			"empty stream decodes")) return false;
	if (!expect(decoded.empty() && cons == 1, "empty stream decodes to zero messages")) return false;
	return true;
}

// Malformed input cases.
bool check_malformed_rejected() {
	opennova::NapiMessage m;
	size_t cons = 0;
	// Missing container end
	{
		const uint8_t wire[] = {0x02, 'x', 0x00};
		if (!expect(opennova::napi_message_decode(wire, sizeof(wire), m, &cons) != 0, "missing 0x03 rejected")) return false;
	}
	// Bad start marker
	{
		const uint8_t wire[] = {0xFF, 'x', 0x00, 0x03};
		if (!expect(opennova::napi_message_decode(wire, sizeof(wire), m, &cons) != 0, "non-0x02 start rejected")) return false;
	}
	// Field missing trailing NUL before 0x05 (common wrong-impl trap)
	{
		const uint8_t wire[] = {
			0x02, 'c', 0x00,
			0x04, 'k', 0x00, 0x01, 0x00, 0xAA, 0x05,  // MISSING the 0x00 between data and 0x05
			0x03,
		};
		if (!expect(opennova::napi_message_decode(wire, sizeof(wire), m, &cons) != 0,
				"field missing trailing NUL rejected")) return false;
	}
	return true;
}

} // namespace

int main() {
	if (!check_size_math()) return 1;
	if (!check_empty_container()) return 1;
	if (!check_fields_only()) return 1;
	if (!check_nested_containers()) return 1;
	if (!check_byte_layout_minimal()) return 1;
	if (!check_byte_layout_field()) return 1;
	if (!check_interleaved_parse()) return 1;
	if (!check_stream_roundtrip_and_terminator()) return 1;
	if (!check_empty_stream()) return 1;
	if (!check_malformed_rejected()) return 1;
	std::printf("OK: TLV container 0x02/0x03 + field 0x04/NUL/0x05 byte-exact\n");
	return 0;
}
