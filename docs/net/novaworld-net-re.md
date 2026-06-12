# NovaWorld networking - protocol + struct RE record

> **Status**: the NovaWorld web/UDP stack (libs/novacrypto, libs/napi, libs/novaworld, the
> standalone server app, and the web portal) landed in PR #37, was reverted in PR #50 while
> it matured, and is now **relanded on the `web-nw-for-real-master` integration branch**
> (see `plan/`). This protocol RE remains the durable wire record; §8 will accumulate
> per-system equivalence verdicts as the IDA grill proceeds.

Consolidated 2026-06-10 from `notes/novaworld_protocol_matrix.md`, `notes/dispatcher_table.md`,
`notes/dispatcher_findings.md`, `notes/spawn_gate_24C1928.md`, `notes/tag_cross_capture_diff.md`,
`notes/struct_typing_2026-04-26.md`, `notes/struct_refine_NapiGameSettings.md`,
`notes/struct_refine_NapiNPProtocol_pad500.md`, `notes/struct_refine_NapiPingManager.md`,
`notes/g_napi_np_ctx_layout.md`, and `notes/architecture.md`.

Binary: `Jointops.exe` (retail JO: Combined Arms, V1.7.5.7), imagebase `0x400000`. All
addresses are absolute in that image unless a section states otherwise. Retail captures
referenced: capture3 (dvxi5, AS Dormant Volcano Isle), capture4 (dvxc1, TD Tenaga Delta),
capture7 (spawn-flow session), retail_capture2 (g11, AS Kendari Airport).

---

## 1. Protocol layering

NovaLogic multiplayer UDP is a layered protocol with a universal wire format and pluggable
per-game message sets:

| Layer | Contents | Reimpl home (reverted stack) |
|---|---|---|
| 4 — message sets | Selected by the `PN` field at CLIENT_HELLO. `PN=NOVAWORLDUDP` → container-based browser/session services (ClientHostRequest, ClientPlayRequest, ServerVerifyResult, ...; §3). `PN="JointOperations"` etc. → in-match TLV game traffic dispatched via the NAPI msginfo tables (§4). | `libs/novaworld` (PN dispatch inside the lib) |
| 3 — session + framing | Per-(ip,port) session state (`CK`, `SK`, `SCRK`, fragment buffers); the opcode `0x43`/`0x83` protocol-message envelope (flags/len/seq/frag). | `libs/napi` |
| 2 — NWU wire framing | 4-byte LSB CRC32 header; 1-byte opcode `0x41` HELLO / `0x42` JOIN / `0x43` SESSION / `0x46` GOODBYE (server replies `0x81`/`0x82`/`0x83`/`0x86`); NWU stream-cipher payload encryption. | `libs/novacrypto` + `libs/napi` |
| 1 — UDP sockets | Owned by the app, not a lib. | server app / Godot client |

The **gate probe** (`novaworld_gate`, UDP 7597) is a separate, simpler protocol: plain
`GATEPROTOCOL` text encrypted with a static `"GATEAPI"` key, no NWU framing. It bootstraps the
client with the HTTP service URL and the address of the Layer-3+ NW UDP server.
`CNapiGateManager` (§6.8) hard-codes `gs.novaworld.net`, port 7597, probe tag `jop:cus2`
(retail JO; jodemo uses `jopd:cus4`).

**Container wire format** (Layer-4 NOVAWORLDUDP messages): marker bytes `0x01` root end,
`0x02` container start, `0x03` container end, `0x04` field start (NUL-terminated name + LE16
length + value + `0x00`), `0x05` field end. `libs/napi/tlv.h::NapiMessage` is byte-for-byte
equivalent to this container model; no separate abstraction is needed.

## 2. Legacy HTTP / web flow

The `NW*.dll` routes as implemented in the reverted stack, matching retail wire behavior:

| Route | Direction | Behavior | Data carried | Notes |
| --- | --- | --- | --- | --- |
| `/nwprepare.dll` | client GET | Issues `EPASK` and persistent login seed cookies. | `EPASK`, `PERSISTENTEXPRESSLOGINDATA`, client IP. | Retail uses `EPASK` to encrypt the login form. |
| `/NWStart.dll` | client GET | Renders the selected start/login template. | Template names, `EPASK`, `YOURIP`. | Lowercase route also accepted. |
| `/NWLogin.dll` POST | client POST | Authenticates EPASK `NAME`/`PASSWORD`; rejects maintenance, bad password, banned/restricted users, denied game access, duplicate active sessions. | `pfid`, `success`, `failure`, `relay`, `msgbase`; resolved `PCID`, `NWH`, `NWHANDLE`, `EXPBITS`. | `pfid=28` maps JO, `pfid=38` maps DFX2. |
| `/NWLogin.dll` GET | client relay GET | Completes login and sets identity cookies. | `NWH`, `NWHANDLE`, `CHAR`, `PCID`, `EXPBITS`, `LOGINSESSIONTAG`. | Relay tag may come from query string: retail HTTP/1.0 drops cookies. |
| `/NWLogout.dll` | client GET | Clears pending login session, active-user row, persistent-cookie pin. | `LOGINSESSIONTAG`, `PERSISTENTEXPRESSLOGINDATA`. | UDP state still tears down via GOODBYE or timeout. |
| `/NWHost.dll` | client GET relay | Issues `HOSTKEY` used by later UDP host registration. | `HOSTKEY`, `NWPF`, `NWPF2`, `PUB*` placeholders. | Host identity is finalized by UDP `ClientHostRequest`. |
| `/NWJoin.dll` | client GET relay | Resolves RID, expansion-gates the joiner, emits `.joi` tokens and PUB cookies. | `RID`, `NK`, `CK`, `BK`, `PUBPCID`, `PUBNAMEINFO`, `PUBSQUADINFO`, `PUBJOINTICKET`. | `NK` is encoded host `ip:port`; `CK` is encoded host app id. |
| `/NWCharacter.dll`, `/NWAccount.dll` | client GET | Renders account/character templates from login cookies. | `NWHANDLE`, `PCID`, `NWH`. | Best-effort identity refresh. |
| `/*.gsb` | client GET | Builds encrypted GSB response from active hosts. | RID, host IP, 26 server-browser fields. | `jop_2.gsb` and `dfx2_0.gsb` routed. |

### GSB server row (positional, 26 fields)

`ServerName`, `GameType`, `MissionName`, `Region`, `Players`, `MaxPlayers`, `Dedicated`,
`TimeLeft`, `Password`, `Country`, `Msg`, `Age`, `TimeOfDay`, `Stat`, `LevelRange`, `Locked`,
`Tracers`, `Skins`, `BBMode`, `Mod`, `PIX`, `PBSERVER`, `VER1`, `Exp`, `Expbits`, `Joicon2`.

Host-var aliases observed from clients:

| Stored field | Client var aliases |
| --- | --- |
| `GameType` | `GameType`, `GameTypeName`, `GameMode` |
| `MissionName` | `MissionName`, `Mission`, `MapName` |
| `Country` | `Country`, `CountryCode` |
| `Password` | `Password`, `Passworded`, `RequiresPassword` |
| `Locked` | `Locked`, `Private` |
| `Dedicated` | `Dedicated` |
| `Stat` | `Stat`, `Stats`, `StatsEnabled` |
| `Exp` | `Exp`, `EXP`, `Expansion` |
| `Expbits` | `Expbits`, `ExpBits`, `EXPBITS` |
| `VER1` | `VER1`, `Ver1`, `Version1` |
| `Joicon2` | `Joicon2`, `JOICON2` |

Open RE items: exact retail rejection message text, complete status-code semantics beyond the
known NWEC mappings, additional host-var aliases retail emits for specific expansions.

## 3. NOVAWORLDUDP session flow

### Session opcodes

| Opcode | Name | Direction | Behavior | Key data |
| --- | --- | --- | --- | --- |
| `0x41` | ClientHello | C→S | Accepts only `PN=NOVAWORLDUDP`, replies ServerHello. | `CI`, `PN`, client address. |
| `0x42` | ClientAuth | C→S | Promotes addr-keyed session; stores client SCRK and server SCRK/SK; replies ServerAuth. | `CI`, `CK`, `SCRK`, `NA`, generated `SK`, `NWUID`. |
| `0x43` | ProtocolMessage | both | Decodes encrypted session packet, ACKs every valid packet, dispatches browser/session containers, fragments/reassembles Layer-4 payloads. | Header `session_id`, `seq_num`, `ack_count`; one or more protocol messages. |
| `0x46` | ClientGoodBye | C→S | Drops connection/session state and active host/player rows. | Client connection id if present. |

### Session containers (browser/session services)

| Container | Direction | Reply | Semantic data |
| --- | --- | --- | --- |
| `ClientConnected` | C→S | `ServerStartVerify` | Starts session verification. |
| `ClientRequestVerifyResult` | C→S | `ServerVerifyResult` | `Success=1` + stable `SessIdString`; maintenance mode → `Success=0`, `MsgCode=3000`. |
| `ClientHostRequest` | C→S | `ServerHostResult` | Registers active host; returns `RID`, `GSID`, `HostRequiresJoinTicket=0`. |
| `ClientHostUpdate` | C→S | ACK only | Refreshes host address, player counts, keys, GSB fields. |
| `ClientPlayRequest` | C→S | `ServerPlayResult` | Stores play setup, acknowledges selected `GSID`. |
| `ClientPlayerEnterRequest` | C→S | `ServerPlayerEnterResult` | Confirms player entry; normalizes `ConnectionId` → `ConnectionID`. |
| `ClientHostPlayerAdded` | C→S | ACK only | Increments host player count until next host update. |
| `ClientHostPlayerRemoved` | C→S | ACK only | Decrements host player count. |
| `ClientStopHosting` | C→S | ACK only | Clears hosting state, removes active host row. |
| `ClientStopPlaying` | C→S | ACK only | Clears play state. |

### Key wire fields

| Field | Meaning | Source |
| --- | --- | --- |
| `CI` | Client connection id. Retail often sends `1` per process, so runtime state is keyed by address. | ClientHello. |
| `CK` | Client local key; server uses it as outbound session header `session_id`. | ClientAuth, join `CK` token. |
| `SK` | Server local key; client echoes it as inbound session header `session_id`. | ServerAuth. |
| `SCRK` | String cipher key for encrypted protocol-message bodies. | ClientAuth/ServerAuth. |
| `NWH` | NovaWorld account handle id/counter. | HTTP login cookies. |
| `NWHANDLE` / `CHAR` | Display handle (retail UI + join identity lookup). | HTTP login cookies. |
| `PCID` | Player account id used by PUBcrypto join cookies. | HTTP login cookies. |
| `EXPBITS` | Expansion ownership/access bitmask. | player game-access, GSB `Expbits`. |
| `RID` | Browser-visible hosted-server row id. | `ClientHostRequest`, GSB, `/NWJoin.dll`. |
| `GSID` | Game session id returned to the host. | `ClientHostRequest`. |
| `HOSTKEY` | HTTP-issued host key copied into UDP host registration. | `/NWHost.dll`. |
| `PCIDKey` | Host PUBcrypto key for join cookies. | `ClientHostUpdate`. |
| `NK` | Encoded host network endpoint token. | `/NWJoin.dll` response. |
| `BK` | Static join token value (onnet + retail captures). | `/NWJoin.dll` response. |

## 4. NAPI in-match dispatch

### Naming convention (load-bearing)

The `NapiNPClientMsg_0xNNN` / `NapiNPServerMsg_0xNNN` name suffix equals the **wire `msg_id`
field**, not the handler-table index. `NapiNPMsgInfo` (16 B) is
`{u32 msg_id, u32 magic, handler, handler2}`. At init, `[orig: NapiNPProtocol_FindMsgInfo @
0x61E380]` builds an inverted index `msginfo_*_index[msg_id] → NapiNPMsgInfo*`; the wire
`msg_type` byte indexes that array directly. The flat tables are **not** sorted by msg_id
(handler-table-index order; e.g. msg_id 0x05 sits at flat index 4, before 0x04 at index 5;
0x24 sits near the table tail). Verified by spot-checking 10+ entries.

### Opcode dispatcher — `[orig: NapiNPProtocol_DispatchOpcode @ 0x622B40]`

Reads the first byte of an inbound packet as opcode and linear-searches `g_np_opcode_handlers`
(`NapiNPOpcodeInfo`, 16 B: `{u32 index, u32 opcode, u32 magic, handler}`; magic always
`0x7C08C6`). Opcode space `0x41`-`0x47` (C→S) and `0x81`-`0x87` (S→C); 14 entries + sentinel
(`index=0xFFFFFFFF` @ `0x849E40`). Table at `0x849D90`:

