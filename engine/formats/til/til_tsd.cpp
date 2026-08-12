#include <til/til_tsd.h>

#include <cstdlib>
#include <cstring>

namespace opennova {

// [orig: the 64-B-stride name table @ 0x8493f0, "TSD_NULL".."TSD_FLESH"]
const char *const til_tsd_surface_names[TIL_TSD_SURFACE_NAME_COUNT] = {
	"TSD_NULL",      "TSD_DIRT",       "TSD_GRASS",   "TSD_SNOW",
	"TSD_CEMENT",    "TSD_SAND",       "TSD_PACKEDDIRT", "TSD_UNDERWATER",
	"TSD_RAILROAD",  "TSD_MUD",        "TSD_ICE",     "TSD_QUICKSAND",
	"TSD_STONE",     "TSD_WOOD",       "TSD_METAL",   "TSD_GLASS",
	"TSD_CLOTH",     "TSD_FOLIAGE",    "TSD_HMETAL",  "TSD_FLESH",
};

namespace {

bool ieq(const char *a, const char *b) {
	while (*a && *b) {
		char ca = *a, cb = *b;
		if (ca >= 'a' && ca <= 'z') ca -= 32;
		if (cb >= 'a' && cb <= 'z') cb -= 32;
		if (ca != cb) return false;
		++a;
		++b;
	}
	return *a == *b;
}

bool ieq_prefix(const char *s, const char *prefix, size_t n) {
	for (size_t i = 0; i < n; ++i) {
		char cs = s[i], cp = prefix[i];
		if (cs == 0) return false;
		if (cs >= 'a' && cs <= 'z') cs -= 32;
		if (cp >= 'a' && cp <= 'z') cp -= 32;
		if (cs != cp) return false;
	}
	return true;
}

// One tokenized line -> the sub_604C00 row apply. [orig: sub_604C00 @ 0x604c00]
void apply_row(const char *key, const char *value, TilSurfaceTable &out) {
	// token 2 vs the name table, case-insensitive; no match -> 0 (retail's
	// unbounded walk is bounded at the 20 real entries here) [orig: @ 0x604c23].
	uint8_t ordinal = 0;
	for (int i = 0; i < TIL_TSD_SURFACE_NAME_COUNT; ++i) {
		if (ieq(value, til_tsd_surface_names[i])) {
			ordinal = static_cast<uint8_t>(i);
			break;
		}
	}
	// "INDEX_" prefix, case-insensitive; the slot is atol of the suffix
	// [orig: @ 0x604c46/@ 0x604c62]. Out-of-range slots are skipped where
	// retail writes out of bounds (reimpl guard).
	if (!ieq_prefix(key, "INDEX_", 6)) return;
	const long slot = std::strtol(key + 6, nullptr, 10);
	if (slot < 0 || slot > 255) return;
	out.map[slot] = ordinal;
}

} // namespace

void til_tsd_parse(const char *text, size_t len, TilSurfaceTable &out) {
	if (text == nullptr) return;
	size_t pos = 0;
	while (pos < len) {
		// CRLF-strict line scan; a tail line with no CRLF loses its final byte
		// to the in-place terminator [orig: @ 0x53d8de-0x53d8ec].
		size_t end = pos;
		bool crlf = false;
		while (end < len) {
			if (text[end] == '\r' && end + 1 < len && text[end + 1] == '\n') {
				crlf = true;
				break;
			}
			++end;
		}
		size_t line_len = end - pos;
		if (!crlf && line_len > 0) --line_len; // the retail --lineEnd clobber
		const char *line = text + pos;
		pos = crlf ? end + 2 : len;

		// Tokenize: leading space/tab skip, 1000-char clamp, space/comma/tab
		// delimiters, '"' toggles quoting, unquoted "//" or ';' cuts the line
		// [orig: Terrain_TokenizeConfigLine @ 0x53cb60].
		size_t i = 0;
		while (i < line_len && (line[i] == ' ' || line[i] == '\t')) ++i;
		char buf[1001];
		size_t n = line_len - i;
		if (n > 1000) n = 1000;
		std::memcpy(buf, line + i, n);
		buf[n] = 0;

		const char *tok[2] = {"", ""};
		int tok_count = 0;
		bool in_quote = false;
		bool expect_new = true;
		for (size_t c = 0; c < n && tok_count < 30; ++c) {
			char ch = buf[c];
			if (!in_quote && ((ch == '/' && buf[c + 1] == '/') || ch == ';')) break;
			if (!in_quote && (ch == ' ' || ch == ',' || ch == '\t')) {
				buf[c] = 0;
				expect_new = true;
			} else if (ch == '"') {
				in_quote = !in_quote;
				expect_new = true;
				buf[c] = 0;
			} else {
				if (expect_new) {
					if (tok_count < 2) tok[tok_count] = &buf[c];
					++tok_count;
				}
				expect_new = false;
			}
		}
		// Empty lines and '/'-leading first tokens skip the callback
		// [orig: @ 0x53d915/@ 0x53d91e].
		if (tok_count == 0 || tok[0][0] == '/') continue;
		apply_row(tok[0], tok[1], out);
	}
}

} // namespace opennova
