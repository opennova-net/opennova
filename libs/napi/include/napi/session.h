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
//   CNapiNetwork_RandomizeTimeout@0x4a6d50   (1000-9999ms random timeout)

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
constexpr uint32_t SESSION_CONNECT_TIMEOUT_MS = 20000u; // 0x4E20 in CNapiGameSession_ConnectOrHost polling
constexpr uint32_t SESSION_HANDSHAKE_RETRANSMIT_MS = 1300u; // last arg of CNapiSession_SendMessage
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
NapiMessage make_client_host_request(NapiMessage host_setup,
                                     NapiMessage host,
                                     NapiMessage player_list);
NapiMessage make_client_host_update(NapiMessage host, NapiMessage player_list);

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

} // namespace opennova
