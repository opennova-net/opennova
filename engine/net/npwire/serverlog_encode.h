#pragma once

// `/PROFILE` server-log (".sph") RECORD WRITERS, built from scratch (ADR 0003):
// the inverse of serverlog_decode.h, record for record. Each appends one chunk
// through the container writer (formats/sph append_chunk); the recorder that
// decides when (inmatch/server_log_recorder.h) composes them in retail's order.
// The field maps are docs/net/novaworld-net-re.md §5.22.
//
// Bytes retail never stores (the BEGN mission name's 16th byte, PDAT +32, every
// chunk's +6..+7 pad) are written as zero; retail leaves them as whatever its
// unzeroed 1 MiB heap buffer held there.

#include <net/npwire/serverlog_decode.h>

#include <cstdint>
#include <string>
#include <vector>

namespace opennova {

// The format version BEGN carries [orig: ServerLog_OpenForWrite @0x4e2060].
inline constexpr uint32_t kServerLogVersion = 2;

// BEGN — the header chunk: u32 version 2, then the map file name strncpy'd
// into 15 of 16 bytes [orig: ServerLog_OpenForWrite @0x4e2056..0x4e206f —
// strncpy(buffer + 12, g_MapFileName, 15)].
void append_server_log_begin(std::vector<uint8_t> &out, const std::string &map_file_name);

// PDEF — one roster entry: u32 id, u32 team (the entity+354 char, sign
// extended), u32 name length (strlen + 1), the name and its NUL
// [orig: CServerLog_WritePlayerNameRecord @0x4e1da6..0x4e1dda].
void append_server_log_player(std::vector<uint8_t> &out, uint32_t net_id, int8_t team,
		const std::string &name);

// FBEG — the frame marker: u32 frame index (tick >> 3)
// [orig: CServerLog_WriteTimestampRecord @0x4e1ade..0x4e1ae8].
void append_server_log_frame(std::vector<uint8_t> &out, uint32_t frame_index);

// PDAT — one entity sample, 44 bytes: +8 id, +12 -Y, +16 Z, +20 X (16.16),
// +24 yaw, +28 roll, +36 Flags, +40 u16 vehicle flag, +42 u16 the slot's
// frame-rate stat [orig: CServerLog_WritePositionRecord @0x4e1b3d..0x4e1bb3].
// `e` carries the entity's world triple (entity+4/+8/+12), as the decoder
// reconstructs it.
void append_server_log_entity(std::vector<uint8_t> &out, const ServerLogEntity &e);

// PBRK / PREM — the 12-byte id markers [orig: CServerLog_WriteDeathMarker
// @0x4e1e78..0x4e1e9f; CServerLog_WriteDisconnectMarker @0x4e1c8d..0x4e1cb0].
void append_server_log_event(std::vector<uint8_t> &out, ServerLogEventKind kind,
		uint32_t net_id);

// .END — the 8-byte end chunk [orig: CServerLog_CloseAndFree @0x4e1a4e..0x4e1a58,
// strcpy "DNE.\b" then +8].
void append_server_log_end(std::vector<uint8_t> &out);

} // namespace opennova
