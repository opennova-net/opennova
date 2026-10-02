#pragma once

// The player-facing reason for a failed join or a lost session: retail's one
// text builder over the connection's error record, read by the join screen's
// failure legs and the post-mission router alike. The key resolution is pure;
// the text comes from gameerr.bin through the engine's text lookup.
// [orig: CNapiNetwork_GetDisconnectReasonString @0x4c7000]

#include <cstdint>
#include <string>

#include <formats/rtxt/rtxt.h>

namespace opennova::inmatch {

// The connection fields the builder reads: the join failure the CR=0 0x82
// stored, and the latched disconnect record.
// [orig: NapiNPConnection +0x5F0 JFC / +0x5F4 JFP / +0x5F8 JFS (stored by
//  NapiNP_HandleServerJoinResponse @0x629840); the disconnect event at +0x654:
//  +0x65C DC / +0x668 DSTR / +0x6E8 DPC]
//
// JFC is the host's NP-layer join gate family: 3 the session key, 4 the server
// password, 5 an empty player name, 6 joins disabled, 7 the PV2 game-version
// token, 9/10/15 the CU overflow arms, 11 a CU chunk the host could not create,
// 12/14 the validate callback (14 carries its sub-reason in JFP).
// [orig: HandleClientJoin @0x62b750 — 3 HK @0x62bdd5 / 4 PW @0x62be18 / 7 PV2
//  @0x62be40 / 5 empty-NA @0x62be7d / 6 disabled @0x62be8f; 11
//  "NP.C:PCCR:CCH[1]" @0x62c14e; CNapiNetwork_Init @0x4ca4a0 pins proto+364]
struct ConnectionErrorRecord {
	uint32_t connect_error = 0;    // JFC
	uint32_t connect_param = 0;    // JFP: the validate-callback (JFC 14) sub-reason
	std::string connect_text;      // JFS
	uint32_t disconnect_code = 0;  // DC
	std::string disconnect_text;   // DSTR
	uint32_t disconnect_param = 0; // DPC
};

// The gameerr entry a record names. `section` is null when the record holds no
// error at all (both codes zero): retail then builds an empty reason. A named
// entry substitutes `substitution` for every "[[$]]" in its text.
struct DisconnectReasonKey {
	const char *section = nullptr;
	std::string key;
	std::string substitution;
	bool substitutes = false;
};

// The key retail looks up for `conn` (null = no connection object: ERR1).
// JFC 14 reads the game-connect table by JFP, any other JFC the net-connect
// table; with no JFC, DC 2 reads the game-disconnect table by DPC and any other
// DC the net-disconnect table by DC. A code a table lacks falls back to
// "MP Errors" ERR3 / ERR2 / ERR5 / ERR4 respectively.
DisconnectReasonKey disconnect_reason_key(const ConnectionErrorRecord *conn);

// The reason text: the key's entry (override table first, a miss renders
// "??section:key??"), its "[[$]]" substituted. `nw_server_message` is the
// NovaWorld server-message code a ServerStopHosting left behind; when set it is
// appended, after "MP Errors" UNSPECIFIED when the reason is otherwise empty.
// [orig: dword_25E587C @0x4c712c..0x4c7189]
std::string disconnect_reason_string(const ConnectionErrorRecord *conn,
		const rtxt::File *override_table, const rtxt::File *gameerr,
		const std::string &nw_server_message = {});

} // namespace opennova::inmatch
