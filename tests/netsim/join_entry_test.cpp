// The server config flag word a joiner reads (inmatch/server_flags.h): the
// named bits match the witnessed BuildFlags literals [orig:
// CNapiServerConfig_BuildFlags @0x4c4dc0], an undiscovered word answers false
// to every read, and the join entry's step — dial, enumerate first, or ask the
// player/spectator question — follows the word and what the player answered.
#include <runtime/inmatch/server_flags.h>

#include <cstdio>
#include <initializer_list>

using namespace opennova::inmatch;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

int main() {
	// The witnessed literals (a fresh retail DM advertises 0x904: team choose +
	// LAN + the non-SP bit).
	CHECK(server_flag::kTeamChoice == 0x4u && server_flag::kServerPassword == 0x8u);
	CHECK(server_flag::kSideBPassword == 0x10u && server_flag::kSideAPassword == 0x20u);
	CHECK(server_flag::kSpectators == 0x2000u && server_flag::kSpectatorPassword == 0x4000u);

	// An undiscovered word reads false everywhere.
	CHECK(!server_flags_known(-1) && !server_allows_team_choice(-1) && !server_has_side_password(-1) &&
			!server_password_required(-1) && !server_allows_spectators(-1) &&
			!server_spectator_password_required(-1));
	CHECK(server_flags_known(0x904) && server_allows_team_choice(0x904) && !server_password_required(0x904));
	CHECK(server_has_side_password(0x10) && server_has_side_password(0x20) && !server_has_side_password(0x8));
	CHECK(server_allows_spectators(0x2000) && !server_spectator_password_required(0x2000));
	CHECK(server_spectator_password_required(0x6000));

	// The entry: nothing known -> enumerate once, then dial.
	JoinEntryFacts facts;
	CHECK(join_entry_step(facts, false) == JoinEntryStep::Preflight);
	CHECK(join_entry_step(facts, true) == JoinEntryStep::Dial);
	// A word with no choice dials at once.
	facts.server_flags = 0x904 & ~0x4;
	CHECK(join_entry_step(facts, false) == JoinEntryStep::Dial);
	// Each choice bit prompts.
	for (const uint32_t bit : { server_flag::kTeamChoice, server_flag::kServerPassword,
				 server_flag::kSideAPassword, server_flag::kSideBPassword, server_flag::kSpectators }) {
		facts.server_flags = static_cast<int32_t>(0x900u | bit);
		CHECK(join_entry_step(facts, false) == JoinEntryStep::Prompt);
		CHECK(join_entry_step(facts, true) == JoinEntryStep::Prompt);
	}
	// An answered role dials once the passwords its word demands are given.
	facts.server_flags = static_cast<int32_t>(server_flag::kServerPassword | server_flag::kSideAPassword);
	facts.role_explicit = true;
	CHECK(join_entry_step(facts, false) == JoinEntryStep::Prompt); // both missing
	facts.server_password_given = true;
	CHECK(join_entry_step(facts, false) == JoinEntryStep::Prompt); // the side password missing
	facts.side_password_given = true;
	CHECK(join_entry_step(facts, false) == JoinEntryStep::Dial);
	// A spectator never owes a side password.
	facts.side_password_given = false;
	facts.spectator = true;
	CHECK(join_entry_step(facts, false) == JoinEntryStep::Dial);
	// An answered role on an undiscovered word (the --lan-join launch flag) dials, never enumerates.
	facts.server_flags = -1;
	facts.server_password_given = false;
	CHECK(join_entry_step(facts, false) == JoinEntryStep::Dial);

	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("join_entry_test OK\n");
	return 0;
}
