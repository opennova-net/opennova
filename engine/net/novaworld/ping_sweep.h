#pragma once

#include <cstdint>

namespace opennova {

// The server-browser PING SWEEP semantics. On the GSB XXXX finalize retail
// walks every accumulated row and pings its IPv4 (row dword1; the sweep
// carries NO port): NapiGameList_StartPingSweep formats each address and
// creates one ping entry per row, the ping manager opens ONE raw ICMP echo
// socket (`WSASocketA(AF_INET, SOCK_RAW, IPPROTO_ICMP)`, non-blocking) and a
// worker thread drives the echoes; per-row results land in
// NapiGameList_OnPingResult.
// [orig: NapiGameList_StartPingSweep @0x63bcf0 -> NapiPingEntry_Create
//  @0x62ffb0 -> NapiPingManager_Start @0x62fe50; the socket
//  Network_CreateRawSocket @0x62f690 (AF_INET/SOCK_RAW/IPPROTO_ICMP via
//  WSASocketA, FIONBIO); the manager init NapiConnection_Init @0x6302f0]
//
// A raw ICMP socket needs administrator rights on modern Windows, so the
// reimpl's binding sends the echo through the OS ICMP facility
// (IcmpSendEcho2) — the platform-primitive equivalent of retail's raw echo;
// these constants and the result mapping are the witnessed behavior it
// preserves.

// Per-echo timeout and retry count [orig: NapiConnection_Init @0x63037c
// stores 3000 ms at conn+20 and 2 retries at conn+24].
inline constexpr uint32_t kPingTimeoutMs = 3000;
inline constexpr int kPingRetries = 2;

// The per-row result fold the browser applies before display: raw code -4
// (never attempted) reads as -3, every other negative (send/timeout/error)
// reads as -2, and a zero raw code means the entry carries a millisecond
// round-trip time [orig: NapiGameList_OnPingResult @0x63bc60 — raw -4 -> -3,
// -3/-2/-1 -> -2, 0 -> ms from the entry's transfer info].
inline constexpr int kPingNeverAttempted = -3;
inline constexpr int kPingFailed = -2;
inline int fold_ping_result(int raw_code, int ms) {
	if (raw_code == -4) return kPingNeverAttempted;
	if (raw_code < 0) return kPingFailed;
	return ms;
}

} // namespace opennova
