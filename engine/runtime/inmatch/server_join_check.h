#pragma once

// The game-layer join check: the host's validation of a remote joiner's C2S 0x00 JOIN, after the
// NP-level 0x42 admitted its connection. One reject code (the DPC its disconnect carries) or 0.
// [orig: Server_ValidatePlayerJoinRequest @0x512100] [orig: Server_ValidatePlayerJoinRequest
//  @0x50f1d0 (dfx2.exe), the rewrite: no PunkBuster leg, a LAN CD-key leg]

#include <cstdint>
#include <vector>

namespace opennova::inmatch {

struct GameConfig;
struct NapiNPConnection;
struct NapiNPServerCtx;

// The CDKIID / CDKIIDEXP1 TLVs of a C2S 0x00 JOIN body onto `conn`, each the C string at its
// value (the last of a repeated tag wins); absent tags leave the field empty. Every host reads
// them; only a DFX2 host's join check tests them.
// [orig: NapiNPServer_HandlePlayerJoinMessage @0x512aa0 - Napi_StrCaseEqual "CDKIID" @0x512b73
//  -> Napi_SetString(player cdkey_id) @0x512b95, "CDKIIDEXP1" @0x512bac -> expansion_cdkey_id
//  @0x512bce] [orig: its twin @0x50fcb0 (dfx2.exe), the same code]
void read_join_cd_key_ids(NapiNPConnection &conn, const std::vector<uint8_t> &join_payload);

// The ladder, in retail's order over the legs the port keeps: the build and ban-type gates
// (2, 3, 4, 6, 7, 8; the NovaWorld app id's 9 is not ported), the title's LAN CD-key leg (10..13,
// DFX2-13), the expansion (47, 48), the spectator legs (14, 15, 16), the side passwords (18..22)
// and, with a server context, the bans (29, 30, 31). `ctx` null (the World-less unit path) runs
// JO's title and no context leg. 0 admits.
uint32_t Server_ValidatePlayerJoinRequest(const GameConfig &config, NapiNPServerCtx *ctx,
		NapiNPConnection &conn, const std::vector<NapiNPConnection> &roster,
		const std::vector<uint8_t> &join_payload, bool pcid_decrypted);

} // namespace opennova::inmatch