| Index | Opcode | Handler | IDA name / role |
|---|---|---|---|
| 0 | 0x41 'A' | 0x6213B0 | `NapiNPProtocol_HandleClientHello` (TLV ClientHello) |
| 1 | 0x42 'B' | 0x62B750 | `NapiNPProtocol_HandleClientJoin` (TLV ClientAuth/Join) |
| 2 | 0x43 'C' | 0x626CF0 | `Nwu_HandleClientSession` — thunks to `NapiNPProtocol_HandleSessionPacket(..., has_seq=1)` → ParseMessages. **The ProtocolMessage path.** |
| 3 | 0x44 'D' | 0x6241F0 | resend list / NACK (name pending) |
| 4 | 0x45 'E' | 0x624220 | resend list / NACK (name pending) |
| 5 | 0x46 'F' | 0x624250 | `Nwu_HandleClientGoodbye` (thunks `Nwu_HandleDisconnect`) |
| 6 | 0x47 'G' | 0x624280 | pending decompile (possibly NAT probe / session recovery) |
| 7 | 0x81 | 0x626D20 | `Nwu_HandleServerHello` (response to 0x41) |
| 8 | 0x82 | 0x629840 | `NapiNP_HandleServerJoinResponse` (response to 0x42) |
| 9 | 0x83 | 0x627F90 | server-direction ProtocolMessage mirror of 0x43 |
| 10-13 | 0x84-0x87 | 0x6242B0/0x6242E0/0x624310/0x624340 | server-direction mirrors of 0x44-0x47 (pending) |

`[orig: NapiNPConnection_SendEncryptedPayload @ 0x61F5A0]` writes opcodes `0x47`/`0x87` — a
different category of encrypted packet, **not** the ProtocolMessage path.

### Message dispatcher — `[orig: NapiNPConnection_DispatchMessage @ 0x622570]`

Handles fragment flags (bit `0x06` mid-fragment, `0x04` continuation, `0x02` end), reassembles
into a per-connection `NapiBuffer` at connection offset `+860`, then resolves the handler via
`FindMsgInfo(proto, dir_flag, msg_type)`. `dir_flag`: `0x01` = client-receive
(`msginfo_client_table`), `0x02` = server-receive (`msginfo_server_table`), `+0x04` when
`flags >> 7` (high-byte / settings-update mode; selects the `msginfo_high_*` tables).

### Dispatcher table globals

| Global | Address | Typed as | Notes |
|---|---|---|---|
| `g_np_msginfo_client` | `0x82AE28` | `NapiNPMsgInfo[123]` | S2C dispatch, 122 entries + sentinel (counted from delta to next named global). |
| `g_np_msginfo_server` | `0x82B5D8` | `NapiNPMsgInfo[72]` | C2S dispatch, msg_ids 0x00..0x51 + sentinel (conservative count). |
| `g_np_msginfo_highbit` | `0x849E80` | `NapiNPMsgInfo[64]` | High-bit msg_type dispatch; count not fully witnessed. |

Tables terminate on a sentinel entry with `magic == 0`
(`[orig: NapiNPMsgInfo_BuildIndex @ 0x61e2b0]`); they are assigned to `NapiNPProtocol`'s
`msginfo_*_table` fields in `[orig: CNapiNetwork_Init @ 0x4CA4A0]`. Discrepancy on record: the
dispatcher byte-decode session placed the server table at `0x82B6D8`, the later typing pass at
`0x82B5D8` (= `0x82AE28` + 123×16, self-consistent and applied to the IDB). Treat the IDB
typing as authoritative; re-verify if exact counts ever matter.

### S2C message table (server emits, client handles) — `0x82AE28`

This is what a reimplemented server must **emit**. Semantics merged from the per-tag decompile
sweep; blank = not yet characterized.

| msg_id | Handler | IDA name (suffix = msg_id) | Semantics / wire notes |
|---|---|---|---|
| 0x00 | 0x42E0E0 | `NapiNPClientMsg_0x000` | tiny stub no-op |
| 0x01 | 0x425360 | `_0x001` | reads u32 sync state |
| 0x02 | 0x42E0F0 | `_0x002` | game-start confirm + position ACK |
| 0x03 | 0x425390 | `_0x003` | u8 + 2×u16 sync tick |
| 0x04 | 0x425410 | `_0x004` | session config (max players, flags) |
| 0x05 | 0x42E180 | `_HandleGameStart` | the actual GAME-START UI signal: reads u8 flag; if non-zero, client replies C2S 0x4E and resets game-state dwords; toggles `dword_A86C28`, queues UI notification, resets game timers |
| 0x06 | 0x432BC0 | `_HandleChatCommand` | server→client chat |
| 0x07 | 0x422730 | `_0x007` | per-frame keep-alive stub |
| 0x08 | 0x4281D0 | `_0x008` | game-state snapshot / delta entity updates (~2 KB) |
| 0x0A | 0x42FEC0 | (no IDA function defined; valid prolog) | 604 B in capture7 |
| 0x0B | 0x422660 | `_0x00B` | copies the 616-byte BMS header into `byte_A761D0` (field map §5.4) |
| 0x0C | 0x42E730 | `_0x00C` | full entity spawn batch (~1 KB); parses name fields inline (no opt-in trailer) |
| 0x0D | 0x432C40 | `_0x00D` | pool-entity spawn batch; sets `dword_A82370=3`; per-entity flag-driven layout: u16 count + {u16 flags, u16 slot_id, cstr name, conditional u32/u8 fields per flag bit, always 3×u32 pos, conditional team byte (`flags&0x10`), bone-attach byte}; `flags&0x20` writes entity[+36]; AI-flagged item defs require the `flags&0x800` trailer (§5.6) |
| 0x0F | 0x42E200 | `_0x00F` | **WORLD-STATE-LOAD** (no descriptive Kong name; any "game-start" label is misleading): 4×i32 (sessionId, X, Y, Z), 3×i16 fixed-point angles, u8 flags, team scores, player count, waypoint + team names; sets `dword_81474C=0` (load-bearing input/heartbeat gate); client replies with the C2S burst 0x22 0x23 0x28 0x29 0x2D 0x32; ~624 B, sometimes fragmented in retail |
| 0x10 | 0x433400 | `_0x010` | static entity batch (pool 2): u16 start_idx, u16 count, flag-driven per-entity records; 612-644 B in retail, every frame |
| 0x11 | 0x4226E0 | `_0x011` | one-line stub: `dword_A82358=1` (unblocks WaitForDisconnect); retail only ever ships it bundled last with 0x0B (§5.5) |
| 0x12 | 0x425EE0 | `_0x012` | |
| 0x13 | 0x42EB50 | `_0x013` | |
| 0x14 | 0x42F240 | `_0x014` | |
| 0x16 | 0x42FAE0 | `_0x016` | PLAYER-LIST: max_players, count, per-player slot/ping/scores/flags |
| 0x17 | 0x4226F0 | `_0x017` | |
| 0x18 | 0x433780 | `_0x018` | does not fire in normal multiplayer (§5.7); an early "EntitySpawn" label is unverified |
| 0x19 | 0x425E80 | `_0x019` | |
| 0x1A | 0x425EB0 | `_0x01A` | sets `dword_A82364` (WaitForGameStart return-0 unlock) |
| 0x1B | 0x426080 | `_0x01B` | |
| 0x1C | 0x4227F0 | `_0x01C` | empty stub |
| 0x1D | 0x430840 | `_0x01D` | **spawn-success gate**: sets `dword_24C1928=1` before any payload parse when `is_authority==0` (§5.2) |
| 0x1E | 0x426270 | `_0x01E` (`NetPacket_HandleGameEvent`) | 8-byte game event; does not unblock movement directly |
| 0x1F | 0x427CB0 | `_0x01F` | |
| 0x20 | 0x425C00 | `_0x020` | bulk pool-3 entity sync; sets `dword_A82370=5`; u16 start_idx + u16 count + per-entity {u16 type_id, u8 flags, 3×u32 pos, u32 (f&1), u32 (f&2), u16→entity[+290] (f&4), u16→entity[+124], u8 team (f&8), u16 (f&0x10), u8 (f&0x20)}; allocates pool-3 entries |
| 0x21 | 0x430B10 | `_HandleSpawnEffect` | |
| 0x22 | 0x42EC90 | `_0x022` | part of the 0x0F reply ecosystem |
| 0x23 | 0x4F81E0 | `_0x023` | part of the 0x0F reply ecosystem |
| 0x24 | 0x429E70 | `_0x024` | (sits near the flat-table tail) |
| 0x25 | 0x422800 | `_0x025` | game reset, empty payload. Client side: input reset, clears camera/HUD state dwords, `dword_24C1928=1` + companion `dword_24C195C=1`, increments round counter `dword_24C116C`. Server side: round counter only. Retail does **not** use S2C 0x25 in the spawn flow (§5.2) |
| 0x26 | 0x42EC30 | `_0x026` | entity kill-sync (killer/victim slots); fires on kill events |
| 0x27 | 0x425AA0 | `_0x027` | |
| 0x28 | 0x425B40 | `_0x028` | |
| 0x29 | 0x427D00 | `_0x029` | |
| 0x2A | 0x425BA0 | `_0x02A` | chat-history entries |
| 0x2B | 0x427DF0 | `_0x02B` | |
| 0x2C | 0x427E10 | `_0x02C` | chat entry |
| 0x2D | 0x427E90 | `_0x02D` | |
| 0x2E | 0x427F80 | `_0x02E` | |
| 0x2F | 0x430E10 | `_0x02F` | |
| 0x30 | 0x431170 | `_HandleChecksumRequest` | anti-cheat CRC challenge → client replies C2S 0x21 |
| 0x31 | 0x4311E0 | `_0x031` | CRC request for weapon loadout → C2S 0x21 |
| 0x32 | 0x428060 | `_0x032` | |
| 0x33 | 0x425FA0 | `_0x033` | |
| 0x34 | 0x4283A0 | `_0x034` | |
| 0x35 | 0x4261A0 | `_0x035` | |
| 0x36 | 0x426120 | `_0x036` | |
| 0x37 | 0x431250 | `_0x037` | |
| 0x38 | 0x4260B0 | `_0x038` | |
| 0x39 | 0x42E6D0 | `_0x039` | |
| 0x3A | 0x422680 | `_0x03A` | |
| 0x3B | 0x431340 | `_0x03B` | |
| 0x3D | 0x422870 | `_0x03D` | |
| 0x3E | 0x4226D0 | `_0x03E` | ack-style |
| 0x3F | 0x42BB20 | `_0x03F` | |
| 0x40 | 0x425A50 | `_0x040` | capture-zone state (1 B) → minimap-overlay array `unk_28E5620` via the `sub_5BEBB0` chain; needed for AS capture-zone markers |
| 0x41 | 0x4254C0 | `_0x041` | |
| 0x42 | 0x4281A0 | `_0x042` | spawn-gate-adjacent; retail emits during load |
| 0x43 | 0x42FA90 | `_0x043` | |
| 0x44 | 0x422710 | `_0x044` | |
| 0x45 | 0x422890 | `_0x045` | empty payload; sets `dword_A82370=6`; calls `PolyTrn_LoadTileData()` (terrain texture rebuild) |
| 0x46 | 0x431370 | `_0x046` | PLAYER-SYNC: slot + u16 fields-present bitfield (bit 15 = remove); read order must match IDA |
| 0x48 | 0x4284B0 | `_0x048` | |
| 0x49 | 0x42C0A0 | `_0x049` | weapon-reload notification |
| 0x4C | 0x428570 | `_0x04C` | target-assignment list (squad/AI orders) |
| 0x4D | 0x4317B0 | `_HandleSpawnSlot` | reads u8 slot; local slot → tip event; otherwise client replies C2S 0x22+0x23; reads `dword_24C1928` as a skip-tip-if-already-spawned guard (never writes it) |
| 0x4E | 0x431870 | `_HandleBatchSpawn` (misleading) | u16 count + per-slot u16; calls `Entity_KillBySlotId` (kill, not spawn), then replies C2S 0x28 |
| 0x4F | 0x4286C0 | `_0x04F` | |
| 0x50 | 0x431910 | `_0x050` | |
| 0x51 | 0x431BB0 | `_0x051` | |
| 0x52 | 0x428A80 | `_0x052` | |
| 0x53 | 0x428AE0 | `_0x053` | |
| 0x54 | 0x429040 | `_0x054` | |
| 0x56 | 0x431D10 | `_0x056` | touches `dword_24C1928` (write unconfirmed; decomp on demand) |
| 0x57 | 0x432210 | `_0x057_RTT` | RTT echo |
| 0x58 | 0x4228C0 | `_0x058` | texture loader (terrain assets) |
| 0x59 | 0x4228E0 | `_0x059` | |
| 0x5A | 0x4290E0 | `_0x05A` | weapon-loadout sync; resets `dword_81474C=0` |
| 0x5B | 0x4322B0 | `_0x05B` | |
| 0x5C | 0x425200 | `_0x05C` | |
| 0x5D | 0x429730 | `_0x05D` | entity-destroy list (cleanup) |
| 0x5E | 0x4297B0 | `_0x05E` | |
| 0x5F | 0x4228F0 | `_0x05F` | |
| 0x60 | 0x432350 | `_HandleFileTransferChunk` | u32 sessionId, u32 totalSize, u32 offset, payload; incomplete → client requests the next chunk via C2S 0x33 |
| 0x61 | 0x4297C0 | `_HandleSessionKey` | u32 session key → `g_sessionKey`; **disables `_connectlog.txt`** (source of the "DISABLING CONNECTLOG" log line) |
| 0x62 | 0x42D200 | `_0x062` | |
| 0x63 | 0x42D450 | `_0x063` | |
| 0x64 | 0x432410 | `_0x064` | mission-file chunk transfer (like 0x60); incomplete → C2S 0x37 |
| 0x65 | 0x429870 | `_0x065` | |
| 0x66 | 0x42D4C0 | `_HandleWeaponRestrictions` | count + (slot, restriction) pairs |
| 0x67 | 0x42D570 | `_0x067` | |
| 0x68 | 0x42DAA0 | `_0x068` | |
| 0x6A | 0x432510 | `_0x06A` | |
| 0x6B | 0x425520 | `_0x06B` | |
| 0x6C | 0x428FC0 | `_0x06C` | |
| 0x6D | 0x430C50 | `_HandleEntityDeath` | |
| 0x6E | 0x429880 | `_0x06E` | team/squad roster sync |
| 0x6F | 0x428D60 | `_0x06F` | cinematic camera assignment |
| 0x70 | 0x429A30 | `_0x070` | |
| 0x71 | 0x425600 | `_0x071` | |
| 0x72 | 0x425710 | `_0x072` | |
| 0x73 | 0x425770 | `_0x073` | |
| 0x74 | 0x4258B0 | `_0x074` | |
| 0x75 | 0x4259E0 | `_0x075` | spectator-mode flags (2 B); sets `byte_A860EC`, `dword_24D1DF4` |
| 0x76 | 0x42D540 | `_0x076` | u16 → `dword_24D59FC` |
| 0x78 | 0x4259070 | `_0x078` | (address as recorded in the source note has 7 hex digits — likely a typo; re-verify in the IDB) |
| 0x79 | 0x429B00 | `_0x079` | spectator-mode (1 B → `dword_82BEE4`) |
| 0x7A | 0x429B40 | `_0x07A` | player name (max 64 chars) → server-info struct |
| 0x7B | 0x429BB0 | `_0x07B` | full player info: 5×cstring(32), u32, 2×cstring(512) (MOTD) |
| 0x7C | 0x426020 | `_0x07C` | |
| 0x7D | 0x432690 | `_0x07D` | |
| 0x7E | 0x425E20 | `_0x07E` | two cstrings → `byte_A86520` / `byte_A86120` (server config strings) |
| 0x7F | 0x429E60 | `_0x07F` | |
| 0x80 | 0x42A070 | `_0x080` | |
| 0x81 | 0x42A0B0 | `_0x081` | |
| 0x82 | 0x42A0E0 | `_0x082` | |
| 0x83 | 0x4326E0 | `_0x083` | |

