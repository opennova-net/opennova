#pragma once

#include <net/npwire/session_hello.h> // DisconnectEvent

#include <cstdint>

namespace opennova::inmatch {

// g_MissionExitReason: why the mission loop ended. The values the post-mission router tells
// apart are named here; 9..19 are the server's disconnect classes the in-match connection maps
// its description record onto.
inline constexpr int32_t kMissionExitNone = 0;
// ESC, the epilog timeout, a load abort, and the description-record punts the client turns into
// its own disconnect [orig: Input_HandleActionBinding case 3 @0x49af26].
inline constexpr int32_t kMissionExitQuit = 1;
// The session reset [orig: Input_HandleActionBinding case 4 @0x49af4c].
inline constexpr int32_t kMissionExitReset = 2;
// A host's round linger expiring into the map cycle [orig: Server_TickUpdate @0x51db63].
inline constexpr int32_t kMissionExitMapCycle = 3;
// A joiner's round linger expiring [orig: Client_ProcessNetworkFrame @0x42c3d3].
inline constexpr int32_t kMissionExitRoundOver = 4;
inline constexpr int32_t kMissionExitCdTrouble = 5;  // [orig: Input_HandleActionBinding case 36 @0x49b780]
inline constexpr int32_t kMissionExitSystem = 6;
inline constexpr int32_t kMissionExitPirate = 7;
inline constexpr int32_t kMissionExitDisconnectFirst = 9;
// The NovaWorld session's end: the main frame's check, a ServerStopPlaying and the service's
// punt all store it [orig: Game_ProcessMainFrame @0x52657c,
// CNapiGameSession_HandleServerDisconnectMsg @0x4d208f,
// CNapiGameSession_HandlePuntNotification @0x4d2216].
inline constexpr int32_t kMissionExitNovaWorld = 12;
inline constexpr int32_t kMissionExitDisconnectLast = 19;
// The host's mission file failed its header check [orig: Game_StartMission @0x524785].
inline constexpr int32_t kMissionExitBadMission = 20;

// The exit reason an in-match disconnect stores: a description-family record (class 2) maps its
// reason code (DPC) through the client's switch; any other class leaves the reason alone.
// [orig: CNapiNetwork_OnDisconnectedFromServer @0x4c63d0 — the `dc == 2` gate @0x4c6563, the
//  DPC switch @0x4c6583..0x4c677c: 1 / 33 / an unlisted code queue input event 3 (reason 1),
//  34 queues event 36 (reason 5), 35 -> 7, 36..45 -> 9..18, 46 and 49 -> 19, 0 nothing]
inline int32_t mission_exit_reason_for_disconnect(const DisconnectEvent &event) {
	if (event.dc != 2 || event.dpc == 0) return kMissionExitNone;
	switch (event.dpc) {
	case 34: return kMissionExitCdTrouble;
	case 35: return kMissionExitPirate;
	case 46:
	case 49: return kMissionExitDisconnectLast;
	default: break;
	}
	if (event.dpc >= 36 && event.dpc <= 45) {
		return kMissionExitDisconnectFirst + static_cast<int32_t>(event.dpc - 36);
	}
	return kMissionExitQuit;
}

// What the post-mission router does with an exit: whether the session's network type (and with
// it a NovaWorld session) survives for the menu, and the error text the menu shows first.
enum class PostMissionError : uint8_t {
	None,
	CdTrouble,        // gameerr "Generic Error Strings" STRE_CDTROUBLE
	System,           // STRE_SYSTEM
	Pirate,           // STRE_PIRATE
	BadMission,       // STRE_BADMISSION
	DisconnectReason, // the in-match connection's disconnect reason text
};

struct PostMissionRoute {
	bool keep_session = false;
	PostMissionError error = PostMissionError::None;
};

// The gameerr.bin key an error text resolves through (empty for None / DisconnectReason).
inline const char *post_mission_error_key(PostMissionError error) {
	switch (error) {
	case PostMissionError::CdTrouble: return "STRE_CDTROUBLE";
	case PostMissionError::System: return "STRE_SYSTEM";
	case PostMissionError::Pirate: return "STRE_PIRATE";
	case PostMissionError::BadMission: return "STRE_BADMISSION";
	default: return "";
	}
}

// The "Post Menu" scene's router. Reason 2 resets the session; 5, 6, 7 and 20 store their
// gameerr text and 9..19 the disconnect reason text, each clearing the network type (which
// resets a NovaWorld session); every other reason keeps a NovaWorld network type and its
// session for the NovaWorld menu. (An in-session authority with a next rotation entry loads it
// instead -- the rotation is not modeled.)
// [orig: PostMenu_RouteMissionExit @0x568460 — reason 2 @0x568465..0x5684a7; 5/6/7
//  @0x5684b1..0x568526; 9..19 @0x56852b..0x568588 -> CNapiNetwork_GetDisconnectReasonString
//  @0x5686d0; 20 @0x56858e..0x5685bd; the rest -> CNapiGameSession_FullDestroy @0x568683 and
//  the `transport_mode == 1` keep @0x568688]
inline PostMissionRoute route_mission_exit(int32_t reason, bool novaworld) {
	PostMissionRoute route;
	switch (reason) {
	case kMissionExitReset: return route;
	case kMissionExitCdTrouble: route.error = PostMissionError::CdTrouble; return route;
	case kMissionExitSystem: route.error = PostMissionError::System; return route;
	case kMissionExitPirate: route.error = PostMissionError::Pirate; return route;
	case kMissionExitBadMission: route.error = PostMissionError::BadMission; return route;
	default: break;
	}
	if (reason >= kMissionExitDisconnectFirst && reason <= kMissionExitDisconnectLast) {
		route.error = PostMissionError::DisconnectReason;
		return route;
	}
	route.keep_session = novaworld;
	return route;
}

} // namespace opennova::inmatch
