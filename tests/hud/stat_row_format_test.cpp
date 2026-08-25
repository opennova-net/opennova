// The post-round stat board's row formatting: the unset dash, the time format,
// the gametext key families and the row colouring.
// [orig: populate_stat_results_list @0x562240]

#include <hud/stat_row_format.h>

#include <cstdio>

using namespace opennova::hud;

namespace {

int failures = 0;
#define CHECK(c, m)                                                            \
	do {                                                                       \
		if (!(c)) { std::fprintf(stderr, "FAIL: %s\n", m); ++failures; }       \
	} while (0)

// AN UNSET VALUE IS A DASH, NOT A ZERO. That distinction is the column's whole
// point: a player who has not scored shows "-", one who scored zero shows "0".
void test_unset_is_a_dash_not_zero() {
	CHECK(stat_format_value(0, StatFieldKind::Integer, false) == "-",
			"an absent value is a dash");
	CHECK(stat_format_value(0, StatFieldKind::Integer, true) == "0",
			"a real zero is a zero");
	CHECK(stat_format_value(0, StatFieldKind::Integer, false) !=
					stat_format_value(0, StatFieldKind::Integer, true),
			"absent and zero must NOT render the same");

	// The unset case wins over the field type, so a missing TIME is a dash
	// rather than "0:00".
	CHECK(stat_format_value(0, StatFieldKind::Time, false) == "-",
			"a missing time is a dash, not 0:00");
}

// Time renders m:ss from SECONDS, minutes unpadded and seconds padded.
void test_time_format() {
	CHECK(stat_format_time(0) == "0:00", "zero is 0:00");
	CHECK(stat_format_time(61) == "1:01", "61s is 1:01, not 1:1");
	CHECK(stat_format_time(59) == "0:59", "under a minute keeps the 0");
	CHECK(stat_format_time(600) == "10:00", "minutes are not padded");
	CHECK(stat_format_time(3599) == "59:59", "and roll past an hour as minutes");
	CHECK(stat_format_time(3600) == "60:00", "60 minutes rather than 1:00:00");
	// The seconds pad is what stops 1:1 — check a single-digit second.
	CHECK(stat_format_time(65) == "1:05", "single-digit seconds are padded");
	// A negative never reaches the board; clamped rather than rendered.
	CHECK(stat_format_time(-30) == "0:00", "a negative clamps to zero");
}

void test_integer_and_unknown() {
	CHECK(stat_format_value(42, StatFieldKind::Integer, true) == "42", "plain int");
	CHECK(stat_format_value(-7, StatFieldKind::Integer, true) == "-7",
			"negatives render");
	// An unrecognised field type is VISIBLE rather than dropped or guessed.
	CHECK(stat_format_value(5, StatFieldKind::Other, true) == "??",
			"an unknown field type shows ??");
}

// The board has LARGE and SMALL layouts with separate key families, and every
// key has a '!'-prefixed fallback.
void test_gametext_keys() {
	CHECK(stat_field_key(3, true, false) == "STROVER_STATFIELD03",
			"large key, zero-padded to two");
	CHECK(stat_field_key(3, false, false) == "STROVER_STATFIELDSMALL03",
			"small layout has its own family");
	CHECK(stat_field_key(3, true, true) == "!STROVER_STATFIELD03",
			"the fallback prefixes a bang");
	CHECK(stat_field_key(12, true, false) == "STROVER_STATFIELD12",
			"two digits stay two digits");
	CHECK(stat_field_key(3, true, false) != stat_field_key(3, false, false),
			"large and small must not collide");
	// A field with no table entry is visible rather than blank.
	CHECK(stat_field_unknown_key(9) == "Unk entry 9",
			"an unmapped field shows its id");
}

// Rows colour by team; the local player's row is found by IDENTITY, not
// position, because the board is sorted and the local row can be anywhere.
void test_row_colour_and_local() {
	CHECK(stat_row_color(1) == kStatRowTeam1, "team 1 cyan");
	CHECK(stat_row_color(2) == kStatRowTeam2, "team 2 red");
	CHECK(stat_row_color(0) == kStatRowNeutral, "no team is neutral");
	CHECK(stat_row_color(7) == kStatRowNeutral, "an unknown team is neutral");
	CHECK(kStatRowTeam1 != kStatRowTeam2, "the two teams are distinguishable");

	CHECK(stat_row_is_local(5, 5), "the local row matches by slot");
	CHECK(!stat_row_is_local(4, 5), "and not by adjacency");
}

// A player with no squad shows the same dash as an unset stat.
void test_squad_text() {
	CHECK(stat_squad_text("") == "-", "no squad is a dash");
	CHECK(stat_squad_text("=X=") == "=X=", "a squad renders as itself");
}

} // namespace

int main() {
	test_unset_is_a_dash_not_zero();
	test_time_format();
	test_integer_and_unknown();
	test_gametext_keys();
	test_row_colour_and_local();
	test_squad_text();
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("stat_row_format_test OK\n");
	return 0;
}
