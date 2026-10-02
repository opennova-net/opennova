#pragma once

// THE SERVER CONFIG FLAG WORD a joiner reads before dialing — the ServerHello
// P2 TLV the LAN enumerator answers with and the trailing dword of the S2C
// 0x08 block [orig: CNapiServerConfig_BuildFlags @0x4c4dc0] — and the join
// entry's next step off it: retail asks the player/spectator question (with
// its password and team controls) after enumeration whenever the word offers
// a choice, and a target whose word is not known yet is enumerated first.

#include <cstdint>

namespace opennova::inmatch {

namespace server_flag {
inline constexpr uint32_t kTeamChoice = 0x4u;         // the mpattrib TeamChoose bit (a team game)
inline constexpr uint32_t kServerPassword = 0x8u;     // a configured server password
inline constexpr uint32_t kSideBPassword = 0x10u;     // side B (red) password
inline constexpr uint32_t kSideAPassword = 0x20u;     // side A (blue) password
inline constexpr uint32_t kSpectators = 0x2000u;      // spectator slots configured
inline constexpr uint32_t kSpectatorPassword = 0x4000u; // nested under kSpectators
} // namespace server_flag

// The joiner's reads of a discovered word; -1 = not discovered yet, and every
// read of an undiscovered word is false.
inline bool server_flags_known(int32_t flags) { return flags >= 0; }
inline bool server_allows_team_choice(int32_t flags) {
	return flags >= 0 && (static_cast<uint32_t>(flags) & server_flag::kTeamChoice) != 0;
}
inline bool server_has_side_password(int32_t flags) {
	return flags >= 0 &&
			(static_cast<uint32_t>(flags) &
					(server_flag::kSideAPassword | server_flag::kSideBPassword)) != 0;
}
inline bool server_password_required(int32_t flags) {
	return flags >= 0 && (static_cast<uint32_t>(flags) & server_flag::kServerPassword) != 0;
}
inline bool server_allows_spectators(int32_t flags) {
	return flags >= 0 && (static_cast<uint32_t>(flags) & server_flag::kSpectators) != 0;
}
inline bool server_spectator_password_required(int32_t flags) {
	return flags >= 0 && (static_cast<uint32_t>(flags) & server_flag::kSpectatorPassword) != 0;
}

// What the shell knows of a join target before dialing.
struct JoinEntryFacts {
	int32_t server_flags = -1;
	bool role_explicit = false; // the role question answered (the prompt, or a launch flag)
	bool spectator = false; // the answered role
	bool server_password_given = false;
	bool side_password_given = false;
};

enum class JoinEntryStep {
	Dial, // load as a joiner now
	Preflight, // enumerate the endpoint for its flag word first
	Prompt, // ask the player/spectator question
};

// An answered target dials once every password its word demands is given (a
// spectator never owes a side password); an undiscovered word enumerates the
// endpoint first, once (`preflighted`: the enumeration already ran, answered
// or not); a word offering a choice — spectators (and their password), a
// server or side password — prompts; anything else dials. TeamChoose alone
// (a plain team game, 0x904) is not a choice: retail tests the discovered
// word against 0x6038 and joins at once (D-NET-268).
// [orig: UI_EnumerateAndJoinSession @0x56a296 `test dword_25E5898, 6038h` ->
//  state 8 (PRE_JOINSESSIONCHOICE) or state 4 (join); the choice panels
//  UI_ShowPreGameMenuByState @0x568d10 (0x8 password, 0x2000 spectate, 0x30
//  team password)]
inline JoinEntryStep join_entry_step(const JoinEntryFacts &facts, bool preflighted) {
	const int32_t flags = facts.server_flags;
	if (facts.role_explicit && (!server_password_required(flags) || facts.server_password_given) &&
			(facts.spectator || !server_has_side_password(flags) || facts.side_password_given))
		return JoinEntryStep::Dial;
	if (!server_flags_known(flags) && !preflighted) return JoinEntryStep::Preflight;
	if (server_allows_spectators(flags) || server_spectator_password_required(flags) ||
			server_password_required(flags) || server_has_side_password(flags))
		return JoinEntryStep::Prompt;
	return JoinEntryStep::Dial;
}

} // namespace opennova::inmatch