Note: a separate `0x42FEC0` S2C 0x0A handler exists; the previously cited
`NetPacket_SerializePlayerState @ 0x4C09C0` is a server-side serializer, not the client
handler for tag 0x0A.

### C2S message table (client emits, server handles) — `0x82B5D8`

This is what a reimplemented server must **handle**.

| msg_id | Handler | Notes |
|---|---|---|
| 0x00 | 0x512AA0 | **JOIN** — initial client→server packet (allocates session, returns session key) |
| 0x01 | 0x512ED0 | likely FORM_POST; also compares side passwords during early join (§6.4) |
| 0x02 | 0x512FD0 | likely GLB_JOIN |
| 0x03 | 0x501BE0 | |
| 0x04 | 0x5199D0 | |
| 0x06 | 0x513310 | `_0x006_ClientFiredRound` |
| 0x07 | 0x4FC970 | |
| 0x08 | 0x502210 | entity movement/state delta |
| 0x09 | 0x513200 | client checksum response |
| 0x0A | 0x513260 | |
| 0x0B | 0x51AB10 | |
| 0x0C | 0x501C30 | |
| 0x0D | 0x513760 | replication frame ACK |
| 0x0E | 0x519AF0 | |
| 0x0F | 0x514180 | client input frame (movement + buttons; ~33 ms cadence); len-2 form is the spawn-point query seen in the fallback spawn-menu loop |
| 0x13 | 0x514330 | |
| 0x14 | 0x501E00 | |
| 0x16 | 0x511A70 | client→server chat |
| 0x17 | 0x514850 | |
| 0x18 | 0x51A020 | |
| 0x19 | 0x514250 | |
| 0x1A | 0x514B20 | |
| 0x1B | 0x501D90 | |
| 0x1C | 0x501D40 | |
| 0x1D | 0x501C60 | |
| 0x20 | 0x501F70 | |
| 0x21 | 0x502050 | checksum reply (response to S2C 0x30/0x31) |
| 0x22 | 0x514C90 | member of the client reply burst to S2C 0x0F / 0x4D |
| 0x23 | 0x514D50 | burst member |
| 0x24 | 0x514DC0 | |
| 0x25 | 0x514DF0 | weapon-reload request (mid-game) — note the direction asymmetry vs S2C 0x25 (§5.3) |
| 0x26 | 0x502390 | |
| 0x27 | 0x4FC980 | |
| 0x28 | 0x51A550 | burst member; also the ack reply to S2C 0x4E |
| 0x29 | 0x514F10 | burst member |
| 0x2B | 0x514FE0 | |
| 0x2C | 0x515070 | burst-member receiver |
| 0x2D | 0x502430 | burst-member receiver |
| 0x2E | 0x515390 | |
| 0x2F | 0x515790 | |
| 0x30 | 0x5029B0 | |
| 0x31 | 0x5024A0 | |
| 0x32 | 0x51A600 | burst-member receiver |
| 0x33 | 0x515230 | next-chunk request (reply to S2C 0x60) |
| 0x34 | 0x5024B0 | |
| 0x35 | 0x500DF0 | |
| 0x36 | 0x500E00 | |
| 0x37 | 0x5152E0 | mission-chunk re-request (reply to S2C 0x64) |
| 0x38 | 0x502510 | |
| 0x39 | 0x500E20 | |
| 0x3C | 0x519110 | |
| 0x3D | 0x500EC0 | |
| 0x3E | 0x500E10 | |
| 0x3F | 0x518F10 | |
| 0x40 | 0x51C4C0 | |
| 0x41 | 0x510540 | |
| 0x42 | 0x510930 | |
| 0x43 | 0x510990 | |
| 0x44 | 0x510AE0 | |
| 0x45 | 0x510C00 | |
| 0x46 | 0x510D20 | |
| 0x47 | 0x510ED0 | semantics unknown (client sends; C2S decompile sweep pending) |
| 0x48 | 0x510F30 | semantics unknown (sweep pending) |
| 0x49 | 0x510F40 | |
| 0x4B | 0x510DC0 | |
| 0x4C | 0x5111B0 | |
| 0x4D | 0x518F70 | |
| 0x4E | 0x511210 | client reply when the S2C 0x05 flag byte is non-zero |
| 0x4F | 0x514A40 | |
| 0x50 | 0x5112B0 | |
| 0x51 | 0x51C840 | |

### Open questions

- Magic `0x7C08C6` in every dispatch entry — likely a build/version stamp.
- `handler2` of `NapiNPMsgInfo` — always zero in observed entries; possibly the
  `cb_server_3`/`cb_client_0` callback slots per the dispatcher decompile.
- `msginfo_high_*` tables (selected when `flags >> 7`) — index pointers live in
  `NapiNPProtocol` (§6.5) but the populating init path is unwitnessed; deferred until a
  `flags=0x80` packet shows up in capture analysis.
- Opcode handlers `0x6213B0`..`0x624340` mostly lack descriptive names.
- msg_id values `0x86`/`0x87`/`0x88+` observed in the client-table tail; dispatch assignment
  unclear (possibly dead entries).
- Phase B (full C2S decompile sweep) was never done; priority candidates 0x47, 0x48.

## 5. Tag-level findings (audited against retail captures)

### 5.1 Loading-progress counter — `dword_A82370`

The server walks the client's loading progress up via specific S2C tags. Each handler
increments at most (sets only if current < N):

| Tag | Sets `dword_A82370` to | Note |
|---|---|---|
| 0x0D | 3 | pool-entity spawn batch |
| 0x20 | 5 | bulk pool-3 entity sync |
| 0x45 | 6 | terrain LOD load trigger (`PolyTrn_LoadTileData`) |

A client below 6 may sit in a half-loaded state that prevents spawn confirmation.

### 5.2 Spawn-success gate — `dword_24C1928`

`[orig: NapiClient_WaitForGameStart @ 0x42cc10]` is the loading-screen wait loop. It returns
1 (success — drop loading screen, enter level) when `dword_24C1928` is set; returns 4 on
`dword_24C1878` (timeout), 3 on `dword_24C187C` (user cancel), 0 on `dword_A82364` (set by
S2C 0x1A).

Two S2C client handlers set the gate, but only one is used by retail:

- **Tag 0x1D** `[orig: NapiNPClientMsg_0x01D @ 0x430840]` — sets `dword_24C1928 = 1` **before
  any payload parsing**, gated only on `is_authority == 0`; even an empty payload clears the
  gate. Also writes `dword_C8D820 = 0x7FFFFFFF` (round-end sentinel at +infinity). Present in
  retail capture7's S2C inventory: **this is the retail spawn-gate trigger.**
  - Wire format, slow path (taken when ctx `+0x58` (`is_in_session`) is non-zero and
    `g_GameType & 0x10000` is clear): 3×cstring (max 32 each → `byte_A81B40/60/80`; server /
    mission / scenario names) + 3×i16 sign-extended (→ `dword_A81BA0/A4/A8`; score caps).
  - Fast path (otherwise): u8 → `dword_A81B30`, 2×i16 → `dword_A81B34/38`, u8 →
    `byte_A81BAC`, u8 → `dword_A81B3C`.
  - Minimum non-crashing payload: 9 zero bytes (three empty cstrings + three zero i16).
    Zeroes leave HUD/scoreboard strings blank but do not block gameplay.
- **Tag 0x25** `[orig: NapiNPClientMsg_0x025 @ 0x422800]` — empty payload; client side resets
  input bindings mid-frame, clears camera/HUD state, sets the gate plus companion
  `dword_24C195C = 1`, and increments round counter `dword_24C116C`. Retail uses 0x25 **only
  C2S** (weapon reload); the S2C direction is not part of the retail spawn flow.

Of 70+ writers of `dword_24C1928`, most are server-side (`NapiNPServerMsg_*`, `Server_*`,
`SaveFile_*`) and never run on a retail client. S2C 0x4D only reads the gate; 0x56/0x57 were
not confirmed as writers. `dword_24C1878` (timeout) writers are mission-load state
transitions: `[orig: CNapiNetwork_OnConnectedToServer @ 0x4c62e0]`,
`[orig: CNapiNetwork_OnDisconnectedFromServer @ 0x4c63d0]`,
`[orig: CNapiGameSession_CreateServerConnection @ 0x4c9d30]`,
`[orig: Game_StartMission @ 0x524360]`, `[orig: SaveFile_SendAndWaitForServerAck @ 0x5204b0]`.

Live-test record (2026-04-26, reverted stack vs retail client):

1. Emitting 0x1D cleared the gate ("Mission loading complete") but the `dword_C8D820 =
   0x7FFFFFFF` side effect made the client show "game has ended" and send ClientGoodBye.
2. Emitting 0x25 cleared the gate, but the round-counter increment likely triggered a
   re-load, after which the client fell into the spawn-select-menu fallback: a C2S tag 0x0F
   len-2 query loop (22+ queries per packet, pool-1 slot indices `0x10NN`) without ever
   spawning. The menu-bypass tag retail sends to skip that fallback was still unidentified at
   the time of the revert; candidates were the 0x05 flag byte (must be non-zero) and tags
   retail emits that we did not (e.g. 0x40).

