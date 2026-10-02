// Pins the strict JSON reader/writer behind the editor's project files (ADR 0046 d6):
// round trips, the deterministic writer, and every strictness rule the reader enforces.
#include <cstdio>
#include <string>

#include <base/io/json.h>

#include "common/test_expect.h"

using namespace opennova::io;

static int test_parse_and_typed_reads() {
	JsonValue v;
	std::string error;
	TEST_EXPECT(json_parse(R"({"a": 1, "b": true, "c": "x", "d": [1, 2.5, null], "e": {"f": -3e2}})",
	                       v, error));
	TEST_EXPECT(v.is_object());
	TEST_EXPECT(v.get_int("a", 0) == 1);
	TEST_EXPECT(v.get_bool("b", false));
	TEST_EXPECT(v.get_string("c", "") == "x");
	TEST_EXPECT(v.get("d")->is_array() && v.get("d")->array.size() == 3);
	TEST_EXPECT(v.get("d")->array[1].number == 2.5);
	TEST_EXPECT(v.get("d")->array[2].is_null());
	TEST_EXPECT(v.get("e")->get_number("f", 0) == -300.0);
	TEST_EXPECT(v.get_int("missing", 7) == 7);
	TEST_EXPECT(v.get_int("c", 7) == 7);       // wrong type reads the fallback
	TEST_EXPECT(v.get_int("d", 7) == 7);
	TEST_EXPECT(v.get("a")->get("x") == nullptr); // not an object
	return 0;
}

static int test_writer_is_deterministic() {
	JsonValue v = JsonValue::make_object();
	v.set("zeta", JsonValue::make_number(1));
	v.set("alpha", JsonValue::make_string("q\"uote\n"));
	JsonValue arr = JsonValue::make_array();
	arr.push(JsonValue::make_bool(false));
	arr.push(JsonValue::make_number(0.5));
	arr.push(JsonValue::make_number(-0.0));
	v.set("list", std::move(arr));
	v.set("empty", JsonValue::make_object());
	v.set("none", JsonValue::make_array());
	const std::string text = json_write(v);
	const std::string expected =
	        "{\n"
	        "  \"alpha\": \"q\\\"uote\\n\",\n"
	        "  \"empty\": {},\n"
	        "  \"list\": [\n"
	        "    false,\n"
	        "    0.5,\n"
	        "    0\n"
	        "  ],\n"
	        "  \"none\": [],\n"
	        "  \"zeta\": 1\n"
	        "}\n";
	TEST_EXPECT(text == expected);
	// set() replaces in place; the writer still sorts.
	v.set("zeta", JsonValue::make_number(2));
	TEST_EXPECT(v.object.size() == 5);
	TEST_EXPECT(v.get_int("zeta", 0) == 2);
	return 0;
}

static int test_round_trip_preserves_values() {
	JsonValue v = JsonValue::make_object();
	v.set("pi", JsonValue::make_number(3.141592653589793));
	v.set("big", JsonValue::make_number(9007199254740991.0));
	v.set("tiny", JsonValue::make_number(1e-7));
	v.set("neg", JsonValue::make_number(-42));
	v.set("utf8", JsonValue::make_string("caf\xC3\xA9 \xF0\x9F\x8E\xAE"));
	v.set("ctrl", JsonValue::make_string(std::string("a\x01" "b\t", 4)));
	const std::string text = json_write(v);
	JsonValue back;
	std::string error;
	TEST_EXPECT(json_parse(text, back, error));
	TEST_EXPECT(back.get_number("pi", 0) == 3.141592653589793);
	TEST_EXPECT(back.get_number("big", 0) == 9007199254740991.0);
	TEST_EXPECT(back.get_number("tiny", 0) == 1e-7);
	TEST_EXPECT(back.get_int("neg", 0) == -42);
	TEST_EXPECT(back.get_string("utf8", "") == "caf\xC3\xA9 \xF0\x9F\x8E\xAE");
	TEST_EXPECT(back.get_string("ctrl", "") == std::string("a\x01" "b\t", 4));
	TEST_EXPECT(json_write(back) == text);
	return 0;
}

static int test_escapes_and_surrogates() {
	JsonValue v;
	std::string error;
	TEST_EXPECT(json_parse(R"(["\u00e9", "\ud83c\udfae", "\/\b\f\r"])", v, error));
	TEST_EXPECT(v.array[0].string == "\xC3\xA9");
	TEST_EXPECT(v.array[1].string == "\xF0\x9F\x8E\xAE");
	TEST_EXPECT(v.array[2].string == "/\b\f\r");
	TEST_EXPECT(json_parse("\xEF\xBB\xBF{}", v, error)); // a UTF-8 BOM is tolerated
	TEST_EXPECT(v.is_object() && v.object.empty());
	return 0;
}

static int expect_rejected(const char *text) {
	JsonValue v;
	std::string error;
	if (json_parse(text, v, error)) {
		std::fprintf(stderr, "accepted invalid JSON: %s\n", text);
		return 1;
	}
	if (error.find("line ") != 0) {
		std::fprintf(stderr, "error lacks a position: %s\n", error.c_str());
		return 1;
	}
	return 0;
}

static int test_strictness() {
	TEST_EXPECT(expect_rejected("") == 0);
	TEST_EXPECT(expect_rejected("{\"a\": 1,}") == 0);      // trailing comma
	TEST_EXPECT(expect_rejected("[1 2]") == 0);
	TEST_EXPECT(expect_rejected("{\"a\": 1} x") == 0);      // trailing characters
	TEST_EXPECT(expect_rejected("// c\n{}") == 0);          // comments
	TEST_EXPECT(expect_rejected("{\"a\": 1, \"a\": 2}") == 0); // duplicate key
	TEST_EXPECT(expect_rejected("{a: 1}") == 0);
	TEST_EXPECT(expect_rejected("[01]") == 0);
	TEST_EXPECT(expect_rejected("[1.]") == 0);
	TEST_EXPECT(expect_rejected("[NaN]") == 0);
	TEST_EXPECT(expect_rejected("[\"tab\there\"]") == 0);   // raw control character
	TEST_EXPECT(expect_rejected("[\"\\x41\"]") == 0);        // invalid escape
	TEST_EXPECT(expect_rejected("[\"\\ud83c\"]") == 0);      // lone high surrogate
	TEST_EXPECT(expect_rejected("[\"\xC3\"]") == 0);         // truncated UTF-8
	TEST_EXPECT(expect_rejected("tru") == 0);
	std::string deep;
	for (int i = 0; i < 300; ++i) deep += "[";
	TEST_EXPECT(expect_rejected(deep.c_str()) == 0);         // nesting cap
	JsonValue v;
	std::string error;
	TEST_EXPECT(!json_parse("{\"a\":", v, error));
	TEST_EXPECT(error == "line 1, column 6: unexpected end of input");
	return 0;
}

int main() {
	int failures = 0;
	failures += test_parse_and_typed_reads();
	failures += test_writer_is_deterministic();
	failures += test_round_trip_preserves_values();
	failures += test_escapes_and_surrogates();
	failures += test_strictness();
	if (failures == 0) std::printf("json: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
