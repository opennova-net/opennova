#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <napi/tlv.h>

namespace opennova {

// Session-level shapes for the NAPI NovaWorld handshake. Witnessed in
// jodemo.exe:
//   CNapiGameSession_ConnectOrHost@0x4b08e0  (state polling + error map)
//   CNapiGameSession_SendConnected@0x4add40  ("ClientConnected" sender)
//   CNapiGameSession_SendHostRequest@0x4af7d0 ("ClientHostRequest")
//   CNetClient_SendHostUpdate@0x4af8b0       ("ClientHostUpdate")
//   CNapiGameSession_SendPlayRequest@0x4d3920 ("ClientPlayRequest", retail —
//     the prior 0x4af990 citation was Entity_CheckTripleLOS, a wrong anchor)
//   CNapiGameSession_SendStopHosting@0x4ae320 ("ClientStopHosting")
//   CNapiGameSession_SendStopPlaying@0x4ae3b0 ("ClientStopPlaying")
//   CNapiNetwork_RandomizeTimeout@0x4a6d50   (1000-9999ms random timeout;
//     retail Jointops equivalent is @0x4c4d80 — see docs/net §6.2)

// Session state codes as witnessed in `dword_989574` polling loops inside
// CNapiGameSession_ConnectOrHost. The values match the jodemo globals so
// reviewers can cross-reference them to the original state machine.
// State transitions are driven by CNapiGameSession_ProcessConnect (not
// ported here — runtime/polling work belongs to the apps layer).
enum class SessionState : int {
	Idle = 0,              // pre-connect (value is a convenience; jodemo uses other codes here)
	HostStarting = 5,      // local side is bringing up a listening host
	HostEstablished = 6,   // host is up and accepting joiners
	Connecting = 7,        // client is mid-handshake with the host
	Connected = 8,         // client handshake succeeded
};

// Handshake timing constants (witnessed as immediates in the binary).
// [D-NET-23] The connect/host poll loop in [orig: CNapiGameSession_ConnectOrHost @0x4d4f10] times out
// at 0xEA60 = 60000 ms (BOTH the start-playing and start-hosting GetTickCount loops). The 20000 ms
// (0x4E20) value is the SEPARATE periodic-update background timeout [orig: ProcessPeriodicUpdate
// @0x4d4400] — kept below as its own named constant (the old 20000 here mis-cited the connect poll).
constexpr uint32_t SESSION_CONNECT_TIMEOUT_MS = 60000u;          // 0xEA60 — ConnectOrHost poll
constexpr uint32_t SESSION_PERIODIC_UPDATE_TIMEOUT_MS = 20000u;  // 0x4E20 — ProcessPeriodicUpdate
// [D-NET-24] 1300 is a MESSAGE CHUNK SIZE in bytes (the QueueMessage fragment limit), not a ms
// interval — renamed from the misnomer SESSION_HANDSHAKE_RETRANSMIT_MS. [orig: CNapiNPConnection_QueueMessage @0x628640]
constexpr uint32_t SESSION_MESSAGE_CHUNK_BYTES = 1300u;
constexpr uint32_t SESSION_TIMEOUT_RANDOM_MIN_MS = 1000u;  // rand()%0x2328 + 1000 in RandomizeTimeout
constexpr uint32_t SESSION_TIMEOUT_RANDOM_MAX_MS = 9999u;  // 0x2328 == 9000; +1000 -> [1000,9999]

// Gate-server error codes as observed in dword_989588 after a failed
// handshake, and their user-facing NWEC strings (per jodemo's mapping).
enum class NovaWorldError : int {
	Ok = 0,

	// Generic timeout/polling outcomes (set by the polling loop, not by a
	// server response):
	TimeoutPoll = -1,      // loop timed out without reaching state 6/8 -> NWEC02
	UserCancelled = -2,    // ESC pressed during client-side polling -> NWEC03