### 5.3 Tag direction asymmetry (durable warning)

Most/all tags have **both** a `NapiNPClientMsg_*` (S2C receive) and a `NapiNPServerMsg_*`
(C2S receive) handler, and the two usually implement **different protocol meanings** — they
share only the tag byte. When reading capture inventories, always note message direction; a
tag's appearance in one direction says nothing about the other. The 0x25-vs-0x1D confusion in
§5.2 is the canonical case of this asymmetry biting an investigation.

### 5.4 Tag 0x0B — 616-byte BMS header field map

All observed blobs are 616 bytes (the size hardcoded into the handler's copy into
`byte_A761D0`). Cross-capture diff over three maps (dvxi5 / dvxc1 / g11), sourced from the
fragment-aware dissector (`tools/wireshark/jointops_udp.lua`); never diff from
non-reassembled per-fragment extracts:

| Offset (dec) | Width | Field | dvxi5 | dvxc1 | g11 | Category |
|---|---|---|---|---|---|---|
| 0-3 | 4 | Signature | `42 4d 53 13` | same | same | structural (invariant) |
| 4-35 | 32 | Mission display name | "AS - Dormant Volcano Isle" | "TD - Tenaga Delta" | "AS - Kendari Airport" | map-specific |
| 36-67 | 32 | Designer | "Brophy / Brent / BB" | "Brent Houston / James Payne" | "Brent" | map-specific |
| 68-99 | 32 | Map basename | "dvxi5" | "dvxc1" | "g11" | map-specific — **the BMS file the client must load** |
| 116-127 | ~12 | "Default" + padding | "Default\0..." | same | same | structural (invariant string) |
| 136 | 1 | Game-mode primary | 0x03 | 0x03 | 0x02 | map-specific (3=AS-asym, 2=AS-sym?) |
| 138 | 1 | Game-mode secondary | 0x01 | 0x00 | 0x01 | map-specific |
| 139 | 1 | Game-mode tertiary | 0x00 | 0x20 | 0x00 | map-specific |
| 154 | 1 | numeric (count?) | 21 | 18 | 0 | map-specific (spawn count / team size?) |
| 158-159 | u16 | numeric | 700 | 400 | 1000 | map-specific (round time / score limit?) |
| 164 | 1 | numeric | 76 | 47 | 144 | map-specific |
| 168-169 | u16 | numeric | 1102 | 1145 | 1153 | map-specific (likely entity count) |
| 172-173 | u16 | numeric | 432 | 436 | 558 | map-specific (entity count secondary?) |
| 184 | 1 | flag/count | 2 | 0 | 11 | map-specific |
| 192-215 | 24 | 12-slot ID table? | filled with `ff` | zeros | zeros | map-specific |
| 246-253 | 8 | Icon key | "full_00" | same | same | structural (invariant string) |
| 276-307 | 32 | Atlas key | "trntile10" | "trntile10" | "trntilea1" | atlas-specific |
| 308-615 | 308 | tail metadata + zeros | sparse non-zero | mostly zeros | mostly zeros | map-specific |

Consequence: any fixture blob of this tag is **map-locked** (the basename at +68 names the BMS
the client loads); a server must synthesize it from the actual mission selection.

### 5.5 Tag 0x11 bundling policy (retail)

Retail emits S2C 0x11 **only** inside the BMS-state bundle, always last, never standalone —
verified in both capture3 (frames 197227/197228, the only S2C 0x11 emissions) and capture4
(frames 163797/163798): bundle signature `[0x1C(0), 0x0B(616), 0x66(1), 0x76(2), 0x11(0)]`.

Why it matters: 0x11 sets `dword_A82358 = 1`, which unblocks WaitForDisconnect; the client
then immediately runs `Game_LoadTerrainDuringConnect`, which reads the mission basename out of
the tag-0x0B header copy (`byte_A761D0+0x44`) via `sub_610940`. Sending 0x11 before the 0x0B
header is delivered makes that load fail → unnumbered "Mission loading aborted" → disconnect
~2-3 s later. A reimplementation must deliver 0x0B and 0x11 in the same packet (0x11 last) or
at minimum strictly after 0x0B.

### 5.6 Tag 0x0D — local-player spawn dead end (durable warning)

Do **not** use S2C 0x0D to spawn the local player. Verified by live crash + decompile
(2026-04-25):

- When the record's item def has the AI flag (`ItemDef[+84] & 0x100000` — true for player
  infantry type_id `0x14B9`), the handler `[orig: NapiNPClientMsg_0x00D @ 0x432C40]` runs an
  `Entity_AllocateAISlot` path that string-copies from pointers populated **only** by the
  optional `flags & 0x800` trailer block (`[u16][u32][cstring ai_name]`). Without the trailer
  those pointers are NULL → access violation at handler+0x730 (`0x433370`).
- Retail's S2C 0x0D records always use vehicle/AI type_ids (`0x04bf`, `0x050b`, ...) with
  flags like `0x1c21`/`0x1071` — bit `0x800` always set, never the local-player template.
  Tag 0x0C does not crash on the same type_id because it parses name fields inline.
- The earlier theory that 0x0D was "the only wire route" to clear the local player's
  entity[+36] movement gate was wrong. `[orig: Player_BuildTag0CInputBody @ 0x42A550]` checks
  three gates before serializing player input: entity buffer non-NULL, `entity[+286]` non-zero
  (= `ItemDef.healthMax`, §6.9), and `entity[+36]` bit 1 clear. The remaining unsolved spawn
  blocker pointed at `entity[+286]` init paths and the `dword_A82370` progression (§5.1), not
  at +36.

### 5.7 Spurious tag 0x18

S2C 0x18 does not fire in normal multiplayer (the `NapiNPServerMsg_0x00F → 0x18` reply path is
inert); the reverted stack emitted it speculatively and it was marked for removal.

### 5.8 Wire-format verification gaps

Tags emitted by the reverted stack but never byte-compared against retail: 0x0A (604 B
retail), 0x10 (612-644 B; flag layout), 0x16 (per-player record layout), 0x46 (bitfield read
order), 0x60 and 0x64 (file-transfer chunks; a format error makes the client request
retransmits forever). Cross-capture diffs still pending for 0x0F, 0x10, 0x60, 0x64, 0x7B
(+13-byte size delta vs the reverted builder).

## 6. Struct reference

All structs typed in the IDB during the 2026-04-26 per-class typing pass (Stage 5 of the
decomp-quality plan) and the follow-on refinements. Witness rule: every named field has ≥2
independent decompile witnesses unless flagged. One Kong artifact to know: the
`CNapiSession_*` name prefix is **heterogeneous** (it covers both particle/effect code and
message-queue code) — it is not a real class.

### 6.1 `NapiNPMsgInfo` / `NapiNPOpcodeInfo` (16 B each)

| Struct | Layout | Notes |
|---|---|---|
| `NapiNPMsgInfo` | `{u32 msg_id, u32 magic, handler, handler2}` | sentinel = `magic==0`; `handler2` always 0 in observed entries |
| `NapiNPOpcodeInfo` | `{u32 index, u32 opcode, u32 magic, handler}` | magic always `0x7C08C6`; sentinel `index=0xFFFFFFFF` |

### 6.2 `CNapiNetwork` (~4524 B; methods 0x4a8040-0x4ca4a0; 32/33 typed)

| Offset | Field | Size | Notes |
|---|---|---|---|
| 0 | `list_heads[5]` | 80 | five linked-list head sentinels |
| 80 | `transport_mode` | 4 | 0=down, 1=host, 2=client_relay, 3=client_direct (per method dispatch) |
| 84 | `socket_state` | 4 | state machine 0..4 |
| 88 | `field_58` | 4 | gates OpenTransportSocket |
| 92-100 | `field_5C/60/64` | 12 | likely a small host/client config-flag sub-struct |
| 104 | pad | 3372 | unaccounted NAPI internals (queues / logging / per-session state) |
| 3476 | `disconnect_event_buf` | 184 | `NapiNPDisconnectEvent` buffer; cleared by Shutdown |
| 3660-3668 | `field_E4C/E50/E54` | 12 | |
| 3672 | `np_manager` | 4 | `NapiNPManager *` |
| 3676 | `np_protocol` | 4 | `NapiNPProtocol *` |
| 3680 | `field_E60` | 4 | |
| 3684 | `ping_manager` | 4 | `NapiPingManager *` |
| 3688 | `game_settings` | 216 | inline `NapiGameSettings` (§6.4) |
| 3904 | `server_info_buf` | 72 | |
| 3976 | `net_config` | 520 | inline `NapiNetConfig` |
| 4496 | `field_1190` | 4 | |
| 4500 | pad | 20 | |
| 4520 | `field_11A8` | 4 | |

