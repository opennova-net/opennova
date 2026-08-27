// The tileset .TSD surface-definition parse (D-SND-15): CRLF-strict lines,
// the retail tokenizer's delimiters/comments, INDEX_ rows into the 256-entry
// table, case-insensitive TSD-name resolution, unknown names -> 0.
// [orig: File_ParseASCIIFile @ 0x53d810; Terrain_TokenizeConfigLine
// @ 0x53cb60; Terrain_ParseTsdRow @ 0x604c00]
#include <formats/til/til_tsd.h>

#include <cstdio>
#include <cstring>

namespace {

int failures = 0;

void expect(bool condition, const char *message) {
	if (!condition) {
		std::fprintf(stderr, "FAIL: %s\n", message);
		++failures;
	}
}

opennova::TilSurfaceTable parse(const char *text) {
	opennova::TilSurfaceTable t;
	opennova::til_tsd_parse(text, std::strlen(text), t);
	return t;
}

} // namespace

int main() {
	// The name table pins the witnessed ordinals (charmap-shared domain).
	expect(std::strcmp(opennova::til_tsd_surface_names[0], "TSD_NULL") == 0 &&
	               std::strcmp(opennova::til_tsd_surface_names[3], "TSD_SNOW") == 0 &&
	               std::strcmp(opennova::til_tsd_surface_names[7], "TSD_UNDERWATER") == 0 &&
	               std::strcmp(opennova::til_tsd_surface_names[19], "TSD_FLESH") == 0,
	       "name-table ordinals 0/3/7/19");

	// Ordinary rows: whitespace/comma tokens, case-insensitive key + name.
	{
		const opennova::TilSurfaceTable t = parse(
				"INDEX_0 TSD_CEMENT\r\n"
				"index_5\ttsd_snow\r\n"
				"INDEX_12,TSD_RAILROAD\r\n");
		expect(t.map[0] == 4, "INDEX_0 -> TSD_CEMENT (4)");
		expect(t.map[5] == 3, "lower-case key/name -> TSD_SNOW (3)");
		expect(t.map[12] == 8, "comma-delimited row -> TSD_RAILROAD (8)");
		expect(t.map[1] == 0, "untouched slots stay 0 (the memset default)");
	}

	// An unknown name still WRITES its slot as 0, a missing value token reads
	// as the empty string (retail's pre-seeded empty tokens), and non-INDEX_
	// keys are ignored.
	{
		opennova::TilSurfaceTable t;
		t.map[7] = 9;
		t.map[8] = 9;
		const char text[] =
				"INDEX_7 TSD_NOTATHING\r\n"
				"INDEX_8\r\n"
				"tileinfo TSD_SNOW\r\n";
		opennova::til_tsd_parse(text, sizeof(text) - 1, t);
		expect(t.map[7] == 0, "unknown name overwrites with 0");
		expect(t.map[8] == 0, "missing value token overwrites with 0");
	}

	// Comments: an unquoted ';' or '//' cuts the line; a line whose first
	// token starts with '/' never reaches the row apply.
	{
		const opennova::TilSurfaceTable t = parse(
				"; a full-line comment\r\n"
				"// another\r\n"
				"INDEX_3 TSD_MUD ; trailing comment\r\n"
				"INDEX_4 // cut before the value\r\n");
		expect(t.map[3] == 9, "trailing ';' comment leaves the row intact");
		expect(t.map[4] == 0, "a '//' cut before the value writes 0");
	}

	// The CRLF-strict tail clobber: a final line with no CRLF loses its last
	// byte to the in-place terminator, so TSD_SNOW reads TSD_SNO -> no match.
	{
		const opennova::TilSurfaceTable t = parse("INDEX_9 TSD_SNOW");
		expect(t.map[9] == 0, "tail line without CRLF loses its final byte");
	}
	{
		const opennova::TilSurfaceTable t = parse("INDEX_9 TSD_SNOW\r\n");
		expect(t.map[9] == 3, "CRLF-terminated tail parses whole");
	}

	// Reimpl guards: out-of-range slots are skipped (retail writes OOB).
	{
		const opennova::TilSurfaceTable t = parse(
				"INDEX_256 TSD_SNOW\r\n"
				"INDEX_-1 TSD_SNOW\r\n"
				"INDEX_255 TSD_ICE\r\n");
		expect(t.map[255] == 10, "slot 255 in range -> TSD_ICE (10)");
		for (int i = 0; i < 255; ++i)
			expect(t.map[i] == 0, "OOB rows write nothing");
	}

	if (failures == 0) std::printf("til_tsd_test OK\n");
	return failures == 0 ? 0 : 1;
}