	// Server-side rejection codes (from dword_989588):
	Banned = 200,          // -> NWEC11 (matches novaworld §3.4)
	Restricted = 203,      // -> NWEC12 (matches novaworld §3.4)
	Reject3000 = 3000,     // -> NWEC04
	Reject3001 = 3001,     // -> NWEC05
	Reject3002 = 3002,     // -> NWEC06
	Reject3003 = 3003,     // -> NWEC07
	Reject3004 = 3004,     // -> NWEC08
	Reject3005 = 3005,     // -> NWEC09
	Reject3006 = 3006,     // -> NWEC10
	Reject1009 = 1009,     // -> NWEC14 (witnessed special-case reject; [D-NET-25] @0x4d4f10)
	UnknownReject = 0x7FFFFFFF, // sentinel for an unrecognized NONZERO reject code -> NWEC13 (the
	                            // dword_B60110 default branch, distinct from the NWEC02 poll timeout)
};

// Map an error to its user-facing NWEC tag, or empty string if unknown.
std::string novaworld_error_tag(NovaWorldError err);

// Map a raw server reject code to its NovaWorldError enum (identity for
// known codes, returns Reject3000 for unknown >= 3000 and TimeoutPoll for
// others to match the binary's default branch).
NovaWorldError novaworld_error_from_code(int code);

// Handshake message builders. These construct the TLV tree that would be
// serialized and sent via CNapiSession_SendMessage. The runtime layer is
// expected to wrap them with the envelope + retransmit bookkeeping.

NapiMessage make_client_connected();
NapiMessage make_client_stop_hosting();
NapiMessage make_client_stop_playing();

// One ClientVar entry inside a ClientVarList: VarFNum (a field number) + VarName
// + VarValue — the wire shape NapiStatement_SerializeVarList @ 0x4d0660 emits
// and our server (lobby_session extract_var_lists) parses.
struct ClientVar {
	int fnum = 0;
	std::string name;
	std::string value;
};

// Build a "ClientVarList" container: a "VarList" field carrying the list name
// ("Cookie"/"PlaySetup"/...) then one "ClientVar" child per entry (VarFNum/
// VarName/VarValue). [orig: NapiStatement_SerializeVarList @ 0x4d0660]
NapiMessage make_client_var_list(const std::string &list_name,
                                 const std::vector<ClientVar> &vars);

// ClientPlayRequest: a top-level "CurrentlyPlaying" field (decimal of the flag)
// FIRST, then the "Cookie" var-list, then the "PlaySetup" var-list, in that
// order — NOT two containers literally named PlaySetup/Cookie.
// [orig: CNapiGameSession_SendPlayRequest @ 0x4d3920]
NapiMessage make_client_play_request(int currently_playing,
                                     const std::vector<ClientVar> &cookie,
                                     const std::vector<ClientVar> &play_setup);

// ClientHostRequest: a top-level "CurrentlyHosting" field (decimal of the flag)
// FIRST, then "VarCheck"="1", then the Cookie, HostSetup, Host, and PlayerList
// var-lists in that order (each a ClientVarList carrying a VarList name + one
// ClientVar child per entry) — NOT containers literally named HostSetup/Host.
// [orig: CNapiGameSession_SendHostRequest @ 0x4d3700 — two NapiStatementParam_Create
// (CurrentlyHosting, VarCheck) then four NapiStatement_SerializeVarList @ 0x4d0660
// for "Cookie"(this+388) / "HostSetup"(+460) / "Host"(+532) / "PlayerList"(+604)]
NapiMessage make_client_host_request(int currently_hosting,
                                     const std::vector<ClientVar> &cookie,
                                     const std::vector<ClientVar> &host_setup,
                                     const std::vector<ClientVar> &host,
                                     const std::vector<ClientVar> &player_list);

// ClientHostUpdate: the Host and PlayerList var-lists only — no params, no
// Cookie/HostSetup (the heartbeat refresh). [orig: CNapiGameSession_SendHostUpdate
// @ 0x4d3860 — NapiStatement_SerializeVarList for "Host"(this+532) / "PlayerList"(+604)]
NapiMessage make_client_host_update(const std::vector<ClientVar> &host,
                                    const std::vector<ClientVar> &player_list);

} // namespace opennova