Key witnesses: `[orig: CNapiNetwork_Init @ 0x4ca4a0]` (all list heads + manager pointers +
settings init; also configures the ping manager to 3000/2/10, §6.6),
`[orig: CNapiNetwork_OpenTransportSocket @ 0x4c6a40]` (transport-mode dispatch),
`[orig: CNapiNetwork_CheckPlayerTimeouts @ 0x4c8ad0]` (walks `np_protocol`'s connection list),
`[orig: CNapiNetwork_Shutdown @ 0x4ca440]`. Open: one method missed in the typing batch
(probably `CNapiNetwork_GetConnectionParams @ 0x4a8040`); the 3372-byte interior gap.

### 6.3 `NapiNPServerCtx` — `g_napi_np_ctx @ 0xB5CBC8` (4520 B, 473 xrefs)

The game-level singleton is a **`CNapiNetwork`-shaped header** plus game-specific trailing
fields: field offsets line up byte-for-byte (CNapiNetwork is 4524 B, the singleton 4520 B; the
difference is one trailing pointer), and every call site passes `&g_napi_np_ctx` cast to
`CNapiNetwork *`. Applied (conservative) layout:

| Offset | Field | Size | Notes |
|---|---|---|---|
| 0x000 | `list_heads[5]` | 80 | `+0x04` = enumerated session-list head (NovaWorld + LAN); `+0x24` = player connection-list head (PCID dup-check walks it in `[orig: Server_ValidatePlayerJoinRequest @ 0x512100]`) |
| 0x050 | `transport_mode` | 4 | 1=NovaWorld, 2=LAN (UI enumeration branches ==1 → SetTransportMode(4), ==2 → (2)); NovaWorld-only AppId/JoinTicket gates check ==1 |
| 0x054 | `socket_state` | 4 | |
| 0x058 | `is_in_session` | 4 | non-zero whenever an MP session is in progress; gates `[orig: Server_PumpNetworkTransport @ 0x4FD960]`, 14 branches of `[orig: Server_TickUpdate @ 0x51D7E0]`, 11 of `[orig: Game_StartMission @ 0x524360]`, the whole body of `[orig: NetClient_FlushAndSync @ 0x424710]` |
| 0x05C | `field_5C` | 4 | |
| 0x060 | `is_authority` | 4 | non-zero on host/server (preserved Kong name) |
| 0x064 | `is_mp_session_peer` | 4 | renamed 2026-04-26 from `is_dedicated_server` (see below) |
| 0x068 | pad | 3372 | NAPI internals |
| 0xD94 | `disconnect_event_buf` | 184 | |
| 0xE4C-0xE54 | `field_E4C/E50/E54` | 12 | |
| 0xE58 | `np_manager` | 4 | `NapiNPManager *` |
| 0xE5C | `np_protocol` | 4 | `NapiNPProtocol *` (was Kong `player_list_owner`); `[orig: NapiNPServer_SendFiltered @ 0x4C87E0]` derefs its connection list at +0xEBC |
| 0xE60 | `field_E60` | 4 | protocol pointer cached for the client frame path (`[orig: Client_ProcessNetworkFrame @ 0x42C180]`); never proven to differ from `np_protocol` |
| 0xE64 | `ping_manager` | 4 | `NapiPingManager *` (global alias `0xB5DA2C`) |
| 0xE68 | `game_settings` | 216 | inline `NapiGameSettings` (§6.4); side passwords at absolute 0xEA8/0xEC8 drive join-reject codes 19/20 |
| 0xF40 | `server_info_buf` | 72 | `+0xF3C` region note: a PunkBuster handle/flag is read at 0xF3C (single witness, `Server_TickUpdate` → `PBServer_Shutdown`) |
| 0xF88 | `net_config` | 520 | |
| 0x1190 | `field_1190` | 4 | NAPI-internal only |
| 0x1194 | pad | 4 | |
| 0x1198 | `send_mask` | 4 | bitmask used by `NapiNPServer_SendFiltered` (preserved) |
| 0x119C | `send_target_player` | 4 | preserved |
| 0x11A0 | `send_target_state` | 4 | preserved |
| 0x11A4 | `send_filter_416` | 4 | preserved |

`is_mp_session_peer` rename rationale (user-approved, applied to the IDB): the literal
"is dedicated server" reading is contradicted by three witnesses —
`[orig: CNapiGameSession_BuildHostVarLists @ 0x4D0B50]` publishes the lobby key `Dedicated=0`
when the flag is set (inverted); `[orig: Chat_SendTeamMessage @ 0x49A900]` takes the
client-style reliable-uplink branch when set and the host broadcast branch on `is_authority`
(a real dedi host would never take the first); `[orig: Game_StartMission @ 0x524360]` gates
both polarities in ways only consistent with "peer in an MP session".
`[orig: Game_ParseCommandLineAndInit @ 0x4A7310]` never sets it from `/SERVEONLY`.

Deliberately not applied from the maximal field map: inline `gs_*` password names at the
singleton level (the agent's inline arithmetic was off by one 32-byte slot; the authoritative
offsets are the `NapiGameSettings` ones in §6.4 — side A at settings+0x40 = absolute 0xEA8,
side B at +0x60 = 0xEC8, both directly witnessed in `Server_ValidatePlayerJoinRequest`), a
separate `active_protocol` name for 0xE60, and `pb_server_handle` at 0xF3C (single witness).
Admin SET-command password buffers live in a game-state struct reached through `np_protocol`,
not inline in the singleton.

### 6.4 `NapiGameSettings` (216 B; inline at `CNapiNetwork+3688` / `g_napi_np_ctx+0xE68`)

10 of 11 fields named, all ≥2 witnesses. Strongest source:
`[orig: ServerConfig_ApplyHostSetting @ 0x4a6000]` maps host-config keys to exactly the
globals that `[orig: CNapiGameSession_BuildAndCreateSession @ 0x5694d0]` copies into this
struct. Other witnesses: `[orig: SinglePlayer_StartMission @ 0x561af0]` (default init),
`[orig: Server_ValidatePlayerJoinRequest @ 0x512100]` + `[orig: NapiNPServerMsg_0x001 @
0x512ed0]` (validation sink), `[orig: Game_SaveConfig @ 0x54c490]` (game.cfg writer),
`[orig: UI_PopulateHostSettingsFromConfig @ 0x555fe0]` (host-screen widget labels),
`[orig: CNapiGameSession_BuildHostVarLists @ 0x4d0b50]` (lobby publish),
`[orig: CAdminServer_HandleSetCommand @ 0x405a60]` (admin SET).

| Offset | Field | Type | Config key / semantics |
|---|---|---|---|
| 0x00 | `server_name` | char[32] | lobby-visible `"ServerName"`; saved as `game_name`; UI widget `GAME_NAME` |
| 0x20 | `server_password` | char[32] | `MPHostGamePassword` / admin SET `ServerPassword`; widget `SERVER_PASSWORD` |
| 0x40 | `side_a_password` | char[32] | `MPHostSidePasswordA`; widget `BLUE_PW`; mismatch → join-reject code 19 |
| 0x60 | `side_b_password` | char[32] | `MPHostSidePasswordB`; widget `RED_PW`; mismatch → join-reject code 20 |
| 0x80 | `internet_address` | char[64] | connect-target hostname/IP, default `"0.0.0.0"`; resolved via `Napi_ResolveAddress` in transport mode 3 |
| 0xC0 | `max_players` | u32 | `MaxPlayers`, clamped 1..65 |
| 0xC4 | `use_lineup_queue` | u32 | `UseLineUpQueue` |
| 0xC8 | `lineup_queue_size` | u32 | `LineUpQueueSize` |
| 0xCC | `game_type` | u32 | `mp_gametype` enum (the mission entry's gametype dword); discrete values not enumerated |
| 0xD0 | `mp_attributes` | u32 | `mpattrib` bitmask — observed bits: 0x001 NoTracers, 0x004 TeamChoose, 0x008 FFWarning-suppress, 0x200 NoFriendlyFire, 0x400 NoFriendlyTag, 0x8000 ClaymorePref; `[orig: CNapiServerConfig_BuildFlags @ 0x4c4dc0]` re-derives a public flag word |
| 0xD4 | `_pad_0xD4` | 4 | actually the `sv_punkbuster` server flag carried into the session blob (single witness pair; rename to `sv_punkbuster_enabled` once a second witness lands; see `[orig: Config_SetPunkBusterServerEnabled @ 0x4d94c0]`) |

### 6.5 `NapiNPProtocol` (4064 B, 94 named members; reached via `g_napi_np_ctx.np_protocol`)

Full layout after the `_pad_0x500` refinement (2120 unknown bytes → 100% named):

| Offset | Field | Type/size | Notes |
|---|---|---|---|
| 0x000 | `manager` | ptr | `NapiNPManager *` |
| 0x004 | `link` | 16 | `NapiListNode` |
| 0x014 | `instance_id` | u32 | |
| 0x018 | `company` | char[64] | |
| 0x058 | `machine_name` | char[64] | |
| 0x098 | `build_date` | char[64] | |
| 0x0D8 | `unk_0xD8` | u32 | |
| 0x0DC | `game_name` | char[64] | |
| 0x11C | `version_block` | 16 | `NapiNPVarBlock` |
| 0x12C | `version_string` | char[64] | |
| 0x16C | `max_players_string` | char[64] | |
| 0x1AC | `build_string` | char[64] | |
| 0x1EC | `msginfo_client_table` | ptr | flat S2C table (§4) |
| 0x1F0 | `msginfo_server_table` | ptr | flat C2S table |
| 0x1F4 | `msginfo_flags0` / `disable_processing` / `msginfo_flags2` / `msginfo_flags3` | 4×u8 | |
| 0x1F8 | `callback_ctx` | ptr | |
| 0x1FC | pad | 16 | |
| 0x20C | `msginfo_high_client_index` / `_len` | ptr+u32 | high-bit dispatch index |
| 0x214 | `msginfo_client_index` / `_len` | ptr+u32 | inverted msg_id index |
| 0x21C | `msginfo_high_server_index` / `_len` | ptr+u32 | |
| 0x224 | `msginfo_server_index` / `_len` | ptr+u32 | |
| 0x22C | `pool_client` | 32 | actually a `NapiFifo`, not `NapiNPBufferPool` (Kong mislabel) |
| 0x24C | `pool_server` | 32 | same |
| 0x26C | `cb_client_msg` / `_unhandled` / `_error` / `cb_client_0..3` | 7 ptrs | client-direction callbacks |
| 0x288 | `nstmout_path` | char[64] | actually the **session/server name** (lobby-visible; "HOST STARTED \"%s\"" log) — rename candidate `session_name` |
| 0x2C8 | pad | 8 | two server callbacks set in CreateSession: `CNapiServer_OnPlayerDisconnected`, `CNapiClient_OnDisconnected` |
| 0x2D0 | `cb_server_0..2`, `cb_server_msg` / `_unhandled` / `_error`, `cb_server_3..8` | 12 ptrs | server-direction callbacks |
| 0x300 | `log_buffer` | char[512] | misnamed — compared against the client `PW` TLV in HandleClientJoin; rename candidate `server_password[512]` |
| 0x500 | `server_flags` | u32 | `P1` TLV (server config flag word) |
| 0x504 | `build_flags` | u32 | `P2` TLV (`CNapiServerConfig_BuildFlags`) |
| 0x508-0x51C | `p3_count`..`p8_count` | 6×u32 | `P3`..`P8` TLVs; always zeroed in retail JO (reserved slots) |
| 0x520 | `np_count` | u32 | `NP` TLV (zeroed) |
| 0x524 | `max_players` | u32 | `MP` TLV; clamped 1..251 |
| 0x528 | `npw_count` | u32 | `NPW` TLV (zeroed) |
| 0x52C | `gen_session_seed_flag` | u32 | 1 → regenerate `session_seed_id` at host start |
| 0x530 | `session_seed_id` | u32 | `(GetTickCount + rand) % 900000 + 100000` |
| 0x534 | `host_key` | u32 | `HK` TLV; `NapiNP_GenerateSessionKey()` at StartServer; validated against the client's HK on join (mismatch → result 3) |
| 0x538 | `host_running` | u32 | 1 once StartServer succeeds; HandleClientHello rejects when 0 |
| 0x53C | `host_start_tick` | u32 | GetTickCount at StartServer; uptime base |
| 0x540 | `host_stop_tick` | u32 | GetTickCount at StopServer |
| 0x544 | `host_run_duration_ms` | u32 | stop − start, frozen post-stop |
| 0x548 | `server_user_string1` | char[512] | `SUS1` TLV (populated from a global server-info string) |
| 0x748 | `server_user_string2` | char[512] | `SUS2` TLV (from CGameSession +4056; likely server URL/NF) |
| 0x948 | `server_user_string3` | char[512] | `SUS3` TLV (cleared in retail) |
| 0xB48 | `server_user_string4` | char[512] | `SUS4` TLV (cleared in retail) |
| 0xD48-0xD4C | `unk_0xD48/0xD4C` | 2×u32 | gates a write-flag block |
| 0xD50 | `unk_0xD50` + pad[63] | 64 | NUL-terminated string, emitted as TLV — likely `country_code[64]` (`CN`) |
| 0xD90 | `unk_0xD90` + pad[63] | 64 | likely `timezone[64]` (`TZB`) or `language[64]` (`LNG`) |
| 0xDD0 | `unk_0xDD0` | u32 | TZB DWORD payload? |
| 0xDD4 | `unk_0xDD4` | 112 | actually `opcode_msg_count[14]` + `opcode_msg_bytes[14]` per-opcode stats (indexed by `NapiNPOpcodeInfo.index <= 0xD`; zeroed at Create and StartServer) |
| 0xE44 | `cs_dir1` | 60 | `NapiCSConfig` |
| 0xE80 | `cs_dir0` | 60 | `NapiCSConfig` |
| 0xEBC | `connection_list` | 16 | `NapiListHead` — the list `SendFiltered`/timeouts walk |
| 0xECC | pad | 16 | |
| 0xEDC | `log_netflow` | 80 | `NapiLog` |
| 0xF2C | `unk_0xF2C` | u32 | |
| 0xF30 | `log_condump` | 80 | `NapiLog` |
| 0xF80 | `log_inout` | 80 | `NapiLog` |
| 0xFD0 | `unk_0xFD0` | u32 | connection-lookup cache: packed `{addr,port}[N]` table ptr |
| 0xFD4 | `unk_0xFD4` | u32 | parallel `NapiNPConnection*[N]` ptr |
| 0xFD8 | `unk_0xFD8` | u32 | cache entry count |
| 0xFDC | `unk_0xFDC` | u32 | unknown (capacity / dirty flag?) |

Strongest witnesses for the 0x500 region: `[orig: NapiNPProtocol_SendServerInfoPacket @
0x6204b0]` reads each DWORD in order and emits the matching TLV tag;
`[orig: CNapiGameSession_CreateSession @ 0x4c97c0]` performs the symmetric writes with
explicit 512-byte copies into the SUS buffers. Host-state DWORDs confirmed by StartServer
(`sub_62B5E0`), `StopServer @ 0x62a820`, `NapiNPProtocol_Create @ 0x625a10`,
`NapiNPTimer_GenerateRandomId @ 0x61e533`, and HandleClientJoin's HK validation.

### 6.6 `NapiPingManager` (declared 288 B; **real allocation 116 B**)

`[orig: NapiPingManager_Create @ 0x6303D0]` allocates 116 (0x74) bytes; every witnessed access
is within `0x00..0x73`. The trailing 172 bytes of the declared struct are dead (kept only for
size compatibility). Global instance via `g_napi_np_ctx.ping_manager` (`0xB5DA2C`).
Initializer `[orig: NapiConnection_Init @ 0x6302F0]` (misnamed — only called from Create;
rename candidate `NapiPingManager_Init`).

| Offset | Field | Type | Notes |
|---|---|---|---|
| 0x00 | `mem_mgr_handle` | int | allocator handle |
| 0x04 | `state` | int | −1 stopped, 0 init, 1 paused, 2 running, 3 idle |
| 0x08 | `start_tick_ms` | u32 | |
| 0x0C | `last_active_tick_ms` | u32 | |
| 0x10 | `elapsed_ms` | u32 | |
| 0x14 | `periodicity_ms` | int | **misnamed** — actually the response timeout before retry; default 3000 |
| 0x18 | `field_18` | int | actually `max_retries`; default 2 |
| 0x1C | `max_attempts` | int | **misnamed** — actually the per-entry send throttle in ms; default 1, set to 10 by `CNapiNetwork_Init` |
| 0x20 | `socket_initialized` | int | 1 if `Network_CreateRawSocket` succeeded |
| 0x24 | `socket` | SOCKET | raw UDP socket |
| 0x28 | `last_pump_tick_ms` | u32 | 100 ms pump throttle |
| 0x2C | `last_send_tick_ms` | u32 | backdated by the send throttle at init |
| 0x30 | `thread_running` / `thread_stop_request` + pad | 4 | NapiThread block start |
| 0x34 | `thread_proc` | fn ptr | = `CNapiNPConnection_NetworkThreadProc` |
| 0x38 | `thread_param` | ptr | = this |
| 0x3C-0x48 | `field_3C..48` | 4×u32 | zeroed by `NapiThread_Reset`; no read sites |
| 0x4C | `active_count` | int | entries with state>0; gates thread launch/continue and the idle transition |
| 0x50 | `state3_count` | int | entries with state==3; gates the recvfrom loop |
| 0x54 | `anchor_self` | ptr | back-pointer; entries store `&mgr->anchor_self` as owner |
| 0x58 | `entry_head` | ptr | first `CNapiPingEntry` (also walked by `[orig: Server_SendPingMetricsToGate @ 0x511BF0]`) |
| 0x5C | `entry_tail` | ptr | (single witness; structurally paired) |
| 0x60 | `entry_count` | int | guards metrics emit |
| 0x64 | `callback_ctx` | ptr | `CNapiGateManager *` in practice |
| 0x68 | `callback_event` | fn ptr | fires on entry start AND done; set to `sub_63BC60` |
| 0x6C | `add_jitter_flag` | u8 | fudges RTT by `rand()%15` in `[orig: CNapiNPConnection_HandlePingResponse @ 0x62FC20]` (single witness) |
| 0x70 | `sleep_ms` | int | thread loop sleep; default 5, clamped to 1000 |
| 0x74 | dead pad | 172 | beyond the real allocation |

Per-entry tick logic: `[orig: CNapiPingEntry_ProcessTick @ 0x62F900]` retries after the
response timeout, gives up after `max_retries`, and throttles sends to one per
`send_throttle_ms`. Stale `sub_` names identified: `sub_62FE50` = `NapiPingManager_Start`,
`sub_62FD10` = `NapiPingManager_Pump` (100 ms throttle), `sub_62FE10` =
`NapiPingManager_DestroyEntries`.

### 6.7 `CNapiGateManager` (304 B; methods 0x4ce6b0-0x637b30; 14/14 typed)

| Offset | Field | Type | Notes |
|---|---|---|---|
| 0 | `gate_type` | int | set in InitDefaults from arg |
| 4 | `gate_state` | int | pre-connect cleanup state {−9, −8, −2, −1, 1, 2} |
| 8 | `hostname` | char[24] | default `"gs.novaworld.net"` |
| 32 | `conn_state` | int | state machine [−8..3] in SetState |
| 36 | `state2_enter_tick` | int | GetTickCount at state→2 |
| 40 | `state2_exit_tick` | int | GetTickCount at state 2→other |
| 44 | `state2_duration_ms` | int | |
| 48 | `field_30` | int | semantics unclear (response state / buffer metadata?) |
| 52 | `response_buffer` | ptr | freed when state→0 |
| 56-68 | `field_38/3C/40/44` | 4×int | semantics unclear |
| 72 | `port` | int | default **7597** |
| 76 | `protocol` | char[64] | default **`"jop:cus2"`** (retail JO gate-probe tag; jodemo uses `jopd:cus4`) |
| 140 | pad | 164 | embedded NapiThread + NapiMutex (type when those are declared) |

Size confirmed by the constructor's 0x130 memset. The struct literally hard-codes the
retail gate-probe identity.

### 6.8 `ItemDef` health fields (net-spawn relevant)

The 2780-byte `ItemDef` got two fields lifted out of `pad_17C` during the spawn
investigation:

| Offset | Field | Flows to |
|---|---|---|
| 0x17C | `healthMax` (i16) | `entity[+286]` on creation |
| 0x17E | `armorMax` (i16) | `entity[+288]` |

Witness chain: `[orig: Entity_InitFromItemDef @ 0x49e550]` copies both on entity creation;
`[orig: Player_BuildTag0CInputBody @ 0x42a550]` refuses to serialize player input while
`entity[+286] == 0`; `[orig: Entity_KillByNetId @ 0x43dc10]` and
`[orig: AI_CheckVehicleStuckState @ 0x465480]` clear it on death/stuck;
`[orig: Entity_CalcAverageGroundHeight @ 0x45733f]` uses `<= 0` as a skip-dead guard. The AI
class flag relevant to §5.6 is `ItemDef[+84] & 0x100000`. Open ItemDef follow-ups: `+0x138` →
`entity[+456]` (post-physics handler / model ptr?), `+0x148` init-callback fn ptr (with
recursion guard vs `Entity_InitFromItemDef` itself), `+0x158` → `entity[+452]`, 28 unknown
bytes after `armorMax`.

## 7. Landed architecture

The design that shipped in PR #37, was reverted in PR #50, and is relanded on the
`web-nw-for-real-master` integration branch. `web/` and `apps/novaworld_server/` exist
there (not yet on master).

- **One standalone C++ server binary, three listeners**: gate UDP :7597, HTTP :8080 (the
  `/api/*` routes plus the bundled Vue web portal from `web/dist/`), NW UDP :64206. Shared
  in-memory connection registry; SQLite state at `backend/data/state.db` (schema kept abstract
  enough to swap libpq later). HTTP framework was decided as Drogon but implemented with Crow
  + standalone Asio.
- **Protocol code lives once** in Godot-free libs: `libs/novacrypto` (Layer 2 cipher),
  `libs/napi` (Layer 2-3 framing/session/TLV), `libs/novaworld` (all Layer-4 PN message sets —
  browser/session services and the in-match GameSession runtime — plus connection registry and
  db wrapper). PN dispatch happens inside `libs/novaworld`; the NovaWorld server rejects
  non-`NOVAWORLDUDP` PN, leaving in-match traffic to a (deferred) per-match game server.
- **Godot is the client only**: GDExtension binding under `godot/engine/network/`; servers are
  pure C++ with no Godot dependency. A future Godot admin/stats viewer would talk to the
  standalone server over HTTP, never be the server.
- **Reference equivalence**: `libs/napi/tlv.h::NapiMessage` is byte-for-byte equivalent to the
  Python reference container parser (`onnet/onnw/protocol/container_parser.py`); the wire
  markers are those in §1. The authoritative dissector for opcode/message enumeration is
  `novaworld_udp.lua`.
- State at revert: gate + NW UDP + HTTP listeners, ServerAuth defaults matched to retail
  captures (MI=0x113f, CR=1, NovaworldName="NWServer", 62-char SCRK, 60-char hex NWUID),
  Layer-4 session dispatch, web portal build, DB migrations, and the Godot client demo were
  done; the legacy `NW*.dll` HTTP routes (§2) followed; full retail end-to-end join (spawn
  flow, §5) was still being chased.

### 7.1 Client session state machine (ADR 0010, Phase 1)

The client direction of the session flow is a Godot-free state machine,
`libs/novaworld/client_session.{h,cpp}` — the mirror of `LobbySession`/
`nw_udp_listener.cpp` with request and response inverted. The `NovaWorldClient`
GDExtension binding (`godot/engine/network/`) is now a thin socket pump: the
gate leg yields the NW UDP host:port, then `ClientSession` runs
`HELLO → AUTH → {ClientConnected → ServerStartVerify → ClientRequestVerifyResult →
ServerVerifyResult} → Verified`.

Two client-direction parsers were added (inverses of the existing serializers):
`parse_server_hello` (recovers the host key `HK` the client must echo) and
`parse_server_auth` (recovers `CR`/`SK`/server `SCRK` + the CS/CU control
fields). Phase 1 closed two stubs in the old binding: the hardcoded
`send_session_join(/*server_hk=*/0)` (now echoes `ServerHello.HK` in
`ClientAuth.HK`) and the empty `0x83` handler (now decodes the lobby stream and
drives the verify handshake to `ServerVerifyResult`). The inner-stream crypto is
symmetric: the client encrypts its `0x43` with its own `SCRK`, decrypts inbound
`0x83` with the server's `SCRK`; outbound `session_id` = the server `SK`.

Verified offline by `tests/novaworld/client_session_loopback_test.cpp`, which
drives `ClientSession` against the real server-side parsers/builders +
`LobbySession` in-process and asserts the `HK` echo and the
`Verified`/`SessIdString` outcome. The HELLO and AUTH legs now **have** fresh
IDA witnesses against real NW — the identity gates in `HandleClientHello @
0x6213B0` (NW-S1) and `HandleClientJoin @ 0x62B750` plus the retail `0x42`
builder `NapiNPConnection_SendClientHello @ 0x61fe20` (NW-S2, §8) — confirmed
live: the client reaches `session_join`, and after NW-S2 the join carries the
identity block real NW requires for `ServerAuth`. The verify framing is still
inferred from the container set (§3). Live-smoke past AUTH and the host/join
legs are ADR 0010 Phases 3-5.

## 8. Equivalence verdicts (grill log)

Per-system verdicts from grilling the reimplementation against retail
`Jointops.exe` (Kong IDB). Each row cites the original entry point. Verdict:
**matching** | **divergent → fixed** | **divergent (accepted)** | **unknown**.

### Wave 1 — gate protocol + session envelope (2026-06-11)

| System (reimpl) | Original | Verdict | Notes |
|---|---|---|---|
| Gate response emit (`apps/novaworld_server/gate_listener.cpp::build_gate_response`) | `CNapiGateManager_ProcessResponse @ 0x4ced20` | **matching** | Emits the required POSTIPADDRESS/POSTIPPORT (see NW-G1); the wire shape (`GATEPROTOCOL "1.0"` + `VAR "k" "v"` CRLF lines) is what the retail parser consumes. |
| Gate response parse (`libs/novaworld/gate_response.cpp`) | `CNapiGateManager_ProcessResponse @ 0x4ced20`, tokenizer `String_TokenizeQuotedToArray @ 0x616d60` | **divergent → fixed (×2)** | (1) The parser was missing 6 of retail's 19 keys (LOBBYNAME, USEJUNCTION, CLEARJUNCTION, GLSVSSREQUEST, GLSVSSRIMS, GLSVSSAGRMS) and carried 2 non-retail keys (CUS, PVT). Missing keys added; CUS/PVT kept as flagged tolerant extras. The header's address citation was wrong (`0x4ad330` is `SaveFile_WriteFullState`); corrected to `0x4ced20`. (2) **2026-06-11**: the tokenizer split on whitespace only and left the quotes attached, so the real gate's quoted lines (`VAR "POSTIPADDRESS" "127.0.0.1"`) matched no key (`var_count==0`) and the Godot client rejected every real-NW reply as "bad gate response". Retail's tokenizer `String_TokenizeQuotedToArray @ 0x616d60` toggles on `"` and never copies it (quotes stripped, whitespace inside quotes kept); our `tokenize_line` now mirrors it and uses `tokens[2]` as the value (= retail's `tokenValue`). The envelope was never the issue (the symptom was the post-envelope parse, not "bad gate envelope"). Covered by a quoted-format case in `gate_response_test` + a quoted `gate_server_loopback_test` body. |
| Gate manager defaults (`§6.7` struct) | `CNapiGateManager_InitDefaults @ 0x4d1460` | **matching** | hostname `gs.novaworld.net` @ +8, port 7597 @ +72, tag `jop:cus2` @ +76 — exactly the §6.7 layout. Note: the base `CNapiGateManager_Init @ 0x633f90` defaults to `novaworld.net` @ +64 / port @ +192; the game layer's `InitDefaults` overrides it, so the effective retail gate host is `gs.novaworld.net`. |
| Session HELLO TLV (`libs/novaworld/session_hello.cpp`) | `NapiNPProtocol_HandleClientHello @ 0x6213B0` | **matching** | Flat TLV tag set confirmed: NVS, CO, AP, BDAT, PN (game id), PG (16-byte key), PV1, PV2 — plus retail-only validated/echo tags PV3/PM/CI/EIP/EPN/ET. Retail validates NVS == the Milota version string `"NAPI NP Version 0.0.1 1/12/2004 - 2/20/2004 Milota Copyright 2004 NovaLogic"`, PN == server game id, PG == server key (16 B), PV1 == server build. SESSION NWU key `"asdfj2349857qu23rija;sdlvzx09caweklrj1234hldfj"` @ 0x7DFC50 confirmed. |
| ClientHello identity (`libs/novaworld/client_session.cpp::build_client_hello`) — **client direction** | `HandleClientHello @ 0x6213B0` validation + `CNapiGameSession_InitNPConnection @ 0x4d3be0` constants | **divergent → fixed** | NW-S1 (**2026-06-11**): the Godot client sent `NVS="OpenNova Godot Client 0.1"` and **no PG**. Our own server's `parse_client_hello` doesn't validate, so OpenNova accepted it — but real NW's `HandleClientHello` does the LABEL_68 check: if `NVS != Milota` **or** `PN != game id` **or** the 16-byte `PG != proto+284` **or** `PV1 != proto+300`, it `return 0`s and **sends no ServerInfo/ServerHello** → the client times out in `session_hello`. The client now sends the retail-faithful identity from `InitNPConnection @ 0x4d3be0`: `NVS` = Milota, `PN` = `NOVAWORLDUDP`, `PV1` = `"0.0.0 2/10/2004 EM"`, and `PG` = the 16-byte `NOVAWORLDUDP` protocol GUID built by `sub_62E750 @ 0x62e750` from `(-655487758, 58574, 17549, 144,180,29,66,179,100,171,113)` → bytes `F2 0C EE D8 CE E4 8D 44 90 B4 1D 42 B3 64 AB 71` (`[u32 LE][u16 LE][u16 LE][8B]`). CO/AP/BDAT are read but not validated, kept as our identity. Pinned in `client_session_loopback_test`. (Reference is IDA only — `opennova-int` is server-only and never modeled the client direction.) |
| ClientAuth identity (`libs/novaworld/session_hello.cpp::client_auth_to_bytes` + `client_session.cpp::build_client_auth`) — **client direction** | `HandleClientJoin @ 0x62B750` validation + client builder `NapiNPConnection_SendClientHello @ 0x61fe20` | **divergent → fixed** | NW-S2 (**2026-06-11**): after the NW-S1 hello fix the client advanced `session_hello → session_join` but timed out — real NW never sent `ServerAuth(0x82)`. `HandleClientJoin @ 0x62B750` re-runs the **same** identity gate as the hello and silently `return 0`s (no ServerAuth) unless `NVS==Milota && PN==proto+220 && PG==proto+284(16 B) && PV1==proto+300`; its `is_server` branch additionally requires `HK==proto+1332` (the echo), `PV2==proto+364` (`"1"`), and a non-empty `NA`. Our `client_auth_to_bytes` emitted **only** `CI/HK/CK/NA/SIP/SPN/SCRK/CU` — the entire identity block was missing, so the gate failed. Retail's own `0x42` builder is `NapiNPConnection_SendClientHello @ 0x61fe20` (a Kong **misnomer** — `packet_type=66='B'`=0x42, not the hello), which emits `NVS/CO/AP/BDAT/[DE]/PN/PG/PV1/PV2/[PV3]` ahead of `CI/HK/CK/NA/[PW]/SIP/SPN/CU/SCRK/[NF/DCNT/RCNT]`, identity sourced from `CNapiGameSession_InitNPConnection @ 0x4d3be0` (`CO="NovaLogic Inc, Calabasas CA U.S.A."`, `BDAT="Jul 21 2009 18:54:41"`, `PV2="1"` @ +364). The fix emits the identity block in retail order (each tag gated on non-empty/non-zero, as retail does), reusing the same `Config` values that already pass the hello gate. `parse_client_auth` made symmetric. Pinned in `client_session_loopback_test` (identity-block assertions) + a `client_auth_to_bytes`↔`parse_client_auth` round-trip in `session_hello_roundtrip_test`. (Reference is IDA only — `opennova-int` is server-only.) Proposed IDB rename recorded: `0x61fe20 → NapiNPConnection_SendClientJoin`. |
| Session containers (`libs/novaworld/lobby_session.cpp`) | NOVAWORLDUDP dispatch (§3) | **matching (spot-checked)** | All ten containers dispatched with the documented replies (§3); covered by `lobby_session_test`. Field-for-field read order vs retail handlers deferred to a wave-1 follow-up where it matters for a specific reply. |

#### NW-S3 — the 0x42 join is protocol-complete; real NW gates acceptance on an authenticated session

After NW-S2 the client still times out in `session_join` against live NW. Grilling the
server-side accept path (retail `Jointops.exe`, which contains the host/server code) shows
**the protocol handshake itself is now correct and would be answered on the first 0x42** — the
remaining gate is account/session authentication that lives in the NW *server's* callbacks,
which are not in this binary:

- `HandleClientHello @ 0x6213B0` replies via `NapiNPProtocol_SendServerInfoPacket @ 0x6204b0`
  with **opcode 0x81** (so our `parse_server_hello` on 0x81 is right). It writes the `HK` tag =
  `proto->host_key` at **offset 0x534 (=1332)**. `HandleClientHello`'s version gate is *identical*
  to `HandleClientJoin`'s (NVS/PN/PG/PV1) — and our `0x41` already passes it (we receive the
  ServerInfo), which independently proves our flat-TLV encoding and NVS/PN/PG/PV1 values are
  correct.
- `HandleClientJoin @ 0x62B750` echo-checks the client `HK` against `proto[333]` = `proto+1332`
  = the same `host_key` it advertised. So **the HK echo is sound** (we read and re-send it). PV2
  is checked vs `proto+364` (`"1"`, a protocol constant despite the `max_players_string` Kong
  name) and NA must be non-empty — both satisfied.
- A *fresh, accepted* join sends `ServerSessionInit` **inline on the first 0x42**, no retransmit:
  `NapiNPConnection_OnStateChange @ 0x626060` (state 1) calls `cb_server_1` (`proto+724`) and, if
  it returns ≥ 0, `NapiNPConnection_SendSessionInit @ 0x620ef0`. Earlier, `HandleClientJoin`
  itself runs `cb_server_0` (`proto+720`); on `< 0` it destroys the connection and stores the
  reply tag **`NP.C:PCCR:ILC`** ("Illegal Login Callback"). These two callbacks are the
  account-auth gate and live in the NW **server** binary — not witnessable here.
- The retail client carries the auth/session context the server expects:
  `CNapiGameSession_ConnectToNovaWorld @ 0x4d4640` builds the connection's CU var list via
  `CNapiVarList_SetOrCreate` — **`Application`, `BuildDateAndTime`, `Debug`, `CountryName`,
  `Language`, `TimeZoneBias`, `GateTag`, `MetTag`, `UdpCode1`, `UdpCode2`, `MaxPacketSize`** —
  which `NapiNPConnection_SendClientHello @ 0x61fe20` emits as `CU` chunks in the 0x42. `MetTag`
  (`byte_B5F4BC`) is set by the **gate handler** `CNapiGateManager_ProcessResponse @ 0x4ced20`,
  and `InitNPConnection @ 0x4d3be0` pulls login cookies (`CookieJar_GetCookiesForURL`). Our client
  sends **no CU chunks** and holds **no authenticated session**.

**Verdict: unknown → blocked on auth.** The bare NP handshake (gate → hello → join) is necessary
but not sufficient for live NW: the matchmaking server rejects an unauthenticated join in
`cb_server_0`/`cb_server_1`. Completing AUTH requires the **HTTP account login (ADR 0010 Phase 3,
NWLogin/EPASK)** — which establishes the session cookie and the `MetTag`/`UdpCode*` values — plus
emitting the retail CU-chunk set in the 0x42. This **reorders the ADR**: login is a *prerequisite*
for completing the UDP AUTH, not a post-connect step. (`OnNovaWorldConnected @ 0x4d1570` fires
*after* SessionInit and fills a separate in-game credential form; that is distinct from the
pre-connect web login that authorizes the join.)

**The auth chain, end to end (de-risk grill, 2026-06-11):**
1. `UDPCODE1`/`UDPCODE2` — the session-auth codes the 0x42 join carries as `UdpCode1`/`UdpCode2`
   CU chunks — are **gate-response VAR keys**, parsed in `ProcessResponse @ 0x4ced20`
   (`UDPCODE1 → CMissionInfo_SetGateTag`, `UDPCODE2 → CMissionInfo_SetMetTag`). Our
   `gate_response.cpp` already parses both. So they come from the **gate**, not a separate endpoint.
2. The gate issues them only to an **authenticated** request. Authentication is a **web-form login**
   through the in-game browser (`CUIBrowser` @ `dword_2551100`): `load_persistent_login_credentials
   @ 0x557330` fills the `NAME` (username) + password fields; persisted creds live in
   `PERSISTENTREMEMBERLOGINDATA`, decrypted by `NapiNP_DecodeEncryptedKeyValue @ 0x619470` with key
   `"SLHALI289SZ79210987ZS:OCV789YHK2QJ3HSKDJHVS978THYG23"`. (EPASK crypto = NW-C2.)
3. So **live-NW Phase 3 = web login → gate issues `UDPCODE1/2` → emit them (+ env vars) as CU chunks
   in the 0x42.** Our gate parser already captures `UDPCODE1/2`; the missing pieces are (a) the web
   login that makes the gate issue them and (b) attaching the CU-chunk set to `ClientAuth`.

**Scope note — product vs parity.** This entire auth chain is required only to impersonate a retail
client against **live NovaLogic NW** (parity testing). The OpenNova **product** path is OpenNova
client ↔ the OpenNova server (`apps/novaworld_server`), whose `cb_server_0`/`cb_server_1` are
permissive — there the full handshake already reaches `Verified` (`client_session_loopback_test`).
The `session_join` timeout is therefore expected against live NW and is **not** a blocker for the
OpenNova-server path; it gates only the retail-impersonation parity scenario.

Empirical ground truth available cheaply: retail JO logs the entire gate dialogue to
`_connectlog.txt` (every `GATE LINE #NN [...]`, set by `ProcessResponse`'s `g_ConnectLogEnabled`
path) when launched with `/connectlog`. One retail launch against the same live endpoint captures
the exact VAR set the gate returns (incl. whether `UDPCODE1/2` are present) and resolves any
remaining unknowns (`UdpCode1/2` source `byte_B5F8E8`/`B5F908` had no writer xref).

#### NW-G1 — POSTIPADDRESS / POSTIPPORT are required (resolved)

The standing question (jodemo requires them; onnet omits them yet works on retail JO)
is resolved by `CNapiGateManager_ProcessResponse @ 0x4ced20`. After tokenizing the
response, retail:

1. fails to state `-9` if `parsedFieldCount == 0` (no recognized VAR line);
2. computes a junction/direct-connect bypass `dword_B5FD2C` from command-line flags
   (`dword_B5F928`, `dword_B4C71C`, overridable by `dword_829F88`);
3. **unless that bypass is set**, fails to `-9` with "NO NW POST IP" if `dword_B5F490`
   (POSTIPADDRESS) is unset, then "NO NW POST PORT" if `dword_B5F494` (POSTIPPORT) is
   unset.

So retail genuinely requires both VARs on the normal (non-junction) path — the same as
jodemo. onnet's omission was a bug; the PR #37 stack adding them
(`gate_listener.cpp`) was the correct fix and is what we reland. Command-line overrides
(`dword_B4C6B0`/`dword_B4C6B4`) can supply the POST IP/port directly, which is the only
way the fields become optional.

#### NW-L2 — NovaLogic hostnames the client contacts (launcher hosts set)

String sweep of retail `Jointops.exe` for the launcher's redirect set:

| String | Where | Role | Redirect? |
|---|---|---|---|
| `gs.novaworld.net` @ 0x7cc368 | `CNapiGateManager_InitDefaults @ 0x4d1460` (+8) | **effective gate host**, port 7597 | **yes** (the one the launcher must map) |
| `novaworld.net` @ 0x7e052c | `CNapiGateManager_Init @ 0x633f90` (+64) | base-default gate host, overridden by InitDefaults | optional (belt-and-braces) |
| `http://www.novaworld2.com` / `.../patch/%d` @ 0x7dc144/0x7dc160 | `sub_5C7240` | patch / update URL | **no** (outside matchmaking; we do not manage patches) |
| `http://%s` @ 0x7cc3e8 | `CNapiGameSession_OnNovaWorldConnected @ 0x4d1570` | startup-URL format (domain comes from the gate response) | n/a (not a hostname) |

Conclusion: redirecting **`gs.novaworld.net`** is sufficient for the JO matchmaking flow;
adding `novaworld.net` is harmless belt-and-braces. The launcher's server-driven
redirect set (default `["gs.novaworld.net"]`) is correct; `novaworld.net` can be added
server-side without a launcher release. DFX2's gate host (`dfx2:0:cus:buffy` tag) is
grill item NW-L1, deferred to wave 2 (needs the dfx2.exe IDB).

### Wave 2 — server-browser format (partial, 2026-06-11)

| System (reimpl) | Original | Verdict | Notes |
|---|---|---|---|
| GSB builder (`libs/novaworld/gsb.cpp`) | retail GSB chunk strings | **matching (retail)** | NW-G2: retail `Jointops.exe` contains the `SVRS` (@0x63d793) and `FLDS` (@0x63d7b1) chunk strings and lacks `GLB `/`PLYR`. Our builder emits `IVAR`/`FLDS`/`SVRS`/`XXXX` — the GSB format — so it matches the **retail** client. |
| GSB parser (`libs/novaworld/gsb.cpp::gsb_parse_response`) | retail GSB chunk-tag pool `SVRS`/`GSB `/`FLDS` @0x63d793–0x63d7b1 | **matching (retail)** | NW-G3: client-direction inverse of the anchored builder (ADR 0010 Phase 2). The retail response header `"GSB "` is at 0x63d7a5, adjacent to the parser's tag pool; our parser checks the same header, decrypts each chunk with the SUBTRACT chain, and reads rows positionally against the SVRS field table. Verified by `gsb_parse_roundtrip` (build→parse is byte-faithful). |
| GSB **request** URL (client GET) | *(no binary literal)* | **matching by construction (OpenNova)** | NW-G3: `.gsb` / `jop_2.gsb` / `GSB_SERVER` / `?a=1` are **absent** as string literals in retail `Jointops.exe`. The client does not construct the GSB path/query — it issues a plain HTTP GET of a URL handed to it at runtime in the server-sent menu (the `GSB_SERVER` template substitution our `http_listener` produces). For OpenNova we control that URL, so the request matches by construction. |

#### NW-G2 — GSB (retail) vs GLB (demo) is a per-binary split

The standing disagreement (the Phase C.2 note records the browser format as
`GLB `/`FIEL`/`DATA`/`PLYR`, "NOT onnet's `GSB `/`SVRS`/`FLDS`") resolves as a
**per-binary difference**, not a bug: that GLB layout was witnessed against
*jodemo* (the Joint Operations demo), while retail `Jointops.exe` (JO:CA, the
deploy target) uses the GSB chunk format our `gsb.cpp` already emits. String
evidence in the retail image: `SVRS`, `FLDS` present; `GLB `, `PLYR` absent.

Open: a definitive jodemo-side GLB spec (chunk-by-chunk) needs the jodemo IDB
and, if demo support is wanted, a separate demo GSB/GLB builder. Deferred with
NW-L1 (the DFX2 gate hostname, also a different-binary item) to a wave-2
follow-up that loads those IDBs. The retail path — the one that matters for the
deploy target — is matching.

#### NW-G3 — GSB client parser + request (ADR 0010 Phase 2)

Two client-direction findings, grilled while landing the Godot client's server
browser:

1. **Response parser.** `gsb_parse_response` (`libs/novaworld/gsb.cpp`) is the
   exact inverse of the anchored `gsb_build_response`: it checks the `"GSB "`
   header (retail @0x63d7a5, adjacent to the parser's chunk-tag pool `SVRS`
   @0x63d793 / `FLDS` @0x63d7b1), decrypts each chunk payload with the SUBTRACT
   chain (our `nwu_encrypt`) under `GSB_NWU_KEY`, and reads each server row
   positionally against the field-name table in the SVRS(fields) chunk (lookup
   is case-insensitive, as the retail browser folds case). The GSB row carries
   only `rid` + the four IPv4 octets — **no port** — so the parser leaves
   `port = 0` (host:port for the actual join arrives via a later leg). Locked by
   `gsb_parse_roundtrip` (build→parse byte-faithful). An anchoring IDA comment
   is on 0x63d7a5.

2. **Request URL.** A string sweep of retail `Jointops.exe` for `.gsb`,
   `jop_2.gsb`, `GSB_SERVER`, and `?a=1` finds **none of them** — the only GSB
   string in the image is the `"GSB "` response header. So the client does not
   build the browser URL: it issues a plain HTTP GET of a URL supplied at
   runtime by the server, the `GSB_SERVER` template/menu substitution our
   `http_listener` already produces (`http://<host>:<port>/jop_2.gsb`). The
   `?a=1` seen in captures is part of that server-provided URL, passed through
   verbatim. For OpenNova we own both ends of that URL, so the request is
   **matching by construction**; our Godot client GETs the OpenNova GSB URL
   (derived from the configured host + `/jop_2.gsb`). **Open (real-NW only):**
   whether the engine's shared HTTP client attaches the login cookies
   (`NWHANDLE`/`PCID`) to the GSB fetch is coupled to the login flow and is
   carried into Phase 3 (EPASK login); OpenNova's `handle_gsb` ignores cookies.

### Wave 3 — login/session crypto (2026-06-11)

| System (reimpl) | Original | Verdict | Notes |
|---|---|---|---|
| NWU cipher (`libs/novacrypto/src/nwu.cpp`) | `NapiNP_EncryptBuffer @ 0x6187b0` / `NapiNP_DecryptBuffer @ 0x618880` (retail) | **matching (byte-exact, retail)** | NW-C1: re-grilled against retail (was jodemo-anchored only). All six primitives + the seed + the 3-step derive + the 4-phase order match. Name-swap confirmed at byte level. Fixed a doc bug: the LCG multiplier `78665521` is `0x04B05731`, not `0x04B02631` as commented. |
| EPASK login-form encrypt (`libs/novacrypto/src/epask.cpp`) | `sub_6669A0 @ 0x6669a0` (core) ← edit-widget vtable `+0x38` `build_form_field_query_string @ 0x657760` ← `build_url_and_submit_request @ 0x63e3f0` | **matching (byte-exact, retail)** | NW-C2: polymorphic dispatch resolved. Core = NWU-add → modexp `pow(byte+2,exp,mod)` 4-byte LE (`sub_666600 @ 0x666600`) → NWU-add → A-P low-first (`NapiNP_EncodeToHexAlpha @ 0x666570`); `exp:mod:key` split (`parse_colon_delimited_string @ 0x666710`); the NWU copy `NapiNP_EncryptBufferAlt @ 0x6668e0` is byte-identical to `0x6187b0`. Golden vectors equal the test's own ciphertext fixtures. |
| PUBcrypto `PUB*` join fields (`libs/novacrypto/src/pubcrypto.cpp`) | `NapiNP_EncryptAndEncodeToHexAlpha @ 0x618fd0` (encode) / `NapiNP_DecodeEncryptedString @ 0x619130` (decode) | **matching (byte-exact, retail)** | NW-C3: PUB encode = CRC32-append (`NapiNP_ComputeCRC @ 0x618770`, MPEG-2) → NWU-encrypt → A-P. Single-key path == `encode_pub_value`; because the encrypt step *is* `0x6187b0`, this proves `ticket_transform` == NWU. The colon-key multi-layer form is the Python remember-cookie (out of our scope). |
| url_cipher `NK`/`CK` join tokens (`libs/novacrypto/src/url_cipher.cpp`) | `parse_connection_query_string @ 0x54dfb0` | **matching (byte-exact, retail)** | NW-C4: `plain[i] = cipher[i] - key[i] + '0'`, `'&'`(38)-terminated; keys NK@`0x7d3f30` `"diheijefhgcdjcgcjcfbd"`, CK@`0x7d3f04` `"cfhdcegjigecjehcgjdhe"` (jodemo: `Auth_ParseRegistrationURL @ 0x514c40`, keys `0x74d8a0`/`0x74d874`). `BK` is the literal `"986119"`, not a cipher. |

#### NW-C1 — NWU cipher is byte-exact in retail (resolved)

The core keyed cipher under EPASK, PUBcrypto, GSB, the gate, and session payloads.
Verified primitive-by-primitive against retail `Jointops.exe`:

- `NapiNP_ComputeKeySeed @ 0x618430` — `null → 3252`; `Σ(i + key[i]²) + len + 50`, key
  bytes signed. Exact match to `nwu_compute_seed`.
- `NapiPRNG_Init @ 0x62e430` — LCG struct `{state@0, multiplier@4, counter@8}`, multiplier
  `78665521` (`0x04B05731`); only the low 16 bits (`0x5731`) participate.
- `Crypto_AddWithKey @ 0x6182d0` — `buf[i] += key[i % klen]` (ADD), so retail
  *EncryptBuffer* is the ADD chain = our `nwu_decrypt`. Confirms the name-swap.
- `Crypto_AddProgressive @ 0x618250` — `buf[i] += seed+i; seed += step`.
- `Crypto_AddLCG @ 0x6183b0` — `state = u16(mult·state + 1); buf[i] += state`.
- `NapiNP_ReverseBuffer @ 0x618210` — `len>>1` front/back swaps.

The three NWU keys are all confirmed against retail: gate `"GATEAPI"`, GSB
`"3209452104342624532341"`, and session-payload (opcode 0x47/0x87)
`"asdfj2349857qu23rija;sdlvzx09caweklrj1234hldfj"` @ `0x7DFC50`. Verdict: matching.

#### NW-C2 — EPASK login-form encrypt is byte-exact in retail (resolved)

The client login submit (`build_url_and_submit_request @ 0x63e3f0`) reads the server-issued
`EPASK` cookie (`exp:mod:key`), forms `<url>?EPASK=<key>`, and dispatches each form field
through the widget vtable `+0x38`. For the text/password EDIT widget that slot is
`build_form_field_query_string @ 0x657760`, which sizes its output at `8·len`
(= modexp ×4 · A-P ×2) and calls the EPASK core `sub_6669A0 @ 0x6669a0`:

1. `NapiNP_EncryptBufferAlt @ 0x6668e0` — NWU ADD chain (key-add, reverse, progression-add,
   LCG-add; multiplier `0x5731`, reverse-flag mult `0x31`), byte-identical to `0x6187b0`.
2. `sub_666600 @ 0x666600` — per byte, `modular_exponentiation(byte + 2, exp, mod)`
   (`@ 0x666470`) stored as a 32-bit little-endian word. The `+2` and 4-byte expansion match
   `epask.cpp::modexp_encrypt` exactly (guard: `modulus > 258`).
3. `NapiNP_EncryptBufferAlt` again on the expanded buffer.
4. `NapiNP_EncodeToHexAlpha @ 0x666570` — A-P low-nibble-first (`'A'+lo` then `'A'+hi`).

`exp:mod:key` is split by `parse_colon_delimited_string @ 0x666710` (== `epask_from_string`).
The brute-force modexp *decrypt* is server-side (absent from the client); our `epask_decrypt`
is that server half. Verdict: matching.

#### NW-C3 — PUBcrypto `PUB*` fields are byte-exact in retail (resolved)

`NapiNP_EncryptAndEncodeToHexAlpha @ 0x618fd0` is `encode_pub_value` for a single key:
append `NapiNP_ComputeCRC @ 0x618770` (CRC-32/MPEG-2: init `-1`, MSB-first, no final xor,
table `dword_849938`) little-endian, then `NapiNP_EncryptBuffer @ 0x6187b0` (NWU), then A-P
low-first; `NapiNP_DecodeEncryptedString @ 0x619130` is the inverse. Because the encrypt step
*is* `0x6187b0`, this proves `pubcrypto.cpp::ticket_transform` == NWU (the Python
`_ticket_transform` is an inlined NWU copy). The colon-separated multi-key form of `0x618fd0`
is the Python remember-cookie, which we do not port. Verdict: matching.

#### NW-C4 — url_cipher `NK`/`CK` tokens are byte-exact in retail (resolved)

`parse_connection_query_string @ 0x54dfb0` decodes the join-redirect query: `NK=`/`CK=` run
through `plain[i] = cipher[i] - key[i] + '0'`, stopping at the first `'&'` (38); `NI`/`NP`/`BK`/`LN`/`GS`
are copied verbatim. Keys (retail): NK `"diheijefhgcdjcgcjcfbd"` @ `0x7d3f30`, CK
`"cfhdcegjigecjehcgjdhe"` @ `0x7d3f04` — byte-identical to `url_cipher.h` (jodemo had them at
`0x74d8a0`/`0x74d874` under `Auth_ParseRegistrationURL @ 0x514c40`). `url_cipher_decode`
matches; `url_cipher_encode` is the inverse used server-side. `BK` is the constant `"986119"`,
not a cipher. Verdict: matching.

All of wave 3 (NW-C1..C4) is now matching byte-exact against retail; equivalence was
additionally proven by compiling the actual `libs/novacrypto` sources and byte-comparing to
the production-proven `opennova-int` Python on golden vectors, plus an adversarial
from-scratch re-derivation.
